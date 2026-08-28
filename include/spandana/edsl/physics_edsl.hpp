#pragma once
// ============================================================================
// spandana/edsl/physics_edsl.hpp — Prakriti Dynamics & Force EDSL Directives
// ============================================================================

#include "../timeline.hpp"
#include "containers/numeric/math_vector.hpp"
#include <functional>

namespace pebble::spandana::edsl {

// Radial Impulse Action
class RadialImpulseAction : public IAnimationAction {
public:
    RadialImpulseAction(pebble::math::vec2 center, float radius, float magnitude, ResourceKey key)
        : center_(center), radius_(radius), magnitude_(magnitude), key_(key) {}

    void on_start() override {
        // Pushes radial velocity / force into nearby dynamic bodies
    }

    void update(float, float) override {}
    [[nodiscard]] float duration() const noexcept override { return 0.0f; } // Instantaneous
    [[nodiscard]] ResourceKey resource_key() const noexcept override { return key_; }

    [[nodiscard]] pebble::math::vec2 center() const noexcept { return center_; }
    [[nodiscard]] float radius() const noexcept { return radius_; }
    [[nodiscard]] float magnitude() const noexcept { return magnitude_; }

private:
    pebble::math::vec2 center_;
    float              radius_;
    float              magnitude_;
    ResourceKey        key_;
};

class RadialImpulseBuilder {
public:
    explicit RadialImpulseBuilder(ResourceKey key = kWorldResource)
        : key_(key) {}

    RadialImpulseBuilder& at(pebble::math::vec2 center) {
        center_ = center;
        return *this;
    }

    RadialImpulseBuilder& radius(float r) {
        radius_ = r;
        return *this;
    }

    RadialImpulseAction magnitude(float mag) {
        return RadialImpulseAction(center_, radius_, mag, key_);
    }

private:
    pebble::math::vec2 center_{};
    float              radius_ = 50.0f;
    ResourceKey        key_;
};

inline RadialImpulseBuilder radial_impulse(ResourceKey key = kWorldResource) {
    return RadialImpulseBuilder(key);
}

} // namespace pebble::spandana::edsl
