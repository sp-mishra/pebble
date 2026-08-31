#pragma once
// Gaussian Naive Bayes classifier
// Assumes feature independence within each class; models each feature as Gaussian.
#include <cmath>
#include <stdexcept>
#include <unordered_map>
#include <vector>
#include <containers/tensor/tensor.hpp>

namespace manas::ml {

class GaussianNaiveBayes {
public:
    explicit GaussianNaiveBayes(float var_smoothing = 1e-9f)
        : var_smoothing_{var_smoothing} {}

    // X: [N, F], y: [N] (class labels as floats, must be integers 0,1,...,C-1)
    void fit(const ts::tensor<float>& X, const ts::tensor<float>& y) {
        const size_t n = X.shape()[0], f = X.shape()[1];
        // Find classes
        classes_.clear(); class_to_idx_.clear();
        for (size_t i = 0; i < n; ++i) {
            float lbl = y({i});
            if (class_to_idx_.find(lbl) == class_to_idx_.end()) {
                class_to_idx_[lbl] = static_cast<int>(classes_.size());
                classes_.push_back(lbl);
            }
        }
        const int nc = static_cast<int>(classes_.size());

        // Count per class
        std::vector<int> counts(nc, 0);
        for (size_t i = 0; i < n; ++i) counts[class_to_idx_.at(y({i}))]++;

        // Class priors (log)
        log_priors_.resize(nc);
        for (int c = 0; c < nc; ++c)
            log_priors_[c] = std::log(static_cast<float>(counts[c]) / static_cast<float>(n));

        // Per-class mean and variance
        theta_.assign(nc, std::vector<float>(f, 0.0f));
        sigma_.assign(nc, std::vector<float>(f, 0.0f));

        for (size_t i = 0; i < n; ++i) {
            int c = class_to_idx_.at(y({i}));
            for (size_t j = 0; j < f; ++j) theta_[c][j] += X({i, j});
        }
        for (int c = 0; c < nc; ++c)
            for (size_t j = 0; j < f; ++j)
                theta_[c][j] /= static_cast<float>(counts[c]);

        for (size_t i = 0; i < n; ++i) {
            int c = class_to_idx_.at(y({i}));
            for (size_t j = 0; j < f; ++j) {
                float d = X({i, j}) - theta_[c][j];
                sigma_[c][j] += d * d;
            }
        }
        for (int c = 0; c < nc; ++c)
            for (size_t j = 0; j < f; ++j)
                sigma_[c][j] = sigma_[c][j] / static_cast<float>(counts[c]) + var_smoothing_;

        n_features_ = f; fitted_ = true;
    }

    ts::tensor<float> predict(const ts::tensor<float>& X) const {
        if (!fitted_) throw std::runtime_error("GaussianNaiveBayes: not fitted");
        const size_t n = X.shape()[0];
        ts::tensor<float> out({n});
        for (size_t i = 0; i < n; ++i) {
            auto log_probs = compute_log_probs(X, i);
            int best = 0;
            for (int c = 1; c < static_cast<int>(classes_.size()); ++c)
                if (log_probs[c] > log_probs[best]) best = c;
            out({i}) = classes_[best];
        }
        return out;
    }

    ts::tensor<float> predict_log_proba(const ts::tensor<float>& X) const {
        if (!fitted_) throw std::runtime_error("GaussianNaiveBayes: not fitted");
        const size_t n = X.shape()[0];
        const int nc = static_cast<int>(classes_.size());
        ts::tensor<float> out({n, static_cast<size_t>(nc)});
        for (size_t i = 0; i < n; ++i) {
            auto lp = compute_log_probs(X, i);
            for (int c = 0; c < nc; ++c) out({i, static_cast<size_t>(c)}) = lp[c];
        }
        return out;
    }

private:
    float var_smoothing_;
    bool fitted_ = false;
    size_t n_features_ = 0;
    std::vector<float> classes_;
    std::unordered_map<float, int> class_to_idx_;
    std::vector<float> log_priors_;
    std::vector<std::vector<float>> theta_, sigma_;

    std::vector<float> compute_log_probs(const ts::tensor<float>& X, size_t i) const {
        const int nc = static_cast<int>(classes_.size());
        std::vector<float> lp(nc);
        static constexpr float log2pi = 1.8378770664093455f;
        for (int c = 0; c < nc; ++c) {
            float lp_c = log_priors_[c];
            for (size_t j = 0; j < n_features_; ++j) {
                float x = X({i, j});
                float mu = theta_[c][j], sig2 = sigma_[c][j];
                lp_c -= 0.5f * (log2pi + std::log(sig2) + (x - mu) * (x - mu) / sig2);
            }
            lp[c] = lp_c;
        }
        return lp;
    }
};

} // namespace manas::ml
