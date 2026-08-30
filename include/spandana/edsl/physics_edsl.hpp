#pragma once
// ============================================================================
// spandana/edsl/physics_edsl.hpp — Prakriti Dynamics & Force EDSL Directives
// ============================================================================

#include "../timeline.hpp"
#include "containers/numeric/math_vector.hpp"
#include "prakriti/prakriti.hpp"

namespace pebble::spandana::edsl {

// Radial Impulse Action — pure intent; delegates the physics to Prakriti (matter/physics owner).
// Spandana selects where/how-much; Prakriti applies the impulse to particles in range.
// The target world is optional: bind one via `into(world)` and on_start() delegates to
// prakriti::World::apply_radial_impulse; left unbound it is an inert descriptor (no simulation
// in Spandana). This keeps the fluent verb usable without a world in scope.
class RadialImpulseAction {
public:
    RadialImpulseAction(pebble::math::vec2 center, float radius, float magnitude,
                        prakriti::World<>* world, ResourceKey key)
        : center_(center), radius_(radius), magnitude_(magnitude), world_(world), key_(key) {}

    void on_start() {
        // Delegate to Prakriti: outward impulse to every dynamic particle within radius.
        if (world_) world_->apply_radial_impulse(center_, radius_, magnitude_);
    }

    void update(float, float) noexcept {}
    [[nodiscard]] float duration() const noexcept { return 0.0f; } // Instantaneous
    [[nodiscard]] ResourceKey resource_key() const noexcept { return key_; }

    [[nodiscard]] pebble::math::vec2 center() const noexcept { return center_; }
    [[nodiscard]] float radius() const noexcept { return radius_; }
    [[nodiscard]] float magnitude() const noexcept { return magnitude_; }

private:
    pebble::math::vec2 center_;
    float              radius_;
    float              magnitude_;
    prakriti::World<>* world_;
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

    // Bind the Prakriti world the impulse delegates to (matter/physics owner).
    RadialImpulseBuilder& into(prakriti::World<>& world) {
        world_ = &world;
        return *this;
    }

    RadialImpulseAction magnitude(float mag) {
        return RadialImpulseAction(center_, radius_, mag, world_, key_);
    }

private:
    pebble::math::vec2 center_{};
    float              radius_ = 50.0f;
    prakriti::World<>* world_ = nullptr;
    ResourceKey        key_;
};

inline RadialImpulseBuilder radial_impulse(ResourceKey key = kWorldResource) {
    return RadialImpulseBuilder(key);
}

} // namespace pebble::spandana::edsl
