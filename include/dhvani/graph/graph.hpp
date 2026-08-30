#pragma once
// dhvani/graph/graph.hpp — SoundGraph: type-erased DAG of AudioNodes, evaluated in insertion order.
// Type erasure uses std::function (graph construction path only — not hot audio path).

#include "node.hpp"
#include <functional>
#include <vector>
#include <span>

namespace pebble::dhvani::graph {

struct NodeEntry {
    NodeId                                                       id = kInvalidNode;
    std::function<void(std::span<synth::SampleFrame>,
                       std::span<const synth::SampleFrame>,
                       uint32_t)>                                process_fn;
    std::function<void()>                                        reset_fn;
};

// DAG of AudioNodes. User is responsible for insertion order matching data-flow order.
// For complex graphs, use LiteGraph topological sort and call add_node() in sorted order.
class SoundGraph {
public:
    template <AudioNode Node>
    NodeId add_node(Node node) {
        const NodeId id = next_id_++;
        nodes_.push_back(NodeEntry{
            .id         = id,
            .process_fn = [n = std::move(node)](
                std::span<synth::SampleFrame> out,
                std::span<const synth::SampleFrame> in,
                uint32_t sr) mutable { n.process(out, in, sr); },
            .reset_fn   = [n_ptr = static_cast<void*>(nullptr)]() mutable {}
        });
        return id;
    }

    void connect(NodeId from, NodeId to, uint8_t from_port = 0, uint8_t to_port = 0) {
        edges_.push_back({from, to, from_port, to_port});
    }

    // Evaluate all nodes sequentially into output. Scratch buffer avoids aliasing.
    void process(std::span<synth::SampleFrame> output, uint32_t sr) {
        scratch_.assign(output.size(), synth::SampleFrame{});
        for (auto& node : nodes_)
            node.process_fn(output, scratch_, sr);
    }

    void reset() {
        for (auto& n : nodes_) n.reset_fn();
    }

    [[nodiscard]] std::size_t node_count() const noexcept { return nodes_.size(); }

private:
    std::vector<NodeEntry>      nodes_;
    std::vector<NodeConnection> edges_;
    std::vector<synth::SampleFrame> scratch_;
    NodeId                      next_id_ = 0;
};

} // namespace pebble::dhvani::graph
