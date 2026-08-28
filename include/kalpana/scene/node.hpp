#pragma once
// ============================================================================
// kalpana/scene/node.hpp — Scene Graph Node Types (Shape, Image, Text, Group)
// ============================================================================
// Zero-virtual, SmallVector-configurable node hierarchy with EffectChain storage.
// ============================================================================

#include "../geom/path.hpp"
#include "../paint/paint.hpp"
#include "../effect/effect_chain.hpp"
#include "../geom/transform.hpp"
#include "containers/dynamic/SmallVector.hpp"
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace kalpana {

struct ShapeNode {
    Path  path;
    Paint paint;
    friend bool operator==(const ShapeNode&, const ShapeNode&) = default;
};

struct ImageNode {
    const std::uint32_t* pixels = nullptr;
    std::uint32_t        w = 0, h = 0;
    float                dx = 0.0f, dy = 0.0f;
    float                dw = 0.0f, dh = 0.0f;
    friend bool operator==(const ImageNode&, const ImageNode&) = default;
};

struct TextNode {
    std::string text;
    Color       color = colors::black();
    float       font_size = 16.0f;
    float       x = 0.0f, y = 0.0f;
    friend bool operator==(const TextNode&, const TextNode&) = default;
};

struct Node;

struct GroupNode {
    using container_type = std::vector<Node>;
    std::vector<Node> children;
    friend bool operator==(const GroupNode&, const GroupNode&) = default;
};

struct Node {
    Transform   xf = Transform::identity();
    float       opacity = 1.0f;
    BlendMode   blend = BlendMode::SrcOver;
    EffectChain effects;

    std::variant<ShapeNode, ImageNode, GroupNode, TextNode> content;

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

    static Node text(std::string_view text, Color color = colors::black(), float font_size = 16.0f, float x = 0.0f, float y = 0.0f) {
        Node n;
        n.content = TextNode{.text = std::string(text), .color = color, .font_size = font_size, .x = x, .y = y};
        return n;
    }

    friend bool operator==(const Node&, const Node&) = default;
};

} // namespace kalpana
