#pragma once
// Support Vector Machine: binary SVM with kernel policy, SMO-lite training
// C++23, no virtual, policy-based kernel, header-only
#include <cmath>
#include <random>
#include <stdexcept>
#include <vector>
#include <containers/tensor/tensor.hpp>
#include "kernels.hpp"

namespace manas::ml {
    // SMO (Sequential Minimal Optimization) binary SVM
    // KernelPolicy: LinearKernel, RBFKernel, PolynomialKernel, etc.
    template <typename KernelPolicy = RBFKernel>
    class SVM {
    public:
        explicit SVM(float C = 1.0f, float tol = 1e-3f, int max_iter = 200,
                     KernelPolicy kernel = {})
            : C_{C}, tol_{tol}, max_iter_{max_iter}, kernel_{std::move(kernel)} {}

        // X: [N, F], y: {-1.0f, +1.0f}
        void fit(const ts::tensor<float>& X, const ts::tensor<float>& y) {
            const size_t n = X.shape()[0];
            const size_t f = X.shape()[1];
            alphas_.assign(n, 0.0f);
            b_ = 0.0f;
            X_sv_.clear();
            y_sv_.clear();

            // Cache kernel matrix
            std::vector<std::vector<float>> K(n, std::vector<float>(n));
            for (size_t i = 0; i < n; ++i)
                for (size_t j = 0; j <= i; ++j) {
                    auto xi = row_span(X, i, f);
                    auto xj = row_span(X, j, f);
                    K[i][j] = K[j][i] = kernel_(xi, xj);
                }

            std::mt19937 rng(42);
            for (int iter = 0; iter < max_iter_; ++iter) {
                int changed = 0;
                for (size_t i = 0; i < n; ++i) {
                    float yi = y({i});
                    float Ei = decision_raw(K, alphas_, y, n, i) + b_ - yi;
                    if ((yi * Ei < -tol_ && alphas_[i] < C_) ||
                        (yi * Ei > tol_ && alphas_[i] > 0.0f)) {
                        // Pick j randomly != i
                        std::uniform_int_distribution<size_t> dist(0, n - 2);
                        size_t j = dist(rng);
                        if (j >= i) ++j;
                        float yj = y({j});
                        float Ej = decision_raw(K, alphas_, y, n, j) + b_ - yj;
                        float ai_old = alphas_[i], aj_old = alphas_[j];
                        float L, H;
                        if (yi != yj) {
                            L = std::max(0.0f, aj_old - ai_old);
                            H = std::min(C_, C_ + aj_old - ai_old);
                        }
                        else {
                            L = std::max(0.0f, ai_old + aj_old - C_);
                            H = std::min(C_, ai_old + aj_old);
                        }
                        if (L >= H) continue;
                        float eta = 2.0f * K[i][j] - K[i][i] - K[j][j];
                        if (eta >= 0.0f) continue;
                        alphas_[j] -= yj * (Ei - Ej) / eta;
                        alphas_[j] = std::clamp(alphas_[j], L, H);
                        if (std::abs(alphas_[j] - aj_old) < 1e-5f) continue;
                        alphas_[i] += yi * yj * (aj_old - alphas_[j]);
                        float b1 = b_ - Ei - yi * (alphas_[i] - ai_old) * K[i][i] - yj * (alphas_[j] - aj_old) * K[i][
                            j];
                        float b2 = b_ - Ej - yi * (alphas_[i] - ai_old) * K[i][j] - yj * (alphas_[j] - aj_old) * K[j][
                            j];
                        b_ = (alphas_[i] > 0.0f && alphas_[i] < C_)
                                 ? b1
                                 : (alphas_[j] > 0.0f && alphas_[j] < C_)
                                 ? b2
                                 : 0.5f * (b1 + b2);
                        ++changed;
                    }
                }
                if (changed == 0) break;
            }

            // Store support vectors
            for (size_t i = 0; i < n; ++i) {
                if (alphas_[i] > 1e-5f) {
                    std::vector<float> sv(f);
                    for (size_t j = 0; j < f; ++j) sv[j] = X({i, j});
                    X_sv_.push_back(std::move(sv));
                    y_sv_.push_back(y({i}));
                    alpha_sv_.push_back(alphas_[i]);
                }
            }
            fitted_ = true;
        }

        ts::tensor<float> predict(const ts::tensor<float>& X) const {
            if (!fitted_) throw std::runtime_error("SVM: not fitted");
            const size_t n = X.shape()[0], f = X.shape()[1];
            ts::tensor<float> out({n});
            for (size_t i = 0; i < n; ++i) {
                auto xi = row_span(X, i, f);
                float score = b_;
                for (size_t s = 0; s < X_sv_.size(); ++s)
                    score += alpha_sv_[s] * y_sv_[s] * kernel_(xi, std::span<const float>(X_sv_[s]));
                out({i}) = score >= 0.0f ? 1.0f : -1.0f;
            }
            return out;
        }

        ts::tensor<float> decision_function(const ts::tensor<float>& X) const {
            if (!fitted_) throw std::runtime_error("SVM: not fitted");
            const size_t n = X.shape()[0], f = X.shape()[1];
            ts::tensor<float> out({n});
            for (size_t i = 0; i < n; ++i) {
                auto xi = row_span(X, i, f);
                float score = b_;
                for (size_t s = 0; s < X_sv_.size(); ++s)
                    score += alpha_sv_[s] * y_sv_[s] * kernel_(xi, std::span<const float>(X_sv_[s]));
                out({i}) = score;
            }
            return out;
        }

        size_t n_support_vectors() const noexcept { return X_sv_.size(); }

    private:
        float C_, tol_;
        int max_iter_;
        KernelPolicy kernel_;
        bool fitted_ = false;
        std::vector<float> alphas_;
        float b_ = 0.0f;
        std::vector<std::vector<float>> X_sv_;
        std::vector<float> y_sv_, alpha_sv_;

        static std::span<const float> row_span(const ts::tensor<float>& X, size_t i, size_t f) {
            return std::span<const float>(X.data() + i * f, f);
        }

        static float decision_raw(const std::vector<std::vector<float>>& K,
                                  const std::vector<float>& alphas,
                                  const ts::tensor<float>& y,
                                  size_t n, size_t i) {
            float s = 0.0f;
            for (size_t j = 0; j < n; ++j)
                s += alphas[j] * y({j}) * K[j][i];
            return s;
        }
    };
} // namespace manas::ml
