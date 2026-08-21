#pragma once
// ============================================================================
// kalpana/scene/node.hpp — Scene Graph Node Types (Shape, Image, Group)
// ============================================================================

#include "../geom/path.hpp"
#include "../paint/paint.hpp"
#include "../effect/effect.hpp"
#include "../geom/transform.hpp"
#include <vector>
#include <variant>
#include <memory>

namespace kalpana {

struct ShapeNode {
    Path  path;
    Paint paint;
};

struct ImageNode {
    const std::uint32_t* pixels = nullptr;
    std::uint32_t        w = 0, h = 0;
    float                dx = 0.0f, dy = 0.0f;
    float                dw = 0.0f, dh = 0.0f;
};

struct Node;

struct GroupNode {
    std::vector<Node> children;
};

struct Node {
    Transform           xf = Transform::identity();
    float               opacity = 1.0f;
    BlendMode           blend = BlendMode::SrcOver;
    std::vector<Effect> effects;

    std::variant<ShapeNode, ImageNode, GroupNode> content;

    static Node shape(Path path, Paint paint) {
        Node n;
        n.content = ShapeNode{std::move(path), std::move(paint)};
        return n;
    }

    static Node group(std::vector<Node> children = {}) {
        Node n;
        n.content = GroupNode{std::move(children)};
        return n;
    }
};

} // namespace kalpana
