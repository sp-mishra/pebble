#pragma once
// dhvani/gati_bridge.hpp — Maps Gati physics collision events to SoundBus cues.
// Collision impulse + material pair → impact or fracture sound selection.

#include "dhvani.hpp"
#include "physical/material.hpp"
#include "synth/buffer.hpp"
#include <algorithm>

namespace pebble::dhvani {

struct CollisionSoundEvent {
    physical::MaterialParams material_a{};
    physical::MaterialParams material_b{};
    float                    impulse_magnitude = 1.f;  // normalized [0..1]
    float                    relative_velocity = 0.f;  // normalized [0..1]
};

// Stateless bridge — attach to any collision callback in Gati systems
struct GatiSoundBridge {
    SoundBus& bus;
    uint32_t  sample_rate = synth::kDefaultSampleRate;
    float     volume_scale = 1.f;

    void on_collision(const CollisionSoundEvent& evt) const {
        const float ni = std::clamp(evt.impulse_magnitude, 0.f, 1.f);
        if (ni < 0.01f) return;

        // Average acoustic properties of the two colliding materials
        const physical::MaterialParams combined{
            .density     = (evt.material_a.density     + evt.material_b.density)     * 0.5f,
            .stiffness   = (evt.material_a.stiffness   + evt.material_b.stiffness)   * 0.5f,
            .damping     = (evt.material_a.damping     + evt.material_b.damping)     * 0.5f,
            .brittleness = (evt.material_a.brittleness + evt.material_b.brittleness) * 0.5f,
            .roughness   = (evt.material_a.roughness   + evt.material_b.roughness)   * 0.5f,
            .thickness   = (evt.material_a.thickness   + evt.material_b.thickness)   * 0.5f,
        };

        // High-brittleness + strong impulse → fracture; else impact
        if (combined.brittleness > 0.7f && ni > 0.6f)
            bus.play("__dhvani_fracture__", ni * volume_scale);
        else
            bus.play("__dhvani_impact__",   ni * volume_scale);
    }

    void on_friction(const CollisionSoundEvent& evt) const {
        const float vel = std::clamp(evt.relative_velocity, 0.f, 1.f);
        if (vel > 0.01f)
            bus.play("__dhvani_friction__", vel * volume_scale);
    }
};

} // namespace pebble::dhvani
