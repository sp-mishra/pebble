#pragma once
// Optimizers: SGD (with momentum/Nesterov), Adam, AdaGrad, RMSProp
// All operate on ParamList (borrowed pointers to Parameters).
// No virtual. Policy-based: each optimizer is a concrete type satisfying OptimizerConcept.
// Gradient clipping: MaxNorm and GlobalNorm policies.
#include <cmath>
#include <unordered_map>
#include <vector>
#include <containers/dynamic/SmallVector.hpp>
#include "parameter.hpp"

namespace manas::nn {
    // ─── Gradient clipping policies ──────────────────────────────────────────────
    struct NoClip {
        void operator()(const ParamList&) const noexcept {}
    };

    struct GlobalNormClip {
        float max_norm;

        void operator()(const ParamList& params) const {
            float total_sq = 0.0f;
            for (auto* p : params) {
                if (!p->trainable) continue;
                const auto& g = p->grad();
                for (size_t i = 0; i < g.size(); ++i) total_sq += g.data()[i] * g.data()[i];
            }
            float norm = std::sqrt(total_sq);
            if (norm > max_norm) {
                float scale = max_norm / (norm + 1e-6f);
                for (auto* p : params) {
                    if (!p->trainable) continue;
                    auto& g = const_cast<Tensor&>(p->grad());
                    for (size_t i = 0; i < g.size(); ++i) g.data()[i] *= scale;
                }
            }
        }
    };

    // ─── Optimizer concept ────────────────────────────────────────────────────────
    template <typename O>
    concept OptimizerConcept = requires(O& opt, const ParamList& params) {
        opt.step(params);
        opt.zero_grad(params);
    };

    // ─── SGD with optional momentum and Nesterov ─────────────────────────────────
    template <typename ClipPolicy = NoClip>
    class SGD {
    public:
        explicit SGD(float lr = 0.01f, float momentum = 0.0f,
                     float weight_decay = 0.0f, bool nesterov = false,
                     ClipPolicy clip = {})
            : lr_{lr}, momentum_{momentum}, wd_{weight_decay},
              nesterov_{nesterov}, clip_{clip} {}

        void step(const ParamList& params) {
            clip_(params);
            for (auto* p : params) {
                if (!p->trainable) continue;
                auto& w = p->data();
                const auto& g = p->grad();
                const size_t n = w.size();

                auto& v = velocity_[p];
                if (v.size() != n) { v.assign(n, 0.0f); }

                for (size_t i = 0; i < n; ++i) {
                    float gi = g.data()[i] + wd_ * w.data()[i];
                    v[i] = momentum_ * v[i] + gi;
                    float update = nesterov_ ? (momentum_ * v[i] + gi) : v[i];
                    w.data()[i] -= lr_ * update;
                }
            }
        }

        void zero_grad(const ParamList& params) {
            for (auto* p : params) p->zero_grad();
        }

    private:
        float lr_, momentum_, wd_;
        bool nesterov_;
        ClipPolicy clip_;
        std::unordered_map<const Parameter*, std::vector<float>> velocity_;
    };

    // ─── Adam (Kingma & Ba 2015) ──────────────────────────────────────────────────
    template <typename ClipPolicy = NoClip>
    class Adam {
    public:
        explicit Adam(float lr = 1e-3f, float beta1 = 0.9f, float beta2 = 0.999f,
                      float eps = 1e-8f, float weight_decay = 0.0f,
                      ClipPolicy clip = {})
            : lr_{lr}, b1_{beta1}, b2_{beta2}, eps_{eps}, wd_{weight_decay}, clip_{clip} {}

        void step(const ParamList& params) {
            clip_(params);
            ++step_;
            float bc1 = 1.0f - std::pow(b1_, static_cast<float>(step_));
            float bc2 = 1.0f - std::pow(b2_, static_cast<float>(step_));
            float lr_t = lr_ * std::sqrt(bc2) / bc1;

            for (auto* p : params) {
                if (!p->trainable) continue;
                auto& w = p->data();
                const auto& g = p->grad();
                const size_t n = w.size();

                auto& m = m_[p];
                auto& v = v_[p];
                if (m.size() != n) {
                    m.assign(n, 0.0f);
                    v.assign(n, 0.0f);
                }

                for (size_t i = 0; i < n; ++i) {
                    float gi = g.data()[i] + wd_ * w.data()[i];
                    m[i] = b1_ * m[i] + (1.0f - b1_) * gi;
                    v[i] = b2_ * v[i] + (1.0f - b2_) * gi * gi;
                    w.data()[i] -= lr_t * m[i] / (std::sqrt(v[i]) + eps_);
                }
            }
        }

        void zero_grad(const ParamList& params) {
            for (auto* p : params) p->zero_grad();
        }

        void reset_state() {
            m_.clear();
            v_.clear();
            step_ = 0;
        }

    private:
        float lr_, b1_, b2_, eps_, wd_;
        ClipPolicy clip_;
        uint64_t step_ = 0;
        std::unordered_map<const Parameter*, std::vector<float>> m_, v_;
    };

