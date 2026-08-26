#pragma once

#include <stdexcept>
#include "../containers/tensor/tensor.hpp"
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
        }
        return activation;
    }

private:
    BrainGenome genome_;
};

} // namespace manas