#pragma once
// Neural network layers: Dense, BatchNorm1D, Dropout, Embedding, Conv1D, LayerNorm
// All policy-based, no virtual, header-only.
// Activation policy injected at compile time (default: Identity passthrough).
// Uses TensorVar ops + cache-blocked gemm from ts::DefaultComputationPolicy.
#include <cmath>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <containers/dynamic/SmallVector.hpp>
#include "parameter.hpp"
#include "init.hpp"

namespace manas::nn {
    // ─── Activation policies (zero-cost, compile-time) ───────────────────────────
    struct ActivationNone {
        TensorVar operator()(const TensorVar& x) const { return x; }
        static constexpr std::string_view name = "none";
    };

    struct ActivationReLU {
        TensorVar operator()(const TensorVar& x) const { return relu(x); }
        static constexpr std::string_view name = "relu";
    };

    struct ActivationSigmoid {
        TensorVar operator()(const TensorVar& x) const { return sigmoid(x); }
        static constexpr std::string_view name = "sigmoid";
    };

    struct ActivationTanh {
        TensorVar operator()(const TensorVar& x) const { return tanh_op(x); }
        static constexpr std::string_view name = "tanh";
    };

    struct ActivationGELU {
        TensorVar operator()(const TensorVar& x) const { return gelu(x); }
        static constexpr std::string_view name = "gelu";
    };

    struct ActivationSoftmax {
        TensorVar operator()(const TensorVar& x) const { return softmax(x); }
        static constexpr std::string_view name = "softmax";
    };

    // ─── Dense (fully-connected) layer ───────────────────────────────────────────
    // Template params:
    //   ActPolicy: activation applied after linear transform
    //   WInit, BInit: weight/bias initializer policies
    template <typename ActPolicy = ActivationNone,
              typename WInit = GlorotUniformInit,
              typename BInit = ZerosInit>
    class Dense {
    public:
        Dense(size_t in_features, size_t out_features,
              bool use_bias = true, std::string name = "dense",
              WInit w_init = {}, BInit b_init = {})
            : in_{in_features}, out_{out_features}, use_bias_{use_bias}, name_(std::move(name)) {
            w_ = std::make_unique<Parameter>(name_ + ".weight",
                                             w_init({in_features, out_features}), true);
            if (use_bias_)
                b_ = std::make_unique<Parameter>(name_ + ".bias",
                                                 b_init({out_features}), true);
        }

        // x: [N, in_features] -> [N, out_features]
        TensorVar forward(const TensorVar& x, bool /*training*/  = true) {
            w_->register_on_tape();
            if (use_bias_ && b_) b_->register_on_tape();
            auto z = matmul(x, w_->var); // [N, out]
            if (use_bias_) z = add_bias(z, b_->var);
            return act_(z);
        }

        ParamList parameters() {
            ParamList p;
            p.push_back(w_.get());
            if (use_bias_ && b_) p.push_back(b_.get());
            return p;
        }

        std::string_view name() const noexcept { return name_; }
        size_t in_features() const noexcept { return in_; }
        size_t out_features() const noexcept { return out_; }

    private:
        size_t in_, out_;
        bool use_bias_;
        std::string name_;
        ActPolicy act_;
        std::unique_ptr<Parameter> w_, b_;
    };

    // ─── BatchNorm1D ──────────────────────────────────────────────────────────────
    // Normalizes each feature across the batch.
    // Trainable gamma (scale) and beta (shift). Running mean/var for inference.
    class BatchNorm1D {
    public:
        explicit BatchNorm1D(size_t num_features, float eps = 1e-5f, float momentum = 0.1f,
                             std::string name = "bn")
            : num_features_{num_features}, eps_{eps}, momentum_{momentum}, name_(std::move(name)) {
            gamma_ = std::make_unique<Parameter>(name_ + ".gamma", OnesInit{}({num_features}), true);
            beta_ = std::make_unique<Parameter>(name_ + ".beta", ZerosInit{}({num_features}), true);
            running_mean_ = Tensor({num_features});
            running_var_ = Tensor({num_features});
            for (size_t i = 0; i < num_features; ++i) {
                running_mean_.data()[i] = 0.0f;
                running_var_.data()[i] = 1.0f;
            }
        }

