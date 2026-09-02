#pragma once
// ============================================================================
// kalpana/color/blackbody.hpp — Physical Blackbody Thermal Radiation Science
// ============================================================================
// Maps thermodynamic temperature T (in Kelvin or Celsius) to spectral blackbody
// radiation colors, Planckian locus RGB, and emissive bloom halos.
//
// Derived from Planck's radiation law & CIE 1931 / Kang approximation.
// ============================================================================

#include "color.hpp"
#include "color_space.hpp"
#include <cmath>
#include <algorithm>

namespace kalpana::blackbody {
    // Convert temperature in Kelvin to physical RGB emission color (Normalized [0, 1])
    // Approximates the Planckian blackbody radiator locus from 500K to 40,000K.
    [[nodiscard]] inline Color temperature_to_rgb(float kelvin) noexcept {
        const float k = std::clamp(kelvin, 500.0f, 40000.0f) / 100.0f;
        float r = 0.0f, g = 0.0f, b = 0.0f;

        // Red component
        if (k <= 66.0f) {
            r = 1.0f;
        }
        else {
            r = std::clamp(329.698727446f * std::pow(k - 60.0f, -0.1332047592f) / 255.0f, 0.0f, 1.0f);
        }

        // Green component
        if (k <= 66.0f) {
            g = std::clamp((99.4708025861f * std::log(k) - 161.1195681661f) / 255.0f, 0.0f, 1.0f);
        }
        else {
            g = std::clamp(288.1221695283f * std::pow(k - 60.0f, -0.0755148492f) / 255.0f, 0.0f, 1.0f);
        }

        // Blue component
        if (k >= 66.0f) {
            b = 1.0f;
        }
        else if (k <= 19.0f) {
            b = 0.0f;
        }
        else {
            b = std::clamp((138.5177312231f * std::log(k - 10.0f) - 305.0447927307f) / 255.0f, 0.0f, 1.0f);
        }

        // Alpha / intensity scaling based on Stefan-Boltzmann radiation intensity (T^4 ramp)
        return Color{r, g, b, 1.0f};
    }

    // Convert temperature in Celsius to blackbody emission color
    [[nodiscard]] inline Color celsius_to_rgb(float celsius) noexcept {
        return temperature_to_rgb(celsius + 273.15f);
    }

    // Blend material base color with incandescent blackbody glow based on temperature
    [[nodiscard]] inline Color apply_thermal_glow(Color base_col, float celsius,
                                                  float glow_threshold = 400.0f,
                                                  float max_incandescence = 3000.0f) noexcept {
        if (celsius <= glow_threshold) {
            return base_col;
        }

        const float t = std::clamp((celsius - glow_threshold) / (max_incandescence - glow_threshold), 0.0f, 1.0f);
        const Color emit = celsius_to_rgb(celsius);

        // Additive / screen blend of thermal emission
        return Color{
            std::clamp(base_col.r * (1.0f - t * 0.7f) + emit.r * t * 1.2f, 0.0f, 1.0f),
            std::clamp(base_col.g * (1.0f - t * 0.7f) + emit.g * t * 1.2f, 0.0f, 1.0f),
            std::clamp(base_col.b * (1.0f - t * 0.7f) + emit.b * t * 1.2f, 0.0f, 1.0f),
            base_col.a
        };
    }
} // namespace kalpana::blackbody
