#pragma once
// Decision Tree: CART (Classification & Regression Trees)
// Policy: SplitCriterion (Gini, Entropy, MSE), MaxDepth, MinSamplesSplit
// Header-only, no virtual, recursive node ownership via unique_ptr
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <vector>
#include <containers/tensor/tensor.hpp>

namespace manas::ml {
    struct GiniCriterion {};

    struct EntropyCriterion {};

    struct MSECriterion {}; // for regression

    namespace detail {
        inline float gini(std::span<const float> labels, size_t n) {
            std::unordered_map<float, int> counts;
            for (float v : labels) counts[v]++;
            float g = 1.0f;
            for (auto& [k, c] : counts) {
                float p = static_cast<float>(c) / n;
                g -= p * p;
            }
            return g;
        }

        inline float entropy(std::span<const float> labels, size_t n) {
            std::unordered_map<float, int> counts;
            for (float v : labels) counts[v]++;
            float e = 0.0f;
            for (auto& [k, c] : counts) {
                if (c > 0) {
                    float p = static_cast<float>(c) / n;
                    e -= p * std::log2(p);
                }
            }
            return e;
        }

        inline float mse(std::span<const float> labels, size_t n) {
            if (n == 0) return 0.0f;
            float mean = 0.0f;
            for (float v : labels) mean += v;
            mean /= static_cast<float>(n);
            float s = 0.0f;
            for (float v : labels) {
                float d = v - mean;
                s += d * d;
            }
            return s / static_cast<float>(n);
        }

        inline float majority_class(const std::vector<float>& labels) {
            std::unordered_map<float, int> counts;
            for (float v : labels) counts[v]++;
            float best = labels[0];
            int best_cnt = 0;
            for (auto& [k, c] : counts) if (c > best_cnt) {
                best_cnt = c;
                best = k;
            }
            return best;
        }

        inline float mean_value(const std::vector<float>& labels) {
            float s = 0.0f;
            for (float v : labels) s += v;
            return s / static_cast<float>(labels.size());
        }

        struct Node {
            int feature = -1;
            float threshold = 0.0f;
            float value = 0.0f; // leaf prediction
            bool is_leaf = false;
            std::unique_ptr<Node> left, right;
        };
    } // namespace detail

    template <typename CriterionPolicy = GiniCriterion>
    class DecisionTree {
    public:
        explicit DecisionTree(int max_depth = 10, int min_samples_split = 2,
                              int min_samples_leaf = 1, uint64_t seed = 42)
            : max_depth_{max_depth}, min_samples_split_{min_samples_split},
              min_samples_leaf_{min_samples_leaf}, seed_{seed} {}

        void fit(const ts::tensor<float>& X, const ts::tensor<float>& y) {
            const size_t n = X.shape()[0], f = X.shape()[1];
            n_features_ = f;
            std::vector<size_t> indices(n);
            for (size_t i = 0; i < n; ++i) indices[i] = i;
            root_ = build(X, y, indices, 0);
            fitted_ = true;
        }

        ts::tensor<float> predict(const ts::tensor<float>& X) const {
            if (!fitted_) throw std::runtime_error("DecisionTree: not fitted");
            const size_t n = X.shape()[0];
            ts::tensor<float> out({n});
            for (size_t i = 0; i < n; ++i) out({i}) = predict_one(X, i, root_.get());
            return out;
        }

    private:
        int max_depth_, min_samples_split_, min_samples_leaf_;
        uint64_t seed_;
        size_t n_features_ = 0;
        bool fitted_ = false;
        std::unique_ptr<detail::Node> root_;

        float impurity(const std::vector<float>& labels) const {
            if (labels.empty()) return 0.0f;
            std::span<const float> sp(labels);
            if constexpr (std::is_same_v<CriterionPolicy, GiniCriterion>)
                return detail::gini(sp, labels.size());
            else if constexpr (std::is_same_v<CriterionPolicy, EntropyCriterion>)
                return detail::entropy(sp, labels.size());
            else
                return detail::mse(sp, labels.size());
        }

        float leaf_value(const std::vector<float>& labels) const {
            if constexpr (std::is_same_v<CriterionPolicy, MSECriterion>)
                return detail::mean_value(labels);
            else
                return detail::majority_class(labels);
        }

        std::unique_ptr<detail::Node> build(const ts::tensor<float>& X,
                                            const ts::tensor<float>& y,
                                            const std::vector<size_t>& indices,
                                            int depth) {
            auto node = std::make_unique<detail::Node>();
            std::vector<float> labels;
            labels.reserve(indices.size());
            for (size_t idx : indices) labels.push_back(y({idx}));

            if (depth >= max_depth_ ||
                static_cast<int>(indices.size()) < min_samples_split_ ||
                impurity(labels) < 1e-7f) {
                node->is_leaf = true;
                node->value = leaf_value(labels);
                return node;
            }

            // Find best split
            float best_gain = -std::numeric_limits<float>::max();
            int best_feat = -1;
            float best_thresh = 0.0f;
            float cur_imp = impurity(labels);

            for (size_t f = 0; f < n_features_; ++f) {
                // Collect unique thresholds
                std::vector<float> vals;
                vals.reserve(indices.size());
                for (size_t idx : indices) vals.push_back(X({idx, f}));
                std::sort(vals.begin(), vals.end());
                vals.erase(std::unique(vals.begin(), vals.end()), vals.end());
                for (size_t vi = 0; vi + 1 < vals.size(); ++vi) {
                    float thresh = (vals[vi] + vals[vi + 1]) * 0.5f;
                    std::vector<float> left_labels, right_labels;
                    for (size_t idx : indices) {
                        if (X({idx, f}) <= thresh) left_labels.push_back(y({idx}));
                        else right_labels.push_back(y({idx}));
                    }
                    if (static_cast<int>(left_labels.size()) < min_samples_leaf_ ||
                        static_cast<int>(right_labels.size()) < min_samples_leaf_)
                        continue;
                    float nl = static_cast<float>(left_labels.size());
                    float nr = static_cast<float>(right_labels.size());
                    float nt = nl + nr;
                    float gain = cur_imp - (nl / nt) * impurity(left_labels) - (nr / nt) * impurity(right_labels);
                    if (gain > best_gain) {
                        best_gain = gain;
                        best_feat = static_cast<int>(f);
                        best_thresh = thresh;
                    }
                }
            }

            if (best_feat == -1) {
                node->is_leaf = true;
                node->value = leaf_value(labels);
                return node;
            }

            node->feature = best_feat;
            node->threshold = best_thresh;
            std::vector<size_t> left_idx, right_idx;
            for (size_t idx : indices) {
                if (X({idx, static_cast<size_t>(best_feat)}) <= best_thresh) left_idx.push_back(idx);
                else right_idx.push_back(idx);
            }
            node->left = build(X, y, left_idx, depth + 1);
            node->right = build(X, y, right_idx, depth + 1);
            return node;
        }

        float predict_one(const ts::tensor<float>& X, size_t i, const detail::Node* node) const {
            if (node->is_leaf) return node->value;
            float val = X({i, static_cast<size_t>(node->feature)});
            if (val <= node->threshold) return predict_one(X, i, node->left.get());
            return predict_one(X, i, node->right.get());
        }
    };

    // Convenience aliases
    using DecisionTreeClassifier = DecisionTree<GiniCriterion>;
    using DecisionTreeRegressor = DecisionTree<MSECriterion>;
} // namespace manas::ml
