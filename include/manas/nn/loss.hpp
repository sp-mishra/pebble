#pragma once
// Loss functions: MSE, MAE, Huber, BinaryCrossEntropy, CategoricalCrossEntropy,
//                NLLLoss, HingeLoss, KLDivergence
// All return a scalar TensorVar, differentiable through the autodiff tape.
#include <cmath>
#include <stdexcept>
#include "ops.hpp"

namespace manas::nn {

// ─── Mean Squared Error ───────────────────────────────────────────────────────
// loss = mean((pred - target)^2)
inline TensorVar mse_loss(const TensorVar& pred, const TensorVar& target) {
    const size_t n = pred.data.size();
    Tensor diff(pred.shape());
    for (size_t i = 0; i < n; ++i)
        diff.data()[i] = pred.data.data()[i] - target.data.data()[i];

    auto& tape = Tape::current();
    Tensor sq(pred.shape());
    for (size_t i = 0; i < n; ++i) sq.data()[i] = diff.data()[i] * diff.data()[i];

    float loss_val = 0.0f;
    for (size_t i = 0; i < n; ++i) loss_val += sq.data()[i];
    loss_val /= static_cast<float>(n);

    TensorVar out(Tensor({1}, {loss_val}));
    if (tape.recording()) {
        uint32_t p_id = pred.tape_id;
        out.tape_id = tape.push(true, out.data,
            [&tape, p_id, d = std::move(diff), n](const Tensor& g) {
                if (p_id != Tape::kNoGrad) {
                    float coeff = 2.0f * g.data()[0] / static_cast<float>(n);
                    Tensor gp(d.shape());
                    for (size_t i = 0; i < n; ++i) gp.data()[i] = coeff * d.data()[i];
                    tape.accumulate_grad(p_id, gp);
                }
            });
    }
    return out;
}

// ─── Mean Absolute Error ──────────────────────────────────────────────────────
inline TensorVar mae_loss(const TensorVar& pred, const TensorVar& target) {
    const size_t n = pred.data.size();
    auto& tape = Tape::current();
    float loss_val = 0.0f;
    Tensor signs(pred.shape());
    for (size_t i = 0; i < n; ++i) {
        float d = pred.data.data()[i] - target.data.data()[i];
        loss_val += std::abs(d);
        signs.data()[i] = d > 0.0f ? 1.0f : (d < 0.0f ? -1.0f : 0.0f);
    }
    loss_val /= static_cast<float>(n);

    TensorVar out(Tensor({1}, {loss_val}));
    if (tape.recording()) {
        uint32_t p_id = pred.tape_id;
        out.tape_id = tape.push(true, out.data,
            [&tape, p_id, s = std::move(signs), n](const Tensor& g) {
                if (p_id != Tape::kNoGrad) {
                    float coeff = g.data()[0] / static_cast<float>(n);
                    Tensor gp(s.shape());
                    for (size_t i = 0; i < n; ++i) gp.data()[i] = coeff * s.data()[i];
                    tape.accumulate_grad(p_id, gp);
                }
            });
    }
    return out;
}

// ─── Huber Loss (smooth L1) ───────────────────────────────────────────────────
// delta-robust: L2 for |d|<=delta, L1 beyond
inline TensorVar huber_loss(const TensorVar& pred, const TensorVar& target, float delta = 1.0f) {
    const size_t n = pred.data.size();
    auto& tape = Tape::current();
    float loss_val = 0.0f;
    Tensor grad_coeff(pred.shape());
    for (size_t i = 0; i < n; ++i) {
        float d = pred.data.data()[i] - target.data.data()[i];
        float ad = std::abs(d);
        if (ad <= delta) {
            loss_val += 0.5f * d * d;
            grad_coeff.data()[i] = d;
        } else {
            loss_val += delta * (ad - 0.5f * delta);
            grad_coeff.data()[i] = delta * (d > 0.0f ? 1.0f : -1.0f);
        }
    }
    loss_val /= static_cast<float>(n);

    TensorVar out(Tensor({1}, {loss_val}));
    if (tape.recording()) {
        uint32_t p_id = pred.tape_id;
        out.tape_id = tape.push(true, out.data,
            [&tape, p_id, gc = std::move(grad_coeff), n](const Tensor& g) {
                if (p_id != Tape::kNoGrad) {
                    float coeff = g.data()[0] / static_cast<float>(n);
                    Tensor gp(gc.shape());
                    for (size_t i = 0; i < n; ++i) gp.data()[i] = coeff * gc.data()[i];
                    tape.accumulate_grad(p_id, gp);
                }
            });
    }
    return out;
}

// ─── Binary Cross-Entropy ────────────────────────────────────────────────────
// loss = -mean(y * log(p) + (1-y) * log(1-p))
// pred: probabilities in (0,1), target: {0,1}
inline TensorVar bce_loss(const TensorVar& pred, const TensorVar& target, float eps = 1e-7f) {
    const size_t n = pred.data.size();
    auto& tape = Tape::current();
    float loss_val = 0.0f;
    Tensor grad_coeff(pred.shape());
    for (size_t i = 0; i < n; ++i) {
        float p = std::clamp(pred.data.data()[i], eps, 1.0f - eps);
        float y = target.data.data()[i];
        loss_val -= y * std::log(p) + (1.0f - y) * std::log(1.0f - p);
        grad_coeff.data()[i] = (-y / p + (1.0f - y) / (1.0f - p));
    }
    loss_val /= static_cast<float>(n);

    TensorVar out(Tensor({1}, {loss_val}));
    if (tape.recording()) {
        uint32_t p_id = pred.tape_id;
        out.tape_id = tape.push(true, out.data,
            [&tape, p_id, gc = std::move(grad_coeff), n](const Tensor& g) {
                if (p_id != Tape::kNoGrad) {
                    float coeff = g.data()[0] / static_cast<float>(n);
                    Tensor gp(gc.shape());
                    for (size_t i = 0; i < n; ++i) gp.data()[i] = coeff * gc.data()[i];
                    tape.accumulate_grad(p_id, gp);
                }
            });
    }
    return out;
}

// ─── Categorical Cross-Entropy (with log-softmax internally) ─────────────────
// pred: raw logits [N, C], target: class indices as float [N]
inline TensorVar cross_entropy_loss(const TensorVar& logits, const TensorVar& target) {
    // Apply log-softmax to logits
    auto lsm = log_softmax(logits);
    const size_t n = logits.shape()[0];
    const size_t c = logits.shape()[1];

    auto& tape = Tape::current();
    float loss_val = 0.0f;
    std::vector<size_t> class_idx(n);
    for (size_t i = 0; i < n; ++i) {
        class_idx[i] = static_cast<size_t>(target.data.data()[i]);
        if (class_idx[i] >= c) throw std::out_of_range("cross_entropy: class index out of range");
        loss_val -= lsm.data({i, class_idx[i]});
    }
    loss_val /= static_cast<float>(n);

    TensorVar out(Tensor({1}, {loss_val}));
    if (tape.recording()) {
        uint32_t lsm_id = lsm.tape_id;
        out.tape_id = tape.push(true, out.data,
            [&tape, lsm_id, n, c, cidx = std::move(class_idx)](const Tensor& g) {
                if (lsm_id != Tape::kNoGrad) {
                    float coeff = -g.data()[0] / static_cast<float>(n);
                    Tensor gls({n, c});
                    for (size_t k = 0; k < gls.size(); ++k) gls.data()[k] = 0.0f;
                    for (size_t i = 0; i < n; ++i)
                        gls({i, cidx[i]}) = coeff;
                    tape.accumulate_grad(lsm_id, gls);
                }
            });
    }
    return out;
}

// ─── NLL Loss (negative log-likelihood) ──────────────────────────────────────
// pred: log-probabilities [N, C], target: class indices [N]
inline TensorVar nll_loss(const TensorVar& log_probs, const TensorVar& target) {
    const size_t n = log_probs.shape()[0];
    const size_t c = log_probs.shape()[1];
    auto& tape = Tape::current();
    float loss_val = 0.0f;
    std::vector<size_t> class_idx(n);
    for (size_t i = 0; i < n; ++i) {
        class_idx[i] = static_cast<size_t>(target.data.data()[i]);
        loss_val -= log_probs.data({i, class_idx[i]});
    }
    loss_val /= static_cast<float>(n);

    TensorVar out(Tensor({1}, {loss_val}));
    if (tape.recording()) {
        uint32_t lp_id = log_probs.tape_id;
        out.tape_id = tape.push(true, out.data,
            [&tape, lp_id, n, c, cidx = std::move(class_idx)](const Tensor& g) {
                if (lp_id != Tape::kNoGrad) {
                    float coeff = -g.data()[0] / static_cast<float>(n);
                    Tensor glp({n, c});
                    for (size_t k = 0; k < glp.size(); ++k) glp.data()[k] = 0.0f;
                    for (size_t i = 0; i < n; ++i) glp({i, cidx[i]}) = coeff;
                    tape.accumulate_grad(lp_id, glp);
                }
            });
    }
    return out;
}

// ─── Hinge Loss (SVM-style, binary: y in {-1, +1}) ──────────────────────────
inline TensorVar hinge_loss(const TensorVar& pred, const TensorVar& target) {
    const size_t n = pred.data.size();
    auto& tape = Tape::current();
    float loss_val = 0.0f;
    Tensor grad_coeff(pred.shape());
    for (size_t i = 0; i < n; ++i) {
        float m = target.data.data()[i] * pred.data.data()[i];
        float h = std::max(0.0f, 1.0f - m);
        loss_val += h;
        grad_coeff.data()[i] = h > 0.0f ? -target.data.data()[i] : 0.0f;
    }
    loss_val /= static_cast<float>(n);

    TensorVar out(Tensor({1}, {loss_val}));
    if (tape.recording()) {
        uint32_t p_id = pred.tape_id;
        out.tape_id = tape.push(true, out.data,
            [&tape, p_id, gc = std::move(grad_coeff), n](const Tensor& g) {
                if (p_id != Tape::kNoGrad) {
                    float coeff = g.data()[0] / static_cast<float>(n);
                    Tensor gp(gc.shape());
                    for (size_t i = 0; i < n; ++i) gp.data()[i] = coeff * gc.data()[i];
                    tape.accumulate_grad(p_id, gp);
                }
            });
    }
    return out;
}

// ─── KL Divergence: sum(p * (log(p) - log(q))) ───────────────────────────────
// pred = log(q) (log-probabilities), target = p (true probabilities)
inline TensorVar kl_div_loss(const TensorVar& log_q, const TensorVar& p, float eps = 1e-7f) {
    const size_t n = log_q.data.size();
    auto& tape = Tape::current();
    float loss_val = 0.0f;
    Tensor grad_coeff(log_q.shape());
    for (size_t i = 0; i < n; ++i) {
        float pv = std::max(p.data.data()[i], eps);
        float lq = log_q.data.data()[i];
        loss_val += pv * (std::log(pv) - lq);
        grad_coeff.data()[i] = -pv;
    }
    loss_val /= static_cast<float>(n);

    TensorVar out(Tensor({1}, {loss_val}));
    if (tape.recording()) {
        uint32_t lq_id = log_q.tape_id;
        out.tape_id = tape.push(true, out.data,
            [&tape, lq_id, gc = std::move(grad_coeff), n](const Tensor& g) {
                if (lq_id != Tape::kNoGrad) {
                    float coeff = g.data()[0] / static_cast<float>(n);
                    Tensor glq(gc.shape());
                    for (size_t i = 0; i < n; ++i) glq.data()[i] = coeff * gc.data()[i];
                    tape.accumulate_grad(lq_id, glq);
                }
            });
    }
    return out;
}

} // namespace manas::nn
