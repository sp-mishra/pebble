#pragma once
#include "genome.hpp"
#include <algorithm>
#include <functional>
#include <random>

namespace manas {
    using CrossoverOperator = std::function<BrainGenome(const BrainGenome &, const BrainGenome &)>;

    // Uniform Crossover: For each weight/bias element, randomly choose from parent A or parent B with equal probability.
    struct UniformCrossover {
        float bias = 0.5f;

        BrainGenome operator()(const BrainGenome& parent_a, const BrainGenome& parent_b) const {
            BrainGenome child = parent_a;
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_real_distribution<float> dist(0.0f, 1.0f);

            for (size_t l = 0; l < child.layer_weights.size() && l < parent_b.layer_weights.size(); ++l) {
                auto& child_w = child.layer_weights[l];
                const auto& b_w = parent_b.layer_weights[l];
                for (size_t i = 0; i < child_w.size() && i < b_w.size(); ++i) {
                    if (dist(gen) > bias) {
                        child_w.data()[i] = b_w.data()[i];
                    }
                }
            }

            for (size_t l = 0; l < child.layer_biases.size() && l < parent_b.layer_biases.size(); ++l) {
                auto& child_b = child.layer_biases[l];
                const auto& b_b = parent_b.layer_biases[l];
                for (size_t i = 0; i < child_b.size() && i < b_b.size(); ++i) {
                    if (dist(gen) > bias) {
                        child_b.data()[i] = b_b.data()[i];
                    }
                }
            }

            return child;
        }
    };

    // Blend Crossover (BLX-alpha): Child values are chosen uniformly from [min - alpha*d, max + alpha*d]
    struct BlendCrossover {
        float alpha = 0.5f;

        BrainGenome operator()(const BrainGenome& parent_a, const BrainGenome& parent_b) const {
            BrainGenome child = parent_a;
            std::random_device rd;
            std::mt19937 gen(rd());

            for (size_t l = 0; l < child.layer_weights.size() && l < parent_b.layer_weights.size(); ++l) {
                auto& child_w = child.layer_weights[l];
                const auto& b_w = parent_b.layer_weights[l];
                for (size_t i = 0; i < child_w.size() && i < b_w.size(); ++i) {
                    float v1 = child_w.data()[i];
                    float v2 = b_w.data()[i];
                    float min_v = std::min(v1, v2);
                    float max_v = std::max(v1, v2);
                    float d = max_v - min_v;
                    std::uniform_real_distribution<float> dist(min_v - alpha * d, max_v + alpha * d);
                    child_w.data()[i] = dist(gen);
                }
            }

            for (size_t l = 0; l < child.layer_biases.size() && l < parent_b.layer_biases.size(); ++l) {
                auto& child_b = child.layer_biases[l];
                const auto& b_b = parent_b.layer_biases[l];
                for (size_t i = 0; i < child_b.size() && i < b_b.size(); ++i) {
                    float v1 = child_b.data()[i];
                    float v2 = b_b.data()[i];
                    float min_v = std::min(v1, v2);
                    float max_v = std::max(v1, v2);
                    float d = max_v - min_v;
                    std::uniform_real_distribution<float> dist(min_v - alpha * d, max_v + alpha * d);
                    child_b.data()[i] = dist(gen);
                }
            }

            return child;
        }
    };
} // namespace manas