#pragma once
// ============================================================================
// kalpana/edsl/scene_builder.hpp — Declarative Scene Authoring API & Fluent EDSL
// ============================================================================
// Fluent NodeBuilder and TextBuilder supporting chained transforms, fills,
// strokes, effect pipelines, and stream insertion into Scene.
// ============================================================================

#include "../geom/path.hpp"
#include "../geom/shape_builders.hpp"
#include "../paint/paint.hpp"
#include "../effect/effect_chain.hpp"
#include "../scene/node.hpp"
#include "../scene/scene.hpp"
#include <string>
#include <string_view>
#include <utility>

namespace kalpana::edsl {

// Fluent Shape Node Builder
class NodeBuilder {
public:
    explicit NodeBuilder(Path path)
        : path_(std::move(path)) {}

    // Fill Methods
    NodeBuilder& fill(Color c) noexcept {
        paint_ = Paint::fill(c);
        return *this;
    }

    NodeBuilder& fill(spectral::SpectralColor c) noexcept {
        paint_ = Paint::fill(c);
        return *this;
    }

    NodeBuilder& fill(LinearGradient g) noexcept {
        paint_ = Paint::linear_gradient(std::move(g));
        return *this;
    }

    NodeBuilder& fill(RadialGradient g) noexcept {
        paint_ = Paint::radial_gradient(std::move(g));
        return *this;
    }

    NodeBuilder& fill(spectral::SpectralGradient g) noexcept {
        paint_ = Paint::spectral_gradient(std::move(g));
        return *this;
    }

    NodeBuilder& fill(ProceduralFill f) noexcept {
        paint_ = Paint::procedural(std::move(f));
        return *this;
    }

    // Stroke Methods
    NodeBuilder& stroke(Color c, float width = 1.0f) noexcept {
        paint_.stroke().color = c;
        paint_.stroke().width = width;
        return *this;
    }

    NodeBuilder& stroke_cap(LineCap cap) noexcept {
        paint_.stroke().cap = cap;
        return *this;
    }

    NodeBuilder& stroke_join(LineJoin join) noexcept {
        paint_.stroke().join = join;
        return *this;
    }

    // Transform Methods
    NodeBuilder& translate(float tx, float ty) noexcept {
        xf_ = xf_.combine(Transform::translate(tx, ty));
        return *this;
    }

    NodeBuilder& rotate(float radians) noexcept {
        xf_ = xf_.combine(Transform::rotate(radians));
        return *this;
    }

    NodeBuilder& scale(float sx, float sy) noexcept {
        xf_ = xf_.combine(Transform::scale(sx, sy));
        return *this;
    }

    NodeBuilder& scale(float s) noexcept {
        return scale(s, s);
    }

    // Effect Chaining
    NodeBuilder& effect(EffectNode fx) {
        effects_.add(std::move(fx));
        return *this;
    }

    NodeBuilder& effect(EffectChain fx) {
        effects_ = std::move(fx);
        return *this;
    }

    // Node Properties
    NodeBuilder& opacity(float o) noexcept {
        opacity_ = o;
        return *this;
    }

    NodeBuilder& blend(BlendMode mode) noexcept {
        blend_ = mode;
        return *this;
    }

    // Materialize into Scene Node
    [[nodiscard]] Node build() const {
        Node n = Node::shape(path_, paint_);
        n.xf = xf_;
        n.opacity = opacity_;
        n.blend = blend_;
        n.effects = effects_;
        return n;
    }

    [[nodiscard]] operator Node() const {
        return build();
    }

private:
    Path        path_;
    Paint       paint_ = Paint::fill(colors::black());
    Transform   xf_    = Transform::identity();
    float       opacity_ = 1.0f;
    BlendMode   blend_   = BlendMode::SrcOver;
    EffectChain effects_;
};

// Fluent Text Node Builder
class TextBuilder {
public:
    explicit TextBuilder(std::string_view text)
        : text_(text) {}

    TextBuilder& fill(Color c) noexcept {
        color_ = c;
        return *this;
    }

    TextBuilder& fill(spectral::SpectralColor c) noexcept {
        color_ = c.to_color();
        return *this;
    }

    TextBuilder& font_size(float s) noexcept {
        font_size_ = s;
        return *this;
    }

    TextBuilder& position(float x, float y) noexcept {
        x_ = x;
        y_ = y;
        return *this;
    }

    TextBuilder& effect(EffectChain fx) {
        effects_ = std::move(fx);
        return *this;
    }

    TextBuilder& effect(EffectNode fx) {
        effects_.add(std::move(fx));
        return *this;
    }

    [[nodiscard]] Node build() const {
        Node n = Node::text(text_, color_, font_size_, x_, y_);
        n.effects = effects_;
        return n;
    }

    [[nodiscard]] operator Node() const {
        return build();
    }

private:
    std::string text_;
    Color       color_     = colors::black();
    float       font_size_ = 16.0f;
    float       x_         = 0.0f;
    float       y_         = 0.0f;
    EffectChain effects_;
};

// ── EDSL Entry Points ────────────────────────────────────────────────────────

[[nodiscard]] inline NodeBuilder shape(Path p) {
    return NodeBuilder(std::move(p));
}

[[nodiscard]] inline TextBuilder text(std::string_view t) {
    return TextBuilder(t);
}

// operator<< for Scene & NodeBuilder
inline Scene& operator<<(Scene& s, const NodeBuilder& nb) {
    s.add(nb.build());
    return s;
}

inline Scene& operator<<(Scene& s, const TextBuilder& tb) {
    s.add(tb.build());
    return s;
}

} // namespace kalpana::edsl
