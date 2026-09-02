#pragma once
// TensorVar: a tensor with optional gradient tracking via the Tape.
// This is the primary user-facing type for all NN computations.
// Ops (+, *, matmul, relu, etc.) record backward closures on the global Tape.
// Uses ts::DefaultComputationPolicy::gemm for matmul (cache-blocked BLAS).
#include <cmath>
#include <stdexcept>
#include <containers/tensor/tensor.hpp>
#include "tape.hpp"

namespace manas::nn {
    // ─── TensorVar ────────────────────────────────────────────────────────────────
    // Value + tape_id. Lightweight — copies are cheap (tensor is ref-counted via its storage).
    struct TensorVar {
        Tensor data;
        uint32_t tape_id = Tape::kNoGrad;

        explicit TensorVar(Tensor t, bool requires_grad = false)
            : data(std::move(t)) {
            if (requires_grad) {
                tape_id = Tape::current().push(true, data);
            }
        }

        TensorVar() = default;

        bool requires_grad() const noexcept { return tape_id != Tape::kNoGrad; }

        // Access accumulated gradient after backward()
        const Tensor& grad() const {
            if (tape_id == Tape::kNoGrad) throw std::runtime_error("TensorVar: no gradient");
            return Tape::current().node(tape_id).grad;
        }

        void zero_grad() {
            if (tape_id == Tape::kNoGrad) return;
            auto& g = Tape::current().node(tape_id).grad;
            for (size_t i = 0; i < g.size(); ++i) g.data()[i] = 0.0f;
        }

        const ts::TensorShape& shape() const noexcept { return data.shape(); }
        size_t size() const noexcept { return data.size(); }
    };

    // ─── Core differentiable ops ──────────────────────────────────────────────────

    // Element-wise addition: z = a + b
    inline TensorVar add(const TensorVar& a, const TensorVar& b) {
        auto& tape = Tape::current();
        const size_t n = a.data.size();

        Tensor out_data(a.shape());
        for (size_t i = 0; i < n; ++i) out_data.data()[i] = a.data.data()[i] + b.data.data()[i];

        TensorVar out(std::move(out_data));
        if (tape.recording()) {
            uint32_t a_id = a.tape_id, b_id = b.tape_id;
            out.tape_id = tape.push(true, out.data,
                                    [&tape, a_id, b_id](const Tensor& g) {
                                        if (a_id != Tape::kNoGrad) tape.accumulate_grad(a_id, g);
                                        if (b_id != Tape::kNoGrad) tape.accumulate_grad(b_id, g);
                                    });
        }
        return out;
    }

    // Element-wise multiply: z = a * b
    inline TensorVar mul(const TensorVar& a, const TensorVar& b) {
        auto& tape = Tape::current();
        const size_t n = a.data.size();

        Tensor out_data(a.shape());
        for (size_t i = 0; i < n; ++i) out_data.data()[i] = a.data.data()[i] * b.data.data()[i];

        TensorVar out(std::move(out_data));
        if (tape.recording()) {
            uint32_t a_id = a.tape_id, b_id = b.tape_id;
            // Capture data copies for backward
            Tensor a_data_copy = a.data, b_data_copy = b.data;
            out.tape_id = tape.push(true, out.data,
                                    [&tape, a_id, b_id,
                                        a_copy = std::move(a_data_copy), b_copy = std::move(b_data_copy)](
                                    const Tensor& g) {
                                        if (a_id != Tape::kNoGrad) {
                                            Tensor ga(g.shape());
                                            for (size_t i = 0; i < g.size(); ++i)
                                                ga.data()[i] = g.data()[i] * b_copy.
                                                    data()[i];
                                            tape.accumulate_grad(a_id, ga);
                                        }
                                        if (b_id != Tape::kNoGrad) {
                                            Tensor gb(g.shape());
                                            for (size_t i = 0; i < g.size(); ++i)
                                                gb.data()[i] = g.data()[i] * a_copy.
                                                    data()[i];
                                            tape.accumulate_grad(b_id, gb);
                                        }
                                    });
        }
        return out;
    }