        // x: [N, F] -> [N, F]
        TensorVar forward(const TensorVar& x, bool training = true) {
            gamma_->register_on_tape();
            beta_->register_on_tape();
            const size_t n = x.shape()[0], f = x.shape()[1];
            if (f != num_features_) throw std::invalid_argument("BatchNorm1D: feature mismatch");

            Tensor mean({f}), var({f});

            if (training) {
                // Compute batch mean/var
                for (size_t j = 0; j < f; ++j) mean.data()[j] = 0.0f;
                for (size_t i = 0; i < n; ++i)
                    for (size_t j = 0; j < f; ++j) mean.data()[j] += x.data({i, j});
                float inv_n = 1.0f / static_cast<float>(n);
                for (size_t j = 0; j < f; ++j) mean.data()[j] *= inv_n;

                for (size_t j = 0; j < f; ++j) var.data()[j] = 0.0f;
                for (size_t i = 0; i < n; ++i)
                    for (size_t j = 0; j < f; ++j) {
                        float d = x.data({i, j}) - mean.data()[j];
                        var.data()[j] += d * d;
                    }
                for (size_t j = 0; j < f; ++j) var.data()[j] *= inv_n;

                // Update running stats
                for (size_t j = 0; j < f; ++j) {
                    running_mean_.data()[j] = (1.0f - momentum_) * running_mean_.data()[j] + momentum_ * mean.data()[j];
                    running_var_.data()[j] = (1.0f - momentum_) * running_var_.data()[j] + momentum_ * var.data()[j];
                }
            }
            else {
                mean = running_mean_;
                var = running_var_;
            }

            // Normalize + scale + shift (with gradient tracking)
            Tensor x_hat_data({n, f});
            for (size_t i = 0; i < n; ++i)
                for (size_t j = 0; j < f; ++j)
                    x_hat_data({i, j}) = (x.data({i, j}) - mean.data()[j]) / std::sqrt(var.data()[j] + eps_);

            TensorVar x_hat(x_hat_data);
            // Manually record backward for BN: dx_hat propagated through gamma/beta
            auto& tape = Tape::current();
            if (tape.recording()) {
                uint32_t x_id = x.tape_id, g_id = gamma_->tape_id(), b_id = beta_->tape_id();
                Tensor inv_std({f}), mean_copy = mean;
                for (size_t j = 0; j < f; ++j) inv_std.data()[j] = 1.0f / std::sqrt(var.data()[j] + eps_);

                TensorVar out_var(Tensor({n, f}));
                for (size_t i = 0; i < n; ++i)
                    for (size_t j = 0; j < f; ++j)
                        out_var.data({i, j}) = x_hat_data({i, j}) * gamma_->data().data()[j] + beta_->data().data()[j];

                out_var.tape_id = tape.push(true, out_var.data,
                                            [&tape, x_id, g_id, b_id, n, f,
                                                xh = x_hat_data, invs = inv_std, xdata = x.data, mc = mean_copy,
                                                gdata = gamma_->data()](const Tensor& g) {
                                                // d(beta) = sum_i g_{i,j}
                                                if (b_id != Tape::kNoGrad) {
                                                    Tensor db({f});
                                                    for (size_t j = 0; j < f; ++j) db.data()[j] = 0.0f;
                                                    for (size_t i = 0; i < n; ++i)
                                                        for (size_t j = 0; j < f; ++j) db.data()[j] += g({i, j});
                                                    tape.accumulate_grad(b_id, db);
                                                }
                                                // d(gamma) = sum_i g_{i,j} * x_hat_{i,j}
                                                if (g_id != Tape::kNoGrad) {
                                                    Tensor dg({f});
                                                    for (size_t j = 0; j < f; ++j) dg.data()[j] = 0.0f;
                                                    for (size_t i = 0; i < n; ++i)
                                                        for (size_t j = 0; j < f; ++j) dg.data()[j] += g({i, j}) * xh({
                                                            i, j
                                                        });
                                                    tape.accumulate_grad(g_id, dg);
                                                }
                                                // d(x) = gamma / (N * std) * (N * g - sum_g - x_hat * sum(g * x_hat))
                                                if (x_id != Tape::kNoGrad) {
                                                    Tensor dx({n, f});
                                                    float inv_n = 1.0f / static_cast<float>(n);
                                                    for (size_t j = 0; j < f; ++j) {
                                                        float sum_g = 0.0f, sum_gxh = 0.0f;
                                                        for (size_t i = 0; i < n; ++i) {
                                                            sum_g += g({i, j});
                                                            sum_gxh += g({i, j}) * xh({i, j});
                                                        }
                                                        for (size_t i = 0; i < n; ++i)
                                                            dx({i, j}) = gdata.data()[j] * invs.data()[j] * inv_n *
                                                            (static_cast<float>(n) * g({i, j}) - sum_g - xh({i, j})
                                                                * sum_gxh);
                                                    }
                                                    tape.accumulate_grad(x_id, dx);
                                                }
                                            });
                return out_var;
            }
            // No-grad path
            TensorVar out_var(Tensor({n, f}));
            for (size_t i = 0; i < n; ++i)
                for (size_t j = 0; j < f; ++j)
                    out_var.data({i, j}) = x_hat_data({i, j}) * gamma_->data().data()[j] + beta_->data().data()[j];
            return out_var;
        }