    // ─── AdaGrad ──────────────────────────────────────────────────────────────────
    template <typename ClipPolicy = NoClip>
    class AdaGrad {
    public:
        explicit AdaGrad(float lr = 0.01f, float eps = 1e-8f,
                         float weight_decay = 0.0f, ClipPolicy clip = {})
            : lr_{lr}, eps_{eps}, wd_{weight_decay}, clip_{clip} {}

        void step(const ParamList& params) {
            clip_(params);
            for (auto* p : params) {
                if (!p->trainable) continue;
                auto& w = p->data();
                const auto& g = p->grad();
                const size_t n = w.size();
                auto& h = h_[p];
                if (h.size() != n) h.assign(n, 0.0f);
                for (size_t i = 0; i < n; ++i) {
                    float gi = g.data()[i] + wd_ * w.data()[i];
                    h[i] += gi * gi;
                    w.data()[i] -= lr_ * gi / (std::sqrt(h[i]) + eps_);
                }
            }
        }

        void zero_grad(const ParamList& params) {
            for (auto* p : params) p->zero_grad();
        }

    private:
        float lr_, eps_, wd_;
        ClipPolicy clip_;
        std::unordered_map<const Parameter*, std::vector<float>> h_;
    };

    // ─── RMSProp ──────────────────────────────────────────────────────────────────
    template <typename ClipPolicy = NoClip>
    class RMSProp {
    public:
        explicit RMSProp(float lr = 0.01f, float alpha = 0.99f, float eps = 1e-8f,
                         float weight_decay = 0.0f, float momentum = 0.0f,
                         ClipPolicy clip = {})
            : lr_{lr}, alpha_{alpha}, eps_{eps}, wd_{weight_decay},
              momentum_{momentum}, clip_{clip} {}

        void step(const ParamList& params) {
            clip_(params);
            for (auto* p : params) {
                if (!p->trainable) continue;
                auto& w = p->data();
                const auto& g = p->grad();
                const size_t n = w.size();
                auto& sq = sq_avg_[p];
                auto& buf = buf_[p];
                if (sq.size() != n) {
                    sq.assign(n, 1.0f);
                    buf.assign(n, 0.0f);
                }
                for (size_t i = 0; i < n; ++i) {
                    float gi = g.data()[i] + wd_ * w.data()[i];
                    sq[i] = alpha_ * sq[i] + (1.0f - alpha_) * gi * gi;
                    float step = gi / (std::sqrt(sq[i]) + eps_);
                    if (momentum_ > 0.0f) {
                        buf[i] = momentum_ * buf[i] + step;
                        w.data()[i] -= lr_ * buf[i];
                    }
                    else {
                        w.data()[i] -= lr_ * step;
                    }
                }
            }
        }

        void zero_grad(const ParamList& params) {
            for (auto* p : params) p->zero_grad();
        }

    private:
        float lr_, alpha_, eps_, wd_, momentum_;
        ClipPolicy clip_;
        std::unordered_map<const Parameter*, std::vector<float>> sq_avg_, buf_;
    };

    // ─── AdamW (decoupled weight decay) ──────────────────────────────────────────
    template <typename ClipPolicy = NoClip>
    class AdamW {
    public:
        explicit AdamW(float lr = 1e-3f, float beta1 = 0.9f, float beta2 = 0.999f,
                       float eps = 1e-8f, float weight_decay = 0.01f,
                       ClipPolicy clip = {})
            : lr_{lr}, b1_{beta1}, b2_{beta2}, eps_{eps}, wd_{weight_decay}, clip_{clip} {}

        void step(const ParamList& params) {
            clip_(params);
            ++step_;
            float bc1 = 1.0f - std::pow(b1_, static_cast<float>(step_));
            float bc2 = 1.0f - std::pow(b2_, static_cast<float>(step_));
            float lr_t = lr_ * std::sqrt(bc2) / bc1;

            for (auto* p : params) {
                if (!p->trainable) continue;
                auto& w = p->data();
                const auto& g = p->grad();
                const size_t n = w.size();
                auto& m = m_[p];
                auto& v = v_[p];
                if (m.size() != n) {
                    m.assign(n, 0.0f);
                    v.assign(n, 0.0f);
                }
                for (size_t i = 0; i < n; ++i) {
                    float gi = g.data()[i];
                    m[i] = b1_ * m[i] + (1.0f - b1_) * gi;
                    v[i] = b2_ * v[i] + (1.0f - b2_) * gi * gi;
                    // Decoupled weight decay applied to w directly
                    w.data()[i] -= wd_ * lr_ * w.data()[i];
                    w.data()[i] -= lr_t * m[i] / (std::sqrt(v[i]) + eps_);
                }
            }
        }

        void zero_grad(const ParamList& params) {
            for (auto* p : params) p->zero_grad();
        }

    private:
        float lr_, b1_, b2_, eps_, wd_;
        ClipPolicy clip_;
        uint64_t step_ = 0;
        std::unordered_map<const Parameter*, std::vector<float>> m_, v_;
    };
} // namespace manas::nn