    // Matrix multiply: z = A @ B  (2D tensors)
    // Uses cache-blocked gemm from ts::DefaultComputationPolicy
    inline TensorVar matmul(const TensorVar& A, const TensorVar& B) {
        auto& tape = Tape::current();
        const auto& sa = A.shape();
        const auto& sb = B.shape();
        if (sa.size() != 2 || sb.size() != 2 || sa[1] != sb[0])
            throw std::invalid_argument("matmul: incompatible shapes");

        Tensor C({sa[0], sb[1]});
        ts::DefaultComputationPolicy::gemm<float>(1.0f, A.data, B.data, 0.0f, C);

        TensorVar out(std::move(C));
        if (tape.recording()) {
            uint32_t a_id = A.tape_id, b_id = B.tape_id;
            Tensor A_copy = A.data, B_copy = B.data;
            out.tape_id = tape.push(true, out.data,
                                    [&tape, a_id, b_id,
                                        Ac = std::move(A_copy), Bc = std::move(B_copy)](const Tensor& g) {
                                        // dA = g @ B^T
                                        if (a_id != Tape::kNoGrad) {
                                            auto Bt = ts::DefaultComputationPolicy::transpose(Bc);
                                            Tensor dA({g.shape()[0], Bt.shape()[1]});
                                            ts::DefaultComputationPolicy::gemm<float>(1.0f, g, Bt, 0.0f, dA);
                                            tape.accumulate_grad(a_id, dA);
                                        }
                                        // dB = A^T @ g
                                        if (b_id != Tape::kNoGrad) {
                                            auto At = ts::DefaultComputationPolicy::transpose(Ac);
                                            Tensor dB({At.shape()[0], g.shape()[1]});
                                            ts::DefaultComputationPolicy::gemm<float>(1.0f, At, g, 0.0f, dB);
                                            tape.accumulate_grad(b_id, dB);
                                        }
                                    });
        }
        return out;
    }

    // Add bias (broadcast over batch): z = A + b, A:[N,F], b:[F]
    inline TensorVar add_bias(const TensorVar& A, const TensorVar& b) {
        auto& tape = Tape::current();
        const size_t n = A.shape()[0], f = A.shape()[1];

        Tensor out_data({n, f});
        for (size_t i = 0; i < n; ++i)
            for (size_t j = 0; j < f; ++j)
                out_data({i, j}) = A.data({i, j}) + b.data({j});

        TensorVar out(std::move(out_data));
        if (tape.recording()) {
            uint32_t a_id = A.tape_id, b_id = b.tape_id;
            out.tape_id = tape.push(true, out.data,
                                    [&tape, a_id, b_id, n, f](const Tensor& g) {
                                        if (a_id != Tape::kNoGrad) tape.accumulate_grad(a_id, g);
                                        if (b_id != Tape::kNoGrad) {
                                            Tensor db({f});
                                            for (size_t j = 0; j < f; ++j) db.data()[j] = 0.0f;
                                            for (size_t i = 0; i < n; ++i)
                                                for (size_t j = 0; j < f; ++j)
                                                    db.data()[j] += g({i, j});
                                            tape.accumulate_grad(b_id, db);
                                        }
                                    });
        }
        return out;
    }

    // Scale by scalar: z = a * s
    inline TensorVar scale(const TensorVar& a, float s) {
        auto& tape = Tape::current();
        const size_t n = a.data.size();
        Tensor out_data(a.shape());
        for (size_t i = 0; i < n; ++i) out_data.data()[i] = a.data.data()[i] * s;

        TensorVar out(std::move(out_data));
        if (tape.recording()) {
            uint32_t a_id = a.tape_id;
            out.tape_id = tape.push(true, out.data,
                                    [&tape, a_id, s](const Tensor& g) {
                                        if (a_id != Tape::kNoGrad) {
                                            Tensor ga(g.shape());
                                            for (size_t i = 0; i < g.size(); ++i) ga.data()[i] = g.data()[i] * s;
                                            tape.accumulate_grad(a_id, ga);
                                        }
                                    });
        }
        return out;
    }