        ParamList parameters() {
            ParamList p;
            p.push_back(gamma_.get());
            p.push_back(beta_.get());
            return p;
        }

        std::string_view name() const noexcept { return name_; }

    private:
        size_t num_features_;
        float eps_, momentum_;
        std::string name_;
        std::unique_ptr<Parameter> gamma_, beta_;
        Tensor running_mean_, running_var_;
    };

    // ─── LayerNorm ────────────────────────────────────────────────────────────────
    class LayerNorm {
    public:
        explicit LayerNorm(size_t normalized_shape, float eps = 1e-5f, std::string name = "ln")
            : f_{normalized_shape}, eps_{eps}, name_(std::move(name)) {
            gamma_ = std::make_unique<Parameter>(name_ + ".gamma", OnesInit{}({f_}), true);
            beta_ = std::make_unique<Parameter>(name_ + ".beta", ZerosInit{}({f_}), true);
        }

        TensorVar forward(const TensorVar& x, bool /*training*/  = true) {
            gamma_->register_on_tape();
            beta_->register_on_tape();
            const size_t n = x.shape()[0], f = x.shape()[1];
            if (f != f_) throw std::invalid_argument("LayerNorm: feature mismatch");
            auto& tape = Tape::current();

            TensorVar out(Tensor({n, f}));
            Tensor xhat({n, f}), inv_std_vec({n});
            for (size_t i = 0; i < n; ++i) {
                float m = 0.0f;
                for (size_t j = 0; j < f; ++j) m += x.data({i, j});
                m /= static_cast<float>(f);
                float var = 0.0f;
                for (size_t j = 0; j < f; ++j) {
                    float d = x.data({i, j}) - m;
                    var += d * d;
                }
                var /= static_cast<float>(f);
                float is = 1.0f / std::sqrt(var + eps_);
                inv_std_vec.data()[i] = is;
                for (size_t j = 0; j < f; ++j) {
                    xhat({i, j}) = (x.data({i, j}) - m) * is;
                    out.data({i, j}) = xhat({i, j}) * gamma_->data().data()[j] + beta_->data().data()[j];
                }
            }

            if (tape.recording()) {
                uint32_t x_id = x.tape_id, g_id = gamma_->tape_id(), b_id = beta_->tape_id();
                out.tape_id = tape.push(true, out.data,
                                        [&tape, x_id, g_id, b_id, n, f,
                                            xh = xhat, isv = inv_std_vec,
                                            gdata = gamma_->data()](const Tensor& g_upstream) {
                                            if (b_id != Tape::kNoGrad) {
                                                Tensor db({f});
                                                for (size_t j = 0; j < f; ++j) db.data()[j] = 0.0f;
                                                for (size_t i = 0; i < n; ++i)
                                                    for (size_t j = 0; j < f; ++j) db.data()[j] += g_upstream({i, j});
                                                tape.accumulate_grad(b_id, db);
                                            }
                                            if (g_id != Tape::kNoGrad) {
                                                Tensor dg({f});
                                                for (size_t j = 0; j < f; ++j) dg.data()[j] = 0.0f;
                                                for (size_t i = 0; i < n; ++i)
                                                    for (size_t j = 0; j < f; ++j) dg.data()[j] += g_upstream({i, j}) *
                                                        xh({i, j});
                                                tape.accumulate_grad(g_id, dg);
                                            }
                                            if (x_id != Tape::kNoGrad) {
                                                Tensor dx({n, f});
                                                float inv_f = 1.0f / static_cast<float>(f);
                                                for (size_t i = 0; i < n; ++i) {
                                                    float is = isv.data()[i];
                                                    // g_hat = g_upstream * gamma
                                                    float sg = 0.0f, sgx = 0.0f;
                                                    for (size_t j = 0; j < f; ++j) {
                                                        float gh = g_upstream({i, j}) * gdata.data()[j];
                                                        sg += gh;
                                                        sgx += gh * xh({i, j});
                                                    }
                                                    for (size_t j = 0; j < f; ++j) {
                                                        float gh = g_upstream({i, j}) * gdata.data()[j];
                                                        dx({i, j}) = is * (gh - inv_f * sg - inv_f * xh({i, j}) * sgx);
                                                    }
                                                }
                                                tape.accumulate_grad(x_id, dx);
                                            }
                                        });
            }
            return out;
        }

