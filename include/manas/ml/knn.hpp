#pragma once
// K-Nearest Neighbors: classification (majority vote) and regression (mean)
// Distance policy: L2Dist, L1Dist, CosineDistance
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_map>
#include <vector>
#include <containers/tensor/tensor.hpp>

namespace manas::ml {

struct L2Distance {
    float operator()(const ts::tensor<float>& X, size_t i, size_t j, size_t f) const noexcept {
        float d = 0.0f;
        for (size_t k = 0; k < f; ++k) { float v = X({i,k}) - X({j,k}); d += v*v; }
        return std::sqrt(d);
    }
    float operator()(const ts::tensor<float>& X, size_t i,
                     const ts::tensor<float>& Q, size_t qi, size_t f) const noexcept {
        float d = 0.0f;
        for (size_t k = 0; k < f; ++k) { float v = X({i,k}) - Q({qi,k}); d += v*v; }
        return std::sqrt(d);
    }
};

struct L1Distance {
    float operator()(const ts::tensor<float>& X, size_t i,
                     const ts::tensor<float>& Q, size_t qi, size_t f) const noexcept {
        float d = 0.0f;
        for (size_t k = 0; k < f; ++k) d += std::abs(X({i,k}) - Q({qi,k}));
        return d;
    }
};

template<typename DistPolicy = L2Distance>
class KNNClassifier {
public:
    explicit KNNClassifier(int k, DistPolicy dist = {})
        : k_{k}, dist_{std::move(dist)} {}

    void fit(const ts::tensor<float>& X, const ts::tensor<float>& y) {
        X_train_ = X; y_train_ = y; fitted_ = true;
    }

    ts::tensor<float> predict(const ts::tensor<float>& X) const {
        if (!fitted_) throw std::runtime_error("KNNClassifier: not fitted");
        const size_t nq = X.shape()[0], f = X.shape()[1];
        const size_t n  = X_train_.shape()[0];
        ts::tensor<float> out({nq});

        for (size_t qi = 0; qi < nq; ++qi) {
            std::vector<std::pair<float,float>> dists;
            dists.reserve(n);
            for (size_t i = 0; i < n; ++i)
                dists.emplace_back(dist_(X_train_, i, X, qi, f), y_train_({i}));
            int kk = std::min(k_, static_cast<int>(n));
            std::partial_sort(dists.begin(), dists.begin() + kk, dists.end(),
                              [](auto& a, auto& b){ return a.first < b.first; });
            std::unordered_map<float, int> votes;
            for (int t = 0; t < kk; ++t) votes[dists[t].second]++;
            float best_label = dists[0].second; int best_cnt = 0;
            for (auto& [lbl, cnt] : votes) if (cnt > best_cnt) { best_cnt = cnt; best_label = lbl; }
            out({qi}) = best_label;
        }
        return out;
    }

private:
    int k_; DistPolicy dist_; bool fitted_ = false;
    ts::tensor<float> X_train_, y_train_;
};

template<typename DistPolicy = L2Distance>
class KNNRegressor {
public:
    explicit KNNRegressor(int k, DistPolicy dist = {})
        : k_{k}, dist_{std::move(dist)} {}

    void fit(const ts::tensor<float>& X, const ts::tensor<float>& y) {
        X_train_ = X; y_train_ = y; fitted_ = true;
    }

    ts::tensor<float> predict(const ts::tensor<float>& X) const {
        if (!fitted_) throw std::runtime_error("KNNRegressor: not fitted");
        const size_t nq = X.shape()[0], f = X.shape()[1];
        const size_t n  = X_train_.shape()[0];
        ts::tensor<float> out({nq});

        for (size_t qi = 0; qi < nq; ++qi) {
            std::vector<std::pair<float,float>> dists;
            dists.reserve(n);
            for (size_t i = 0; i < n; ++i)
                dists.emplace_back(dist_(X_train_, i, X, qi, f), y_train_({i}));
            int kk = std::min(k_, static_cast<int>(n));
            std::partial_sort(dists.begin(), dists.begin() + kk, dists.end(),
                              [](auto& a, auto& b){ return a.first < b.first; });
            float s = 0.0f;
            for (int t = 0; t < kk; ++t) s += dists[t].second;
            out({qi}) = s / static_cast<float>(kk);
        }
        return out;
    }

private:
    int k_; DistPolicy dist_; bool fitted_ = false;
    ts::tensor<float> X_train_, y_train_;
};

} // namespace manas::ml
