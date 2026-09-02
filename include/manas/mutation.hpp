#pragma once
#include "genome.hpp"
#include <containers/dynamic/SmallVector.hpp>
#include <cmath>
#include <functional>
#include <random>

namespace manas {
    using MutationOperator = std::function<void(BrainGenome &)>;
    using MutationOperators = containers::dynamic::SmallVector<MutationOperator, sizeof(MutationOperator) * 4>;

    // Gaussian Jitter: Perturb weights/biases by Gaussian noise N(0, sigma) with probability rate
    struct GaussianJitterMutation {
        float rate = 0.1f;
        float sigma = 0.05f;

        void operator()(BrainGenome& genome) const {
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_real_distribution<float> prob_dist(0.0f, 1.0f);
            std::normal_distribution<float> norm_dist(0.0f, sigma);

            for (auto& w_tensor : genome.layer_weights) {
                for (size_t i = 0; i < w_tensor.size(); ++i) {
                    if (prob_dist(gen) < rate) {
                        w_tensor.data()[i] += norm_dist(gen);
                    }
                }
            }
            for (auto& b_tensor : genome.layer_biases) {
                for (size_t i = 0; i < b_tensor.size(); ++i) {
                    if (prob_dist(gen) < rate) {
                        b_tensor.data()[i] += norm_dist(gen);
                    }
                }
            }
        }
    };

    // Uniform Random Reset: Randomly replace weights with U(min_val, max_val)
    struct UniformResetMutation {
        float rate = 0.05f;
        float min_val = -1.0f;
        float max_val = 1.0f;

        void operator()(BrainGenome& genome) const {
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_real_distribution<float> prob_dist(0.0f, 1.0f);
            std::uniform_real_distribution<float> val_dist(min_val, max_val);

            for (auto& w_tensor : genome.layer_weights) {
                for (size_t i = 0; i < w_tensor.size(); ++i) {
                    if (prob_dist(gen) < rate) {
                        w_tensor.data()[i] = val_dist(gen);
                    }
                }
            }
            for (auto& b_tensor : genome.layer_biases) {
                for (size_t i = 0; i < b_tensor.size(); ++i) {
                    if (prob_dist(gen) < rate) {
                        b_tensor.data()[i] = val_dist(gen);
                    }
                }
            }
        }
    };
} // namespace manas