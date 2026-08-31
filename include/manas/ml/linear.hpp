#pragma once
// Linear models: LinearRegression, RidgeRegression, LogisticRegression
// Uses ts::tensor for matrix ops. No virtual. Policy-based L1/L2 regularization.
#include <cmath>
#include <stdexcept>
#include <containers/tensor/tensor.hpp>

namespace manas::ml {

// ── Linear Regression (closed-form normal equations: W = (X^T X + λI)^{-1} X^T y) ──
// Solver policy: NormalEquations (default)
struct NormalEquationsSolver {};

template<typename SolverPolicy = NormalEquationsSolver>
class LinearRegression {
public:
    explicit LinearRegression(float ridge_lambda = 0.0f, bool fit_intercept = true)
        : lambda_{ridge_lambda}, fit_intercept_{fit_intercept} {}

    void fit(const ts::tensor<float>& X, const ts::tensor<float>& y) {
        // X: [n_samples, n_features], y: [n_samples]
        const auto& sh = X.shape();
        if (sh.size() != 2) throw std::invalid_argument("X must be 2D [N, F]");
        const size_t n = sh[0], f = sh[1];
        if (y.shape()[0] != n) throw std::invalid_argument("X/y row mismatch");

        // Augment X with a bias column of 1s if fit_intercept
        const size_t fa = fit_intercept_ ? f + 1 : f;
        ts::tensor<float> Xa({n, fa});
        for (size_t i = 0; i < n; ++i) {
            for (size_t j = 0; j < f; ++j) Xa({i, j}) = X({i, j});
            if (fit_intercept_) Xa({i, f}) = 1.0f;
        }

        // Xt = Xa^T: [fa, n]
        auto Xt = ts::DefaultComputationPolicy::transpose(Xa);
        // XtX = Xa^T * Xa: [fa, fa]
        auto XtX = ts::dot(Xt, Xa);
        // Add ridge: XtX += lambda * I (skip bias column)
        if (lambda_ > 0.0f) {
            const size_t reg_f = fit_intercept_ ? f : fa;  // don't regularize intercept
            for (size_t i = 0; i < reg_f; ++i)
                XtX({i, i}) += lambda_;
        }
        // XtY = Xa^T * y: [fa]
        auto XtY = ts::dot(Xt, y);
        // Solve XtX * w = XtY
        weights_ = solve_symmetric(XtX, XtY, fa);
        fitted_ = true;
    }

    ts::tensor<float> predict(const ts::tensor<float>& X) const {
        if (!fitted_) throw std::runtime_error("LinearRegression: not fitted");
        const size_t n = X.shape()[0], f = X.shape()[1];
        if (!fit_intercept_) return ts::dot(X, weights_);

        // Augment X with bias column
        const size_t fa = f + 1;
        ts::tensor<float> Xa({n, fa});
        for (size_t i = 0; i < n; ++i) {
            for (size_t j = 0; j < f; ++j) Xa({i, j}) = X({i, j});
            Xa({i, f}) = 1.0f;
        }
        return ts::dot(Xa, weights_);
    }

    // Return feature weights (excluding intercept)
    ts::tensor<float> weights() const noexcept {
        if (!fit_intercept_) return weights_;
        const size_t f = weights_.size() - 1;
        ts::tensor<float> w({f});
        for (size_t i = 0; i < f; ++i) w({i}) = weights_({i});
        return w;
    }
    float intercept() const noexcept {
        if (!fit_intercept_ || !fitted_) return 0.0f;
        return weights_({weights_.size() - 1});
    }

private:
    float lambda_;
    bool  fit_intercept_;
    bool  fitted_ = false;
    ts::tensor<float> weights_;  // [f+1] if fit_intercept, else [f]

