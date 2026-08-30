#pragma once
// dhvani/prakriti_bridge.hpp — Maps Prakriti continuum material state to acoustic MaterialParams.
// Converts normalized density/pressure/temperature to MaterialParams for use with SoundBuilder.

#include "physical/material.hpp"
#include <algorithm>

namespace pebble::dhvani {

// Map Prakriti per-particle state to acoustic material descriptor.
// density_norm:     particle mass / reference max mass [0..1]
// temperature_norm: particle temperature / reference max temperature [0..1]
// pressure_norm:    local pressure field value / reference max pressure [0..1] (reserved)
[[nodiscard]] inline physical::MaterialParams from_prakriti_material(
    float density_norm,
    float temperature_norm,
    float /*pressure_norm*/ = 0.f) noexcept
{
    // Stiffness drops as temperature rises (solid→liquid→gas transition)
    const float stiffness = std::clamp(density_norm - temperature_norm * 0.5f, 0.f, 1.f);
    // Damping increases for low-density, high-temperature (gaseous) states
    const float damping   = std::clamp(
        temperature_norm * 0.6f + (1.f - density_norm) * 0.4f, 0.f, 1.f);
    // Brittleness meaningful only for cold, dense (solid) states
    const float brittleness = std::clamp(stiffness * (1.f - temperature_norm), 0.f, 1.f);

    return physical::MaterialParams{
        .density     = density_norm,
        .stiffness   = stiffness,
        .damping     = damping,
        .brittleness = brittleness,
        .roughness   = 0.5f,
        .thickness   = density_norm,
    };
}

} // namespace pebble::dhvani