    // Sum all elements: scalar TensorVar
    inline TensorVar sum_all(const TensorVar& a) {
        auto& tape = Tape::current();
        float s = 0.0f;
        for (size_t i = 0; i < a.data.size(); ++i) s += a.data.data()[i];
        Tensor out_data({1}, {s});

        TensorVar out(std::move(out_data));
        if (tape.recording()) {
            uint32_t a_id = a.tape_id;
            size_t n = a.data.size();
            ts::TensorShape ashape = a.shape();
            out.tape_id = tape.push(true, out.data,
                                    [&tape, a_id, n, ashape](const Tensor& g) {
                                        if (a_id != Tape::kNoGrad) {
                                            Tensor ga(ashape);
                                            float gv = g.data()[0];
                                            for (size_t i = 0; i < n; ++i) ga.data()[i] = gv;
                                            tape.accumulate_grad(a_id, ga);
                                        }
                                    });
        }
        return out;
    }

    // Mean: scalar
    inline TensorVar mean_all(const TensorVar& a) {
        auto out = sum_all(a);
        return scale(out, 1.0f / static_cast<float>(a.data.size()));
    }

    // ReLU: z = max(0, x)
    inline TensorVar relu(const TensorVar& a) {
        auto& tape = Tape::current();
        const size_t n = a.data.size();
        Tensor out_data(a.shape());
        for (size_t i = 0; i < n; ++i) out_data.data()[i] = a.data.data()[i] > 0.0f ? a.data.data()[i] : 0.0f;

        TensorVar out(std::move(out_data));
        if (tape.recording()) {
            uint32_t a_id = a.tape_id;
            Tensor mask(a.shape());
            for (size_t i = 0; i < n; ++i) mask.data()[i] = a.data.data()[i] > 0.0f ? 1.0f : 0.0f;
            out.tape_id = tape.push(true, out.data,
                                    [&tape, a_id, m = std::move(mask)](const Tensor& g) {
                                        if (a_id != Tape::kNoGrad) {
                                            Tensor ga(g.shape());
                                            for (size_t i = 0; i < g.size(); ++i)
                                                ga.data()[i] = g.data()[i] * m.data()[
                                                    i];
                                            tape.accumulate_grad(a_id, ga);
                                        }
                                    });
        }
        return out;
    }

    // Sigmoid: z = 1 / (1 + exp(-x))
    inline TensorVar sigmoid(const TensorVar& a) {
        auto& tape = Tape::current();
        const size_t n = a.data.size();
        Tensor out_data(a.shape());
        for (size_t i = 0; i < n; ++i) out_data.data()[i] = 1.0f / (1.0f + std::exp(-a.data.data()[i]));

        Tensor sig_copy = out_data; // copy before move
        TensorVar out(std::move(out_data));
        if (tape.recording()) {
            uint32_t a_id = a.tape_id;
            out.tape_id = tape.push(true, out.data,
                                    [&tape, a_id, sc = std::move(sig_copy)](const Tensor& g) {
                                        if (a_id != Tape::kNoGrad) {
                                            Tensor ga(g.shape());
                                            for (size_t i = 0; i < g.size(); ++i) {
                                                float sv = sc.data()[i];
                                                ga.data()[i] = g.data()[i] * sv * (1.0f - sv);
                                            }
                                            tape.accumulate_grad(a_id, ga);
                                        }
                                    });
        }
        return out;
    }

    // Tanh
    inline TensorVar tanh_op(const TensorVar& a) {
        auto& tape = Tape::current();
        const size_t n = a.data.size();
        Tensor out_data(a.shape());
        for (size_t i = 0; i < n; ++i) out_data.data()[i] = std::tanh(a.data.data()[i]);

        Tensor tanh_copy = out_data; // copy before move
        TensorVar out(std::move(out_data));
        if (tape.recording()) {
            uint32_t a_id = a.tape_id;
            out.tape_id = tape.push(true, out.data,
                                    [&tape, a_id, tc = std::move(tanh_copy)](const Tensor& g) {
                                        if (a_id != Tape::kNoGrad) {
                                            Tensor ga(g.shape());
                                            for (size_t i = 0; i < g.size(); ++i) {
                                                float tv = tc.data()[i];
                                                ga.data()[i] = g.data()[i] * (1.0f - tv * tv);
                                            }
                                            tape.accumulate_grad(a_id, ga);
                                        }
                                    });
        }
        return out;
    }