    // Gaussian elimination for symmetric positive-definite system A*x = b, size n
    static ts::tensor<float> solve_symmetric(ts::tensor<float> A, ts::tensor<float> b, size_t n) {
        // Augmented matrix [A | b]
        std::vector<std::vector<double>> aug(n, std::vector<double>(n + 1));
        for (size_t i = 0; i < n; ++i) {
            for (size_t j = 0; j < n; ++j)
                aug[i][j] = static_cast<double>(A({i, j}));
            aug[i][n] = static_cast<double>(b({i}));
        }
        for (size_t col = 0; col < n; ++col) {
            size_t pivot = col;
            for (size_t row = col + 1; row < n; ++row)
                if (std::abs(aug[row][col]) > std::abs(aug[pivot][col])) pivot = row;
            std::swap(aug[col], aug[pivot]);
            if (std::abs(aug[col][col]) < 1e-12) throw std::runtime_error("Singular matrix in LinearRegression");
            double inv = 1.0 / aug[col][col];
            for (size_t row = 0; row < n; ++row) {
                if (row == col) continue;
                double factor = aug[row][col] * inv;
                for (size_t k = col; k <= n; ++k)
                    aug[row][k] -= factor * aug[col][k];
            }
            for (size_t k = col + 1; k <= n; ++k) aug[col][k] *= inv;
            aug[col][col] = 1.0;
        }
        ts::tensor<float> x({n});
        for (size_t i = 0; i < n; ++i)
            x({i}) = static_cast<float>(aug[i][n]);
        return x;
    }
};

// Alias
using RidgeRegression = LinearRegression<NormalEquationsSolver>;

// ── Logistic Regression (binary, L2, gradient descent) ──
class LogisticRegression {
public:
    explicit LogisticRegression(float lr = 0.1f, int max_iter = 200, float lambda = 1e-4f)
        : lr_{lr}, max_iter_{max_iter}, lambda_{lambda} {}

    void fit(const ts::tensor<float>& X, const ts::tensor<float>& y) {
        // y must be {0, 1} labels as float
        const size_t n = X.shape()[0], f = X.shape()[1];
        // Initialize weights to zero
        weights_ = ts::tensor<float>({f});
        for (size_t j = 0; j < f; ++j) weights_({j}) = 0.0f;
        bias_ = 0.0f;

        for (int iter = 0; iter < max_iter_; ++iter) {
            // logits = X*w + b, shape [N]
            ts::tensor<float> dw({f});
            for (size_t j = 0; j < f; ++j) dw({j}) = 0.0f;
            float db = 0.0f;
            float inv_n = 1.0f / static_cast<float>(n);

            for (size_t i = 0; i < n; ++i) {
                float logit = bias_;
                for (size_t j = 0; j < f; ++j)
                    logit += X({i, j}) * weights_({j});
                float pred = 1.0f / (1.0f + std::exp(-logit));
                float err = pred - y({i});
                for (size_t j = 0; j < f; ++j)
                    dw({j}) += err * X({i, j}) * inv_n;
                db += err * inv_n;
            }
            // L2 gradient
            for (size_t j = 0; j < f; ++j)
                dw({j}) += lambda_ * weights_({j});
            // Update
            for (size_t j = 0; j < f; ++j)
                weights_({j}) -= lr_ * dw({j});
            bias_ -= lr_ * db;
        }
        fitted_ = true;
    }

    ts::tensor<float> predict_proba(const ts::tensor<float>& X) const {
        if (!fitted_) throw std::runtime_error("LogisticRegression: not fitted");
        const size_t n = X.shape()[0], f = X.shape()[1];
        ts::tensor<float> out({n});
        for (size_t i = 0; i < n; ++i) {
            float logit = bias_;
            for (size_t j = 0; j < f; ++j)
                logit += X({i, j}) * weights_({j});
            out({i}) = 1.0f / (1.0f + std::exp(-logit));
        }
        return out;
    }

    ts::tensor<float> predict(const ts::tensor<float>& X) const {
        auto p = predict_proba(X);
        ts::tensor<float> labels({p.shape()[0]});
        for (size_t i = 0; i < p.shape()[0]; ++i)
            labels({i}) = p({i}) >= 0.5f ? 1.0f : 0.0f;
        return labels;
    }

    const ts::tensor<float>& weights() const noexcept { return weights_; }
    float bias() const noexcept { return bias_; }

private:
    float lr_; int max_iter_; float lambda_;
    bool fitted_ = false;
    ts::tensor<float> weights_;
    float bias_ = 0.0f;
};

} // namespace manas::ml
