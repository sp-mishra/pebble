#pragma once
// dhvani/graph/node.hpp — AudioNode concept and graph connection descriptor.

#include "../synth/buffer.hpp"
#include <cstdint>
#include <span>

namespace pebble::dhvani::graph {
    using NodeId = uint32_t;
    static constexpr NodeId kInvalidNode = ~NodeId{0};

    // An AudioNode processes a block of PCM samples in-place or mixing into output
    template <typename T>
    concept AudioNode = requires(T& n,
                                 std::span<synth::SampleFrame> out,
                                 std::span<const synth::SampleFrame> in,
                                 uint32_t sr) {
        { n.process(out, in, sr) } -> std::same_as<void>;
        { n.reset() } -> std::same_as<void>;
    };

    struct NodeConnection {
        NodeId from = kInvalidNode;
        NodeId to = kInvalidNode;
        uint8_t from_port = 0;
        uint8_t to_port = 0;
    };
} // namespace pebble::dhvani::graph