    // GELU: x * Φ(x) ≈ x * 0.5 * (1 + tanh(sqrt(2/π) * (x + 0.044715*x^3)))
    inline TensorVar gelu(const TensorVar& a) {
        auto& tape = Tape::current();
        constexpr float kSqrt2OverPi = 0.7978845608028654f;
        constexpr float kCoeff = 0.044715f;
        const size_t n = a.data.size();

        Tensor out_data(a.shape());
        Tensor cache(a.shape()); // store tanh values for backward
        for (size_t i = 0; i < n; ++i) {
            float x = a.data.data()[i];
            float inner = kSqrt2OverPi * (x + kCoeff * x * x * x);
            float t = std::tanh(inner);
            cache.data()[i] = t;
            out_data.data()[i] = 0.5f * x * (1.0f + t);
        }

        TensorVar out(std::move(out_data));
        if (tape.recording()) {
            uint32_t a_id = a.tape_id;
            Tensor x_copy = a.data, tc = std::move(cache);
            out.tape_id = tape.push(true, out.data,
                                    [&tape, a_id, xc = std::move(x_copy), tanh_v = std::move(tc)](const Tensor& g) {
                                        if (a_id != Tape::kNoGrad) {
                                            Tensor ga(g.shape());
                                            for (size_t i = 0; i < g.size(); ++i) {
                                                float x = xc.data()[i];
                                                float tv = tanh_v.data()[i];
                                                float dtanh = 1.0f - tv * tv;
                                                float dphi = kSqrt2OverPi * (1.0f + 3.0f * kCoeff * x * x) * dtanh;
                                                ga.data()[i] = g.data()[i] * (0.5f * (1.0f + tv) + 0.5f * x * dphi);
                                            }
                                            tape.accumulate_grad(a_id, ga);
                                        }
                                    });
        }
        return out;
    }

    // Softmax (row-wise for 2D input [N, C])
    inline TensorVar softmax(const TensorVar& a) {
        auto& tape = Tape::current();
        const auto& sh = a.shape();
        if (sh.size() != 2) throw std::invalid_argument("softmax: requires 2D [N, C]");
        const size_t n = sh[0], c = sh[1];

        Tensor out_data({n, c});
        for (size_t i = 0; i < n; ++i) {
            float max_v = -1e30f;
            for (size_t j = 0; j < c; ++j) max_v = std::max(max_v, a.data({i, j}));
            float sum_exp = 0.0f;
            for (size_t j = 0; j < c; ++j) {
                out_data({i, j}) = std::exp(a.data({i, j}) - max_v);
                sum_exp += out_data({i, j});
            }
            for (size_t j = 0; j < c; ++j) out_data({i, j}) /= sum_exp;
        }

        TensorVar out(std::move(out_data));
        if (tape.recording()) {
            uint32_t a_id = a.tape_id;
            Tensor sm = out.data;
            out.tape_id = tape.push(true, out.data,
                                    [&tape, a_id, sm_copy = std::move(sm), n, c](const Tensor& g) {
                                        if (a_id != Tape::kNoGrad) {
                                            Tensor ga({n, c});
                                            for (size_t i = 0; i < n; ++i) {
                                                // dot = sum_j(g_ij * s_ij)
                                                float dot = 0.0f;
                                                for (size_t j = 0; j < c; ++j) dot += g({i, j}) * sm_copy({i, j});
                                                for (size_t j = 0; j < c; ++j)
                                                    ga({i, j}) = sm_copy({i, j}) * (g({i, j}) - dot);
                                            }
                                            tape.accumulate_grad(a_id, ga);
                                        }
                                    });
        }
        return out;
    }