        ParamList parameters() {
            ParamList p;
            p.push_back(gamma_.get());
            p.push_back(beta_.get());
            return p;
        }

        std::string_view name() const noexcept { return name_; }

    private:
        size_t f_;
        float eps_;
        std::string name_;
        std::unique_ptr<Parameter> gamma_, beta_;
    };

    // ─── Dropout ──────────────────────────────────────────────────────────────────
    // Inverted dropout: scales kept neurons by 1/(1-p) during training.
    class Dropout {
    public:
        explicit Dropout(float p = 0.5f, uint64_t seed = 42, std::string name = "dropout")
            : p_{p}, rng_(seed), name_(std::move(name)) {}

        TensorVar forward(const TensorVar& x, bool training = true) {
            if (!training || p_ == 0.0f) return x;
            auto& tape = Tape::current();
            const size_t n = x.data.size();
            float scale = 1.0f / (1.0f - p_);

            std::bernoulli_distribution dist(1.0 - static_cast<double>(p_));
            Tensor mask(x.shape());
            for (size_t i = 0; i < n; ++i)
                mask.data()[i] = dist(rng_) ? scale : 0.0f;

            Tensor out_data(x.shape());
            for (size_t i = 0; i < n; ++i)
                out_data.data()[i] = x.data.data()[i] * mask.data()[i];

            TensorVar out(std::move(out_data));
            if (tape.recording()) {
                uint32_t x_id = x.tape_id;
                out.tape_id = tape.push(true, out.data,
                                        [&tape, x_id, m = std::move(mask)](const Tensor& g) {
                                            if (x_id != Tape::kNoGrad) {
                                                Tensor gx(g.shape());
                                                for (size_t i = 0; i < g.size(); ++i)
                                                    gx.data()[i] = g.data()[i] * m.data()[i];
                                                tape.accumulate_grad(x_id, gx);
                                            }
                                        });
            }
            return out;
        }

        ParamList parameters() { return {}; }
        std::string_view name() const noexcept { return name_; }

    private:
        float p_;
        std::mt19937_64 rng_;
        std::string name_;
    };

    // ─── Embedding ────────────────────────────────────────────────────────────────
    // Integer index -> dense vector lookup. Gradient is sparse (sum of upstream grads).
    class Embedding {
    public:
        Embedding(size_t vocab_size, size_t embed_dim,
                  std::string name = "embedding")
            : vocab_{vocab_size}, dim_{embed_dim}, name_(std::move(name)) {
            weight_ = std::make_unique<Parameter>(name_ + ".weight",
                                                  NormalInit{0.0f, 0.02f}({vocab_size, embed_dim}), true);
        }

        // x_indices: [N] integer indices as float -> out: [N, embed_dim]
        TensorVar forward(const TensorVar& x, bool /*training*/  = true) const {
            weight_->register_on_tape();
            auto& tape = Tape::current();
            const size_t n = x.shape()[0];
            Tensor out_data({n, dim_});
            std::vector<size_t> idx_vec(n);
            for (size_t i = 0; i < n; ++i) {
                size_t idx = static_cast<size_t>(x.data.data()[i]);
                idx_vec[i] = idx;
                if (idx >= vocab_) throw std::out_of_range("Embedding: index out of range");
                for (size_t j = 0; j < dim_; ++j)
                    out_data({i, j}) = weight_->data()({idx, j});
            }

            TensorVar out(std::move(out_data));
            if (tape.recording()) {
                uint32_t w_id = weight_->tape_id();
                out.tape_id = tape.push(true, out.data,
                                        [&tape, w_id, idx_vec, n, dim = dim_, vocab = vocab_](const Tensor& g) {
                                            if (w_id != Tape::kNoGrad) {
                                                Tensor dw({vocab, dim});
                                                for (size_t k = 0; k < dw.size(); ++k) dw.data()[k] = 0.0f;
                                                for (size_t i = 0; i < n; ++i)
                                                    for (size_t j = 0; j < dim; ++j)
                                                        dw({idx_vec[i], j}) += g({i, j});
                                                tape.accumulate_grad(w_id, dw);
                                            }
                                        });
            }
            return out;
        }

        ParamList parameters() { return {weight_.get()}; }
        std::string_view name() const noexcept { return name_; }
        size_t vocab_size() const noexcept { return vocab_; }
        size_t embed_dim() const noexcept { return dim_; }

    private:
        size_t vocab_, dim_;
        std::string name_;
        std::unique_ptr<Parameter> weight_;
    };
} // namespace manas::nn
