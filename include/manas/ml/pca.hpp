#pragma once
// PCA: Principal Component Analysis via covariance matrix eigen-decomposition
// Uses power iteration for top-k eigenvectors (production-quality, no LAPACK dep)
#include <cmath>
#include <stdexcept>
#include <containers/tensor/tensor.hpp>

namespace manas::ml {
    class PCA {
    public:
        explicit PCA(int n_components, int max_iter = 1000, float tol = 1e-6f)
            : n_components_{n_components}, max_iter_{max_iter}, tol_{tol} {}

        void fit(const ts::tensor<float>& X) {
            const size_t n = X.shape()[0], f = X.shape()[1];
            if (n_components_ > static_cast<int>(f))
                throw std::invalid_argument("n_components > n_features");

            // Compute mean
            mean_ = ts::tensor<float>({f});
            for (size_t j = 0; j < f; ++j) {
                float s = 0.0f;
                for (size_t i = 0; i < n; ++i) s += X({i, j});
                mean_({j}) = s / static_cast<float>(n);
            }

            // Center X
            ts::tensor<float> Xc({n, f});
            for (size_t i = 0; i < n; ++i)
                for (size_t j = 0; j < f; ++j)
                    Xc({i, j}) = X({i, j}) - mean_({j});

            // Covariance: C = Xc^T * Xc / (n-1), shape [F, F]
            ts::tensor<float> C({f, f});
            float inv_n1 = 1.0f / static_cast<float>(n - 1);
            for (size_t a = 0; a < f; ++a)
                for (size_t b = a; b < f; ++b) {
                    float s = 0.0f;
                    for (size_t i = 0; i < n; ++i) s += Xc({i, a}) * Xc({i, b});
                    C({a, b}) = C({b, a}) = s * inv_n1;
                }

            // Power iteration deflation for top n_components eigenvectors
            components_ = ts::tensor<float>({static_cast<size_t>(n_components_), f});
            explained_variance_ = ts::tensor<float>({static_cast<size_t>(n_components_)});

            ts::tensor<float> Cd = C; // deflated copy
            for (int k = 0; k < n_components_; ++k) {
                // Power iteration
                std::vector<float> v(f, 1.0f / std::sqrt(static_cast<float>(f)));
                for (int it = 0; it < max_iter_; ++it) {
                    std::vector<float> Av(f, 0.0f);
                    for (size_t a = 0; a < f; ++a)
                        for (size_t b = 0; b < f; ++b)
                            Av[a] += Cd({a, b}) * v[b];
                    float norm = 0.0f;
                    for (float x : Av) norm += x * x;
                    norm = std::sqrt(norm);
                    if (norm < 1e-12f) break;
                    float prev_dot = 0.0f;
                    for (size_t j = 0; j < f; ++j) prev_dot += v[j] * (Av[j] / norm);
                    for (size_t j = 0; j < f; ++j) v[j] = Av[j] / norm;
                    if (std::abs(prev_dot - 1.0f) < tol_) break;
                }
                // eigenvalue = v^T * C * v
                float eigenval = 0.0f;
                for (size_t a = 0; a < f; ++a) {
                    float Cv_a = 0.0f;
                    for (size_t b = 0; b < f; ++b) Cv_a += Cd({a, b}) * v[b];
                    eigenval += v[a] * Cv_a;
                }
                explained_variance_({static_cast<size_t>(k)}) = eigenval;
                for (size_t j = 0; j < f; ++j)
                    components_({static_cast<size_t>(k), j}) = v[j];
                // Deflate: Cd -= eigenval * v * v^T
                for (size_t a = 0; a < f; ++a)
                    for (size_t b = 0; b < f; ++b)
                        Cd({a, b}) -= eigenval * v[a] * v[b];
            }
            fitted_ = true;
        }

        ts::tensor<float> transform(const ts::tensor<float>& X) const {
            if (!fitted_) throw std::runtime_error("PCA: not fitted");
            const size_t n = X.shape()[0], f = X.shape()[1];
            ts::tensor<float> out({n, static_cast<size_t>(n_components_)});
            for (size_t i = 0; i < n; ++i) {
                for (int k = 0; k < n_components_; ++k) {
                    float dot = 0.0f;
                    for (size_t j = 0; j < f; ++j)
                        dot += (X({i, j}) - mean_({j})) * components_({static_cast<size_t>(k), j});
                    out({i, static_cast<size_t>(k)}) = dot;
                }
            }
            return out;
        }

        void fit_transform(const ts::tensor<float>& X, ts::tensor<float>& out) {
            fit(X);
            out = transform(X);
        }

        const ts::tensor<float>& components() const noexcept { return components_; }
        const ts::tensor<float>& mean() const noexcept { return mean_; }
        const ts::tensor<float>& explained_variance() const noexcept { return explained_variance_; }

    private:
        int n_components_, max_iter_;
        float tol_;
        bool fitted_ = false;
        ts::tensor<float> mean_, components_, explained_variance_;
    };
} // namespace manas::ml
