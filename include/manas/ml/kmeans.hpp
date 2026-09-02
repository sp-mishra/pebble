#pragma once
// K-Means clustering with Lloyd's algorithm
// Policy: distance metric, initialization (Random, KMeansPP)
#include <cmath>
#include <limits>
#include <random>
#include <stdexcept>
#include <vector>
#include <containers/tensor/tensor.hpp>

namespace manas::ml {
    struct RandomInit {};

    struct KMeansPPInit {};

    template <typename InitPolicy = KMeansPPInit>
    class KMeans {
    public:
        explicit KMeans(int k, int max_iter = 300, float tol = 1e-4f, uint64_t seed = 42)
            : k_{k}, max_iter_{max_iter}, tol_{tol}, seed_{seed} {}

        void fit(const ts::tensor<float>& X) {
            const size_t n = X.shape()[0], f = X.shape()[1];
            if (static_cast<size_t>(k_) > n) throw std::invalid_argument("k > n_samples");

            // Initialize centroids
            centers_ = init_centroids(X, n, f);

            labels_.assign(n, 0);
            for (int iter = 0; iter < max_iter_; ++iter) {
                // Assign step
                bool changed = false;
                for (size_t i = 0; i < n; ++i) {
                    int best = 0;
                    float best_d = std::numeric_limits<float>::max();
                    for (int c = 0; c < k_; ++c) {
                        float d = sq_dist(X, i, f, c);
                        if (d < best_d) {
                            best_d = d;
                            best = c;
                        }
                    }
                    if (labels_[i] != best) {
                        labels_[i] = best;
                        changed = true;
                    }
                }
                // Update step
                std::vector<float> counts(k_, 0.0f);
                ts::tensor<float> new_centers({static_cast<size_t>(k_), f});
                for (int c = 0; c < k_; ++c)
                    for (size_t j = 0; j < f; ++j) new_centers({static_cast<size_t>(c), j}) = 0.0f;
                for (size_t i = 0; i < n; ++i) {
                    size_t c = static_cast<size_t>(labels_[i]);
                    counts[c] += 1.0f;
                    for (size_t j = 0; j < f; ++j)
                        new_centers({c, j}) += X({i, j});
                }
                float max_shift = 0.0f;
                for (int c = 0; c < k_; ++c) {
                    if (counts[c] > 0.0f) {
                        float shift = 0.0f;
                        for (size_t j = 0; j < f; ++j) {
                            float v = new_centers({static_cast<size_t>(c), j}) / counts[c];
                            float d = v - centers_({static_cast<size_t>(c), j});
                            shift += d * d;
                            centers_({static_cast<size_t>(c), j}) = v;
                        }
                        max_shift = std::max(max_shift, shift);
                    }
                }
                if (!changed || std::sqrt(max_shift) < tol_) break;
            }
            inertia_ = compute_inertia(X, n, f);
            fitted_ = true;
        }

        ts::tensor<int> predict(const ts::tensor<float>& X) const {
            if (!fitted_) throw std::runtime_error("KMeans: not fitted");
            const size_t n = X.shape()[0], f = X.shape()[1];
            ts::tensor<int> out({n});
            for (size_t i = 0; i < n; ++i) {
                int best = 0;
                float best_d = std::numeric_limits<float>::max();
                for (int c = 0; c < k_; ++c) {
                    float d = sq_dist(X, i, f, c);
                    if (d < best_d) {
                        best_d = d;
                        best = c;
                    }
                }
                out({i}) = best;
            }
            return out;
        }

        const ts::tensor<float>& cluster_centers() const noexcept { return centers_; }
        const std::vector<int>& labels() const noexcept { return labels_; }
        float inertia() const noexcept { return inertia_; }

    private:
        int k_, max_iter_;
        float tol_;
        uint64_t seed_;
        bool fitted_ = false;
        ts::tensor<float> centers_;
        std::vector<int> labels_;
        float inertia_ = 0.0f;

        ts::tensor<float> init_centroids(const ts::tensor<float>& X, size_t n, size_t f) const {
            std::mt19937_64 rng(seed_);
            ts::tensor<float> c({static_cast<size_t>(k_), f});
            if constexpr (std::is_same_v<InitPolicy, RandomInit>) {
                std::uniform_int_distribution<size_t> dist(0, n - 1);
                for (int ci = 0; ci < k_; ++ci) {
                    size_t idx = dist(rng);
                    for (size_t j = 0; j < f; ++j) c({static_cast<size_t>(ci), j}) = X({idx, j});
                }
            }
            else { // KMeans++
                std::uniform_int_distribution<size_t> first(0, n - 1);
                size_t idx = first(rng);
                for (size_t j = 0; j < f; ++j) c({0, j}) = X({idx, j});
                for (int ci = 1; ci < k_; ++ci) {
                    std::vector<float> dists(n);
                    for (size_t i = 0; i < n; ++i) {
                        float min_d = std::numeric_limits<float>::max();
                        for (int cj = 0; cj < ci; ++cj) {
                            float d = 0.0f;
                            for (size_t j = 0; j < f; ++j) {
                                float dv = X({i, j}) - c({static_cast<size_t>(cj), j});
                                d += dv * dv;
                            }
                            min_d = std::min(min_d, d);
                        }
                        dists[i] = min_d;
                    }
                    std::discrete_distribution<size_t> weighted(dists.begin(), dists.end());
                    size_t chosen = weighted(rng);
                    for (size_t j = 0; j < f; ++j) c({static_cast<size_t>(ci), j}) = X({chosen, j});
                }
            }
            return c;
        }

        float sq_dist(const ts::tensor<float>& X, size_t i, size_t f, int c) const {
            float d = 0.0f;
            for (size_t j = 0; j < f; ++j) {
                float v = X({i, j}) - centers_({static_cast<size_t>(c), j});
                d += v * v;
            }
            return d;
        }

        float compute_inertia(const ts::tensor<float>& X, size_t n, size_t f) const {
            float inertia = 0.0f;
            for (size_t i = 0; i < n; ++i)
                inertia += sq_dist(X, i, f, labels_[i]);
            return inertia;
        }
    };
} // namespace manas::ml
