#pragma once
// dhvani/gati_bridge.hpp — Maps Gati physics collision events to SoundBus cues.
// Collision impulse + material pair → impact or fracture sound selection.
//
// Cue names and selection thresholds are no longer magic literals: cue names
// come from the `DhvaniCue` registry (a single source of truth shared with the
// auto-sonification layer) and thresholds live in a tunable
// `CollisionSonifyConfig`. Default construction reproduces the original bridge.

#include "dhvani.hpp"
#include "physical/material.hpp"
#include "synth/buffer.hpp"
#include <algorithm>
#include <string_view>

namespace pebble::dhvani {
    // Canonical procedural-cue identifiers. These names are the contract between the
    // physics bridges (producers) and any procedural-voice sink (consumer). Kept as
    // a registry so there is exactly one spelling of each magic string.
    struct DhvaniCue {
        static constexpr std::string_view impact = "__dhvani_impact__";
        static constexpr std::string_view fracture = "__dhvani_fracture__";
        static constexpr std::string_view friction = "__dhvani_friction__";
    };

    // Tunable collision → cue selection. Defaults match the original thresholds.
    struct CollisionSonifyConfig {
        float min_impulse = 0.01f; // below this, no impact cue
        float min_friction_vel = 0.01f; // below this, no friction cue
        float fracture_brittleness = 0.7f; // combined brittleness above this ...
        float fracture_impulse = 0.6f; // ... and impulse above this → fracture
    };

    struct CollisionSoundEvent {
        physical::MaterialParams material_a{};
        physical::MaterialParams material_b{};
        float impulse_magnitude = 1.f; // normalized [0..1]
        float relative_velocity = 0.f; // normalized [0..1]
    };

    // Stateless bridge — attach to any collision callback in Gati systems
    struct GatiSoundBridge {
        SoundBus& bus;
        uint32_t sample_rate = synth::kDefaultSampleRate;
        float volume_scale = 1.f;
        CollisionSonifyConfig config{};

        void on_collision(const CollisionSoundEvent& evt) const {
            const float ni = std::clamp(evt.impulse_magnitude, 0.f, 1.f);
            if (ni < config.min_impulse) return;

            // Average acoustic properties of the two colliding materials
            const physical::MaterialParams combined{
                .density = (evt.material_a.density + evt.material_b.density) * 0.5f,
                .stiffness = (evt.material_a.stiffness + evt.material_b.stiffness) * 0.5f,
                .damping = (evt.material_a.damping + evt.material_b.damping) * 0.5f,
                .brittleness = (evt.material_a.brittleness + evt.material_b.brittleness) * 0.5f,
                .roughness = (evt.material_a.roughness + evt.material_b.roughness) * 0.5f,
                .thickness = (evt.material_a.thickness + evt.material_b.thickness) * 0.5f,
            };

            // High-brittleness + strong impulse → fracture; else impact
            if (combined.brittleness > config.fracture_brittleness && ni > config.fracture_impulse)
                bus.play(DhvaniCue::fracture, ni * volume_scale);
            else
                bus.play(DhvaniCue::impact, ni * volume_scale);
        }

        void on_friction(const CollisionSoundEvent& evt) const {
            const float vel = std::clamp(evt.relative_velocity, 0.f, 1.f);
            if (vel > config.min_friction_vel)
                bus.play(DhvaniCue::friction, vel * volume_scale);
        }
    };
} // namespace pebble::dhvani
