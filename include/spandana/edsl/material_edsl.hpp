#pragma once
// ============================================================================
// spandana/edsl/material_edsl.hpp — Thermodynamics, Material State & Reaction EDSL
// ============================================================================
// Enables users to declaratively define materials, apply heat, trigger phase
// changes, freeze, melt, burn, and fuse entities in the world timeline.
// ============================================================================

#include "../timeline.hpp"
#include "gati/material.hpp"
#include "gati/transform.hpp"
#include "ecs/ecs.hpp"
#include "containers/numeric/math_vector.hpp"
#include <cmath>

namespace pebble::spandana::edsl {

// 1. Assign Material Action
class SetMaterialAction {
public:
    SetMaterialAction(pebble::ecs::World& world, pebble::ecs::Entity entity,
                      gati::MaterialComponent mat, ResourceKey key)
        : world_(world), entity_(entity), mat_(mat), key_(key) {
        // Material assignment is zero-duration setup (duration()==0): the
        // component must exist as soon as the action is scheduled so that
        // queries and later same-frame actions see the initial state, not only
        // after the first timeline.update(). Applying here is idempotent with
        // the on_start() call below (both overwrite with the same mat_).
        apply();
    }

    void on_start() {
        apply();
    }

    void apply() {
        if (!world_.alive(entity_)) return;
        if (world_.has<gati::MaterialComponent>(entity_)) {
            *world_.get<gati::MaterialComponent>(entity_) = mat_;
        } else {
            world_.add<gati::MaterialComponent>(entity_, mat_);
        }
    }

    void update(float, float) noexcept {}
    [[nodiscard]] float duration() const noexcept { return 0.0f; }
    [[nodiscard]] ResourceKey resource_key() const noexcept { return key_; }

private:
    pebble::ecs::World&     world_;
    pebble::ecs::Entity     entity_;
    gati::MaterialComponent mat_;
    ResourceKey             key_;
};

class SetMaterialBuilder {
public:
    SetMaterialBuilder(pebble::ecs::World& world, pebble::ecs::Entity entity,
                       gati::MaterialComponent mat, ResourceKey key = kWorldResource)
        : world_(world), entity_(entity), mat_(mat), key_(key) {}

    SetMaterialBuilder& temperature(float t) {
        mat_.temperature = t;
        mat_.phase_fractions = prakriti::phase_from_temperature(t, mat_.params);
        return *this;
    }

    SetMaterialBuilder& flammability(bool f) {
        mat_.flammable = f;
        return *this;
    }

    SetMaterialBuilder& can_fuse(bool f) {
        mat_.can_fuse = f;
        return *this;
    }

    [[nodiscard]] SetMaterialAction build() const {
        return SetMaterialAction(world_, entity_, mat_, key_);
    }

    operator SetMaterialAction() const {
        return build();
    }

private:
    pebble::ecs::World&     world_;
    pebble::ecs::Entity     entity_;
    gati::MaterialComponent mat_;
    ResourceKey             key_;
};

inline SetMaterialBuilder set_material(pebble::ecs::World& world, pebble::ecs::Entity entity,
                                       gati::MaterialComponent mat, ResourceKey key = kWorldResource) {
    return SetMaterialBuilder(world, entity, mat, key);
}

// 2. Continuous Heat / Thermal Source Action
class ApplyHeatAction {
public:
    ApplyHeatAction(pebble::ecs::World& world, pebble::math::vec2 center,
                    float radius, float target_temp, float duration, ResourceKey key)
        : world_(world), center_(center), radius_(radius),
          target_temp_(target_temp), duration_(duration), key_(key) {}

    void update(float, float dt) noexcept {
        const float r2 = radius_ * radius_;
        world_.view<gati::MaterialComponent, gati::Transform>([&](pebble::ecs::Entity,
                                                                  gati::MaterialComponent& mat,
                                                                  gati::Transform& tr) {
            const float dx = tr.position[0] - center_[0];
            const float dy = tr.position[1] - center_[1];
            const float d2 = dx * dx + dy * dy;
            if (d2 <= r2) {
                const float falloff = 1.0f - std::sqrt(d2) / radius_;
                const float delta_t = (target_temp_ - mat.temperature) * (mat.params.conductivity * 0.2f) * falloff * dt;
                mat.update_thermodynamics(delta_t, dt);
            }
        });
    }

    [[nodiscard]] float duration() const noexcept { return duration_; }
    [[nodiscard]] ResourceKey resource_key() const noexcept { return key_; }

private:
    pebble::ecs::World& world_;
    pebble::math::vec2  center_;
    float               radius_;
    float               target_temp_;
    float               duration_;
    ResourceKey         key_;
};

class ApplyHeatBuilder {
public:
    explicit ApplyHeatBuilder(pebble::ecs::World& world, ResourceKey key = kWorldResource)
        : world_(world), key_(key) {}

    ApplyHeatBuilder& at(pebble::math::vec2 center) {
        center_ = center;
        return *this;
    }

    ApplyHeatBuilder& radius(float r) {
        radius_ = r;
        return *this;
    }

    ApplyHeatBuilder& temperature(float t) {
        temp_ = t;
        return *this;
    }

    [[nodiscard]] ApplyHeatAction duration(float d = 1.0f) const {
        return ApplyHeatAction(world_, center_, radius_, temp_, d, key_);
    }

private:
    pebble::ecs::World& world_;
    pebble::math::vec2  center_{0.0f, 0.0f};
    float               radius_ = 50.0f;
    float               temp_ = 300.0f;
    ResourceKey         key_;
};

inline ApplyHeatBuilder apply_heat(pebble::ecs::World& world, ResourceKey key = kWorldResource) {
    return ApplyHeatBuilder(world, key);
}

// 3. Freeze Thermal Source Directive
inline ApplyHeatBuilder freeze_region(pebble::ecs::World& world, ResourceKey key = kWorldResource) {
    return apply_heat(world, key).temperature(-50.0f);
}

} // namespace pebble::spandana::edsl
