#pragma once
// Random Forest: Bagging ensemble of DecisionTrees with feature subsampling
// Policy: base tree criterion, n_estimators, max_features
#include <algorithm>
#include <cmath>
#include <random>
#include <stdexcept>
#include <unordered_map>
#include <vector>
#include <containers/tensor/tensor.hpp>
#include "decision_tree.hpp"

namespace manas::ml {
    // SqrtFeatures: sqrt(n_features), LogFeatures: log2(n_features)+1
    struct SqrtFeatures {
        static size_t count(size_t f) {
            return std::max(size_t{1}, static_cast<size_t>(std::sqrt(static_cast<float>(f))));
        }
    };

    struct LogFeatures {
        static size_t count(size_t f) {
            return std::max(size_t{1}, static_cast<size_t>(std::log2(static_cast<float>(f)) + 1));
        }
    };

    struct AllFeatures {
        static size_t count(size_t f) { return f; }
    };

    template <typename CriterionPolicy = GiniCriterion,
              typename MaxFeatPolicy = SqrtFeatures>
    class RandomForest {
    public:
        explicit RandomForest(int n_estimators = 100, int max_depth = 10,
                              int min_samples_split = 2, uint64_t seed = 42)
            : n_estimators_{n_estimators}, max_depth_{max_depth},
              min_samples_split_{min_samples_split}, seed_{seed} {}

        void fit(const ts::tensor<float>& X, const ts::tensor<float>& y) {
            const size_t n = X.shape()[0], f = X.shape()[1];
            n_features_ = f;
            trees_.clear();
            trees_.reserve(static_cast<size_t>(n_estimators_));
            std::mt19937_64 rng(seed_);

            for (int t = 0; t < n_estimators_; ++t) {
                // Bootstrap sample
                std::uniform_int_distribution<size_t> row_dist(0, n - 1);
                ts::tensor<float> Xb({n, f}), yb({n});
                for (size_t i = 0; i < n; ++i) {
                    size_t src = row_dist(rng);
                    for (size_t j = 0; j < f; ++j) Xb({i, j}) = X({src, j});
                    yb({i}) = y({src});
                }
                // Feature subsample
                size_t n_sel = MaxFeatPolicy::count(f);
                std::vector<size_t> feat_idx(f);
                for (size_t j = 0; j < f; ++j) feat_idx[j] = j;
                std::shuffle(feat_idx.begin(), feat_idx.end(), rng);
                feat_idx.resize(n_sel);
                std::sort(feat_idx.begin(), feat_idx.end());

                ts::tensor<float> Xs({n, n_sel});
                for (size_t i = 0; i < n; ++i)
                    for (size_t j = 0; j < n_sel; ++j)
                        Xs({i, j}) = Xb({i, feat_idx[j]});

                DecisionTree<CriterionPolicy> tree(max_depth_, min_samples_split_, 1, seed_ + static_cast<uint64_t>(t));
                tree.fit(Xs, yb);
                trees_.emplace_back(std::move(tree), std::move(feat_idx));
            }
            fitted_ = true;
        }

        ts::tensor<float> predict(const ts::tensor<float>& X) const {
            if (!fitted_) throw std::runtime_error("RandomForest: not fitted");
            const size_t n = X.shape()[0];
            ts::tensor<float> out({n});

            for (size_t i = 0; i < n; ++i) {
                std::unordered_map<float, int> votes;
                for (auto& [tree, feat_idx] : trees_) {
                    size_t n_sel = feat_idx.size();
                    ts::tensor<float> xi({1, n_sel});
                    for (size_t j = 0; j < n_sel; ++j) xi({0, j}) = X({i, feat_idx[j]});
                    auto pred = tree.predict(xi);
                    votes[pred({0})]++;
                }
                float best_lbl = 0.0f;
                int best_cnt = 0;
                for (auto& [lbl, cnt] : votes)
                    if (cnt > best_cnt) {
                        best_cnt = cnt;
                        best_lbl = lbl;
                    }
                out({i}) = best_lbl;
            }
            return out;
        }

        int n_estimators() const noexcept { return n_estimators_; }

    private:
        int n_estimators_, max_depth_, min_samples_split_;
        uint64_t seed_;
        size_t n_features_ = 0;
        bool fitted_ = false;

        struct TreeEntry {
            DecisionTree<CriterionPolicy> tree;
            std::vector<size_t> feat_idx;

            TreeEntry(DecisionTree<CriterionPolicy> t, std::vector<size_t> fi)
                : tree(std::move(t)), feat_idx(std::move(fi)) {}
        };

        std::vector<TreeEntry> trees_;
    };

    using RandomForestClassifier = RandomForest<GiniCriterion>;
    using RandomForestRegressor = RandomForest<MSECriterion>;
} // namespace manas::ml
