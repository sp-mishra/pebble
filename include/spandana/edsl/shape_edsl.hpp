#pragma once
// ============================================================================
// spandana/edsl/shape_edsl.hpp — Akruti Geometry & CSG Shape EDSL Directives
// ============================================================================

#include "../timeline.hpp"
#include "akruti/akruti.hpp"
#include "akruti/csg.hpp"

namespace pebble::spandana::edsl {

// Shape factory wrappers
inline akruti::Circle circle(float r) {
    return akruti::Circle{pebble::math::vec2(0.0f, 0.0f), r};
}

inline akruti::Box box(pebble::math::vec2 half_extents) {
    return akruti::Box{pebble::math::vec2(0.0f, 0.0f), half_extents};
}

inline akruti::Capsule capsule(pebble::math::vec2 a, pebble::math::vec2 b, float r) {
    return akruti::Capsule{a, b, r};
}

// Expanding / Fading Shape Effect Action
class ShapeEffectAction : public IAnimationAction {
public:
    ShapeEffectAction(akruti::CsgNode shape, pebble::math::vec2 pos, float max_scale, float duration, ResourceKey key)
        : shape_(std::move(shape)), pos_(pos), max_scale_(max_scale), duration_(duration), key_(key) {}

    void update(float progress, float) override {
        current_scale_ = 1.0f + (max_scale_ - 1.0f) * progress;
        alpha_ = 1.0f - progress;
    }

    [[nodiscard]] float duration() const noexcept override { return duration_; }
    [[nodiscard]] ResourceKey resource_key() const noexcept override { return key_; }

    [[nodiscard]] float current_scale() const noexcept { return current_scale_; }
    [[nodiscard]] float alpha() const noexcept { return alpha_; }

private:
    akruti::CsgNode     shape_;
    pebble::math::vec2  pos_;
    float               max_scale_;
    float               duration_;
    ResourceKey         key_;
    float               current_scale_ = 1.0f;
    float               alpha_ = 1.0f;
};

// Builder for shape effects
class ShapeEffectBuilder {
public:
    explicit ShapeEffectBuilder(akruti::CsgNode shape, ResourceKey key = kWorldResource)
        : shape_(std::move(shape)), key_(key) {}

    ShapeEffectBuilder& at(pebble::math::vec2 pos) {
        pos_ = pos;
        return *this;
    }

    ShapeEffectBuilder& grow(float max_scale) {
        max_scale_ = max_scale;
        return *this;
    }

    ShapeEffectAction fade_out(float duration = 0.4f) {
        return ShapeEffectAction(std::move(shape_), pos_, max_scale_, duration, key_);
    }

private:
    akruti::CsgNode    shape_;
    pebble::math::vec2 pos_{};
    float              max_scale_ = 1.0f;
    ResourceKey        key_;
};

inline ShapeEffectBuilder shape_fx(const akruti::Circle& c, ResourceKey key = kWorldResource) {
    akruti::CsgNode node;
    node.is_leaf = true;
    node.leaf = c;
    return ShapeEffectBuilder(std::move(node), key);
}

} // namespace pebble::spandana::edsl
