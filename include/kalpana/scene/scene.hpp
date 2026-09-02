#pragma once
// ============================================================================
// kalpana/scene/scene.hpp — Retained 2D Scene Graph Container & EDSL Target
// ============================================================================
// Supports stream insertion operator<< and optional LayerStack integration.
// ============================================================================

#include "node.hpp"
#include "../color/color.hpp"
#include "../layer/layer_stack.hpp"
#include <optional>

namespace kalpana {
    class Scene {
    public:
        Scene() {
            root_.content = GroupNode{};
        }

        void clear_color(Color c) noexcept {
            clear_color_ = c;
        }

        [[nodiscard]] Color clear_color() const noexcept {
            return clear_color_;
        }

        void add(Node node) {
            if (auto* g = std::get_if<GroupNode>(&root_.content)) {
                g->children.push_back(std::move(node));
            }
        }

        // Stream operator<< for EDSL scene construction
        Scene& operator<<(Node node) {
            add(std::move(node));
            return *this;
        }

        [[nodiscard]] const Node& root() const noexcept {
            return root_;
        }

        [[nodiscard]] Node& root() noexcept {
            return root_;
        }

        void clear() noexcept {
            if (auto* g = std::get_if<GroupNode>(&root_.content)) {
                g->children.clear();
            }
            if (layers_) {
                layers_->clear();
            }
        }

        // Layer-based workflow integration
        [[nodiscard]] LayerStack& layers() {
            if (!layers_) {
                layers_.emplace();
            }
            return *layers_;
        }

        [[nodiscard]] const LayerStack& layers() const {
            return *layers_;
        }

        [[nodiscard]] bool has_layers() const noexcept {
            return layers_.has_value() && !layers_->empty();
        }

    private:
        Color clear_color_ = colors::transparent();
        Node root_;
        std::optional<LayerStack> layers_;
    };
} // namespace kalpana
