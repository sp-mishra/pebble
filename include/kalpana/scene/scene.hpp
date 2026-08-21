#pragma once
// ============================================================================
// kalpana/scene/scene.hpp — Retained 2D Scene Graph Container
// ============================================================================

#include "node.hpp"
#include "../color/color.hpp"

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
    }

private:
    Color clear_color_ = colors::transparent();
    Node  root_;
};

} // namespace kalpana
