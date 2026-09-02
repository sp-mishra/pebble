#pragma once
// ============================================================================
// gati/material.hpp — Material Properties, Thermodynamics & Continuous Phase State
// ============================================================================
// Bridges Prakriti material thermodynamics, phase fractions, constitutive limits,
// and state-change thresholds directly to Pebble ECS components.
// ============================================================================

#include "prakriti/material/phase.hpp"
#include "prakriti/state/material_registry.hpp"
#include "containers/numeric/math_vector.hpp"
#include <string_view>
#include <cmath>
#include <algorithm>

namespace gati {
    // ECS Component: Physical Material Properties & Thermodynamics State
    struct MaterialComponent {
        prakriti::MaterialParams params;
        float temperature = 20.0f; // Celsius
        prakriti::PhaseFractions phase_fractions{};
        float damage = 0.0f; // [0, 1] 1.0 = fractured
        bool can_fuse = true;
        bool flammable = false;
        float burn_time = 0.0f;

        // Presets
        [[nodiscard]] static MaterialComponent Ice() noexcept {
            MaterialComponent m;
            m.params.rest_density = 917.0f;
            m.params.heat_capacity = 2.1f;
            m.params.conductivity = 2.2f;
            m.params.melt_temp = 0.0f;
            m.params.boil_temp = 100.0f;
            m.params.yield_strain = 0.001f;
            m.params.ultimate_strain = 0.005f; // Very brittle
            m.params.youngs_modulus = 9e9f;
            m.temperature = -10.0f;
            m.phase_fractions = prakriti::phase_from_temperature(m.temperature, m.params);
            return m;
        }

        [[nodiscard]] static MaterialComponent Water() noexcept {
            MaterialComponent m;
            m.params = prakriti::MaterialRegistry::water();
            m.temperature = 20.0f;
            m.phase_fractions = prakriti::phase_from_temperature(m.temperature, m.params);
            return m;
        }

        [[nodiscard]] static MaterialComponent Glass() noexcept {
            MaterialComponent m;
            m.params.rest_density = 2500.0f;
            m.params.heat_capacity = 0.84f;
            m.params.conductivity = 0.8f;
            m.params.melt_temp = 1400.0f;
            m.params.boil_temp = 2230.0f;
            m.params.yield_strain = 0.0005f;
            m.params.ultimate_strain = 0.001f; // Extremely brittle
            m.params.youngs_modulus = 7e10f;
            m.temperature = 20.0f;
            m.phase_fractions = prakriti::phase_from_temperature(m.temperature, m.params);
            return m;
        }

        [[nodiscard]] static MaterialComponent Steel() noexcept {
            MaterialComponent m;
            m.params = prakriti::MaterialRegistry::steel();
            m.temperature = 20.0f;
            m.phase_fractions = prakriti::phase_from_temperature(m.temperature, m.params);
            return m;
        }

        [[nodiscard]] static MaterialComponent Wood() noexcept {
            MaterialComponent m;
            m.params.rest_density = 700.0f;
            m.params.heat_capacity = 1.7f;
            m.params.conductivity = 0.15f;
            m.params.melt_temp = 300.0f; // Combustion/char point
            m.params.boil_temp = 400.0f;
            m.params.yield_strain = 0.01f;
            m.params.ultimate_strain = 0.03f;
            m.params.youngs_modulus = 1.2e10f;
            m.temperature = 20.0f;
            m.flammable = true;
            m.phase_fractions = prakriti::phase_from_temperature(m.temperature, m.params);
            return m;
        }

        [[nodiscard]] static MaterialComponent Lava() noexcept {
            MaterialComponent m;
            m.params.rest_density = 2800.0f;
            m.params.heat_capacity = 1.0f;
            m.params.conductivity = 2.0f;
            m.params.melt_temp = 700.0f;
            m.params.boil_temp = 1600.0f;
            m.params.visc = {0.0f, 0.5f, 5.0f, 0.01f}; // High viscosity liquid
            m.temperature = 1100.0f; // Molten
            m.phase_fractions = prakriti::phase_from_temperature(m.temperature, m.params);
            return m;
        }

        // Update thermodynamic phase state
        void update_thermodynamics(float delta_temp, float dt) noexcept {
            temperature += delta_temp;
            phase_fractions = prakriti::phase_from_temperature(temperature, params);

            if (flammable && temperature >= params.melt_temp) {
                burn_time += dt;
                damage = std::min(1.0f, damage + dt * 0.2f);
            }
        }
    };
} // namespace gati
