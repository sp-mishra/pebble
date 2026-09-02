#pragma once
// dhvani/prakriti_bridge.hpp — Maps Prakriti continuum material state to acoustic MaterialParams.
// Converts normalized density/pressure/temperature to MaterialParams for use with SoundBuilder.
//
// The mapping coefficients are no longer hardcoded: they live in a
// `PrakritiAcousticMap` config struct with documented defaults, so a simulation
// can retune the density→stiffness / temperature→damping relationships (and now
// pressure→stiffness) without editing the bridge.

#include "physical/material.hpp"
#include <algorithm>

namespace pebble::dhvani {
    // Tunable coefficients mapping normalized continuum state → acoustic material.
    // Defaults reproduce the original bridge behavior exactly.
    struct PrakritiAcousticMap {
        float temp_softens_stiffness = 0.5f; // stiffness -= temp * this
        float pressure_stiffens = 0.3f; // stiffness += pressure * this
        float temp_damps = 0.6f; // damping += temp * this
        float low_density_damps = 0.4f; // damping += (1 - density) * this
        float base_roughness = 0.5f; // roughness output (constant surface finish)
    };

    // Config-driven mapping. `pressure_norm` now participates (stiffens the medium,
    // e.g. a compressed liquid rings more solidly) instead of being ignored.
    [[nodiscard]] inline physical::MaterialParams from_prakriti_material(
        float density_norm,
        float temperature_norm,
        float pressure_norm,
        const PrakritiAcousticMap& map) noexcept {
        // Stiffness drops as temperature rises (solid→liquid→gas) and rises with
        // confining pressure.
        const float stiffness = std::clamp(
            density_norm - temperature_norm * map.temp_softens_stiffness
            + pressure_norm * map.pressure_stiffens,
            0.f, 1.f);
        // Damping increases for low-density, high-temperature (gaseous) states.
        const float damping = std::clamp(
            temperature_norm * map.temp_damps + (1.f - density_norm) * map.low_density_damps,
            0.f, 1.f);
        // Brittleness meaningful only for cold, dense (solid) states.
        const float brittleness = std::clamp(stiffness * (1.f - temperature_norm), 0.f, 1.f);

        return physical::MaterialParams{
            .density = density_norm,
            .stiffness = stiffness,
            .damping = damping,
            .brittleness = brittleness,
            .roughness = map.base_roughness,
            .thickness = density_norm,
        };
    }

    // Back-compat overload: default coefficients. `pressure_norm` defaults to 0,
    // which reproduces the original two-argument behavior bit-for-bit.
    [[nodiscard]] inline physical::MaterialParams from_prakriti_material(
        float density_norm,
        float temperature_norm,
        float pressure_norm = 0.f) noexcept {
        return from_prakriti_material(density_norm, temperature_norm, pressure_norm,
                                      PrakritiAcousticMap{});
    }
} // namespace pebble::dhvani
