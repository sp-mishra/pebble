#pragma once
// ============================================================================
// kalpana/layer/layer_stack.hpp — Ordered Compositing Stack & Layer Pipeline
// ============================================================================
// Supports layer hierarchy, insertion, reordering, flattening, and extensible
// pigment/blend mixing across layer boundaries.
// ============================================================================

#include "layer.hpp"
#include "containers/dynamic/SmallVector.hpp"
#include <cstddef>
#include <vector>
#include <algorithm>
#include <stdexcept>

namespace kalpana {

template <typename LayerContainer = containers::dynamic::SmallVector<Layer, 128>>
class BasicLayerStack {
public:
    using container_type = LayerContainer;

    BasicLayerStack() = default;

    // Layer management
    Layer& add(std::string_view name = "Layer") {
        layers_.push_back(Layer(name));
        return layers_.back();
    }

    Layer& insert(std::size_t index, std::string_view name = "Layer") {
        if (index >= layers_.size()) {
            layers_.push_back(Layer(name));
            return layers_.back();
        }
        layers_.insert(layers_.begin() + index, Layer(name));
        return layers_[index];
    }

    void remove(std::size_t index) {
        if (index < layers_.size()) {
            layers_.erase(layers_.begin() + index);
        }
    }

    void reorder(std::size_t from, std::size_t to) {
        if (from >= layers_.size() || to >= layers_.size() || from == to) return;
        Layer item = std::move(layers_[from]);
        layers_.erase(layers_.begin() + from);
        layers_.insert(layers_.begin() + to, std::move(item));
    }

    // Access
    [[nodiscard]] Layer& operator[](std::size_t index) {
        return layers_[index];
    }

    [[nodiscard]] const Layer& operator[](std::size_t index) const {
        return layers_[index];
    }

    [[nodiscard]] std::size_t size() const noexcept { return layers_.size(); }
    [[nodiscard]] bool empty() const noexcept { return layers_.empty(); }

    void clear() noexcept { layers_.clear(); }

    // Flatten all layers into a single flattened Layer
    [[nodiscard]] Layer flatten(std::string_view flattened_name = "Flattened") const {
        Layer out(flattened_name);
        for (const auto& l : layers_) {
            if (!l.visible()) continue;
            for (const auto& n : l.nodes()) {
                out.add(n);
            }
        }
        return out;
    }

    // Mix two colors across layers using layer combiner policy
    [[nodiscard]] static Color mix_layer(const Layer& bottom, const Layer& top, const Color& dst, const Color& src) {
        (void)bottom;
        return top.combiner().combine(dst, src, top.opacity());
    }

    [[nodiscard]] const LayerContainer& layers() const noexcept { return layers_; }
    [[nodiscard]] LayerContainer& layers() noexcept { return layers_; }

private:
    LayerContainer layers_;
};

using LayerStack = BasicLayerStack<>;

} // namespace kalpana