    // Log-softmax (numerically stable)
    inline TensorVar log_softmax(const TensorVar& a) {
        auto& tape = Tape::current();
        const auto& sh = a.shape();
        const size_t n = sh[0], c = sh[1];

        Tensor out_data({n, c});
        Tensor sm_data({n, c}); // store softmax for backward
        for (size_t i = 0; i < n; ++i) {
            float max_v = -1e30f;
            for (size_t j = 0; j < c; ++j) max_v = std::max(max_v, a.data({i, j}));
            float sum_exp = 0.0f;
            for (size_t j = 0; j < c; ++j) {
                sm_data({i, j}) = std::exp(a.data({i, j}) - max_v);
                sum_exp += sm_data({i, j});
            }
            for (size_t j = 0; j < c; ++j) {
                sm_data({i, j}) /= sum_exp;
                out_data({i, j}) = std::log(sm_data({i, j}));
            }
        }

        TensorVar out(std::move(out_data));
        if (tape.recording()) {
            uint32_t a_id = a.tape_id;
            out.tape_id = tape.push(true, out.data,
                                    [&tape, a_id, sm = std::move(sm_data), n, c](const Tensor& g) {
                                        if (a_id != Tape::kNoGrad) {
                                            Tensor ga({n, c});
                                            for (size_t i = 0; i < n; ++i) {
                                                float gsum = 0.0f;
                                                for (size_t j = 0; j < c; ++j) gsum += g({i, j});
                                                for (size_t j = 0; j < c; ++j)
                                                    ga({i, j}) = g({i, j}) - sm({i, j}) * gsum;
                                            }
                                            tape.accumulate_grad(a_id, ga);
                                        }
                                    });
        }
        return out;
    }

    // ELU: x if x>0 else alpha*(exp(x)-1)
    inline TensorVar elu(const TensorVar& a, float alpha = 1.0f) {
        auto& tape = Tape::current();
        const size_t n = a.data.size();
        Tensor out_data(a.shape());
        for (size_t i = 0; i < n; ++i) {
            float x = a.data.data()[i];
            out_data.data()[i] = x > 0.0f ? x : alpha * (std::exp(x) - 1.0f);
        }
        TensorVar out(std::move(out_data));
        if (tape.recording()) {
            uint32_t a_id = a.tape_id;
            Tensor xc = a.data;
            out.tape_id = tape.push(true, out.data,
                                    [&tape, a_id, xc = std::move(xc), alpha](const Tensor& g) {
                                        if (a_id != Tape::kNoGrad) {
                                            Tensor ga(g.shape());
                                            for (size_t i = 0; i < g.size(); ++i) {
                                                float x = xc.data()[i];
                                                ga.data()[i] = g.data()[i] * (x > 0.0f ? 1.0f : alpha * std::exp(x));
                                            }
                                            tape.accumulate_grad(a_id, ga);
                                        }
                                    });
        }
        return out;
    }

    // Leaky ReLU
    inline TensorVar leaky_relu(const TensorVar& a, float alpha = 0.01f) {
        auto& tape = Tape::current();
        const size_t n = a.data.size();
        Tensor out_data(a.shape());
        for (size_t i = 0; i < n; ++i) {
            float x = a.data.data()[i];
            out_data.data()[i] = x > 0.0f ? x : alpha * x;
        }
        TensorVar out(std::move(out_data));
        if (tape.recording()) {
            uint32_t a_id = a.tape_id;
            Tensor xc = a.data;
            out.tape_id = tape.push(true, out.data,
                                    [&tape, a_id, xc = std::move(xc), alpha](const Tensor& g) {
                                        if (a_id != Tape::kNoGrad) {
                                            Tensor ga(g.shape());
                                            for (size_t i = 0; i < g.size(); ++i)
                                                ga.data()[i] = g.data()[i] * (xc.data()[i] > 0.0f ? 1.0f : alpha);
                                            tape.accumulate_grad(a_id, ga);
                                        }
                                    });
        }
        return out;
    }

    // Operator overloads for ergonomic usage
    inline TensorVar operator+(const TensorVar& a, const TensorVar& b) { return add(a, b); }
    inline TensorVar operator*(const TensorVar& a, const TensorVar& b) { return mul(a, b); }
} // namespace manas::nn
