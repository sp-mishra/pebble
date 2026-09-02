#pragma once

#include <cstdint>
#include <vector>
#include "../containers/tensor/tensor.hpp"
#include "activation.hpp"
#include "brain.hpp"
#include "topology.hpp"

namespace manas {
    struct BrainGenome {
        TopologyType topology_type = TopologyType::Reactive;

        // Per-layer tensors for multi-layer topologies (FeedForward, etc.)
        std::vector<ts::tensor<float>> layer_weights;
        std::vector<ts::tensor<float>> layer_biases;
        std::vector<ActivationType> layer_activations;

        // Flat weight/bias tensors kept for single-layer (Reactive) convenience
        ts::tensor<float> weights;
        ts::tensor<float> biases;

        uint64_t generation = 0;
        BrainId parent_id{0};
    };
} // namespace manas