#pragma once

#include <stdexcept>
#include "../containers/tensor/tensor.hpp"
#include "activation.hpp"
#include "genome.hpp"
#include "topology.hpp"

namespace manas {
    class Network {
    public:
        explicit Network(const BrainGenome& genome)
            : genome_{genome} {}

        ts::tensor<float> operator()(ts::tensor<float> input) const {
            if (genome_.layer_weights.size() != genome_.layer_biases.size()) {
                throw std::invalid_argument(
                    "Mismatched layer_weights and layer_biases counts in BrainGenome");
            }

            ts::tensor<float> activation = input;
            for (size_t i = 0; i < genome_.layer_weights.size(); ++i) {
                // linear: activation = W * activation + b
                activation = ts::dot(genome_.layer_weights[i], activation)
                    + genome_.layer_biases[i];

                // Apply activation function if specified for this layer
                if (i < genome_.layer_activations.size()) {
                    const auto act_type = genome_.layer_activations[i];
                    if (act_type != ActivationType::Identity) {
                        for (size_t k = 0; k < activation.size(); ++k) {
                            activation({k}) = apply_activation(act_type, activation({k}));
                        }
                    }
                }
            }
            return activation;
        }

    private:
        BrainGenome genome_;
    };
} // namespace manas