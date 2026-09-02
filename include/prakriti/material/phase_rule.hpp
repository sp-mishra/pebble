#pragma once
// ============================================================================
// prakriti/material/phase_rule.hpp — Generic Rule-Based Phase Transition Engine
// ============================================================================
// Provides a concept-based, extensible pipeline for physical and chemical
// phase transformations (Melting, Freezing, Boiling, Condensation, Sublimation,
// Combustion, and Custom Inter-Material Reactions).
// ============================================================================
#include "phase.hpp"
#include "../core/config.hpp"
#include "../state/material_registry.hpp"
#include "../state/particle_store.hpp"
#include "rules/easy_rules.hpp"
#include <containers/numeric/math_vector.hpp>
#include <vector>
#include <functional>
#include <concepts>
#include <cmath>

namespace prakriti {
    // Invalid material sentinel ID
    constexpr MaterialId kInvalidMaterial = static_cast<MaterialId>(0xFFFF);

    // Transition trigger condition types
    enum class TransitionTrigger : std::uint8_t {
        Temperature, // Triggered by thermal threshold (Melt, Boil, Freeze, Condense)
        Pressure, // Triggered by pressure / stress limit (Cavitation, Solidification under high pressure)
        Strain, // Triggered by strain / shear yield
        Contact, // Triggered by chemical neighbor contact (e.g. Lava + Water -> Obsidian)
        Custom // User-defined custom predicate
    };

    // Generic Phase Rule descriptor
    struct PhaseRule {
        TransitionTrigger trigger = TransitionTrigger::Temperature;
        Scalar threshold_temp_lo = Scalar(0);
        Scalar threshold_temp_hi = Scalar(100);
        Scalar latent_energy = Scalar(0); // Latent heat absorbed (+) or released (-)
        MaterialId input_material = kInvalidMaterial;
        MaterialId output_material = kInvalidMaterial;

        // Dynamic buoyancy and volumetric impulse upon phase shift
        pebble::math::vec2 phase_impulse{0.0f, 0.0f};
        Scalar buoyancy_factor = Scalar(0); // Upward thermal acceleration for gas phases

        // Custom user predicate (optional)
        std::function<bool(const ParticleStore & P, Index i)> custom_condition;
        // Custom user effect (optional)
        std::function<void(ParticleStore & P, Index i, Scalar dt)> custom_effect;
    };

    // Built-in Generic Phase Rules
    namespace rules {
        // Generic Boiling & Vaporization Rule (Liquid -> Gas)
        [[nodiscard]] inline PhaseRule boiling(MaterialId water_mat, Scalar boil_temp = Scalar(100),
                                               Scalar buoyancy = Scalar(-1400)) {
            PhaseRule r;
            r.trigger = TransitionTrigger::Temperature;
            r.threshold_temp_lo = boil_temp;
            r.threshold_temp_hi = boil_temp + Scalar(10);
            r.latent_energy = Scalar(2260); // Latent heat of vaporization
            r.input_material = water_mat;
            r.buoyancy_factor = buoyancy;
            return r;
        }

        // Generic Condensation Rule (Gas -> Liquid)
        [[nodiscard]] inline PhaseRule condensation(MaterialId water_mat, Scalar boil_temp = Scalar(100)) {
            PhaseRule r;
            r.trigger = TransitionTrigger::Temperature;
            r.threshold_temp_lo = Scalar(-273);
            r.threshold_temp_hi = boil_temp;
            r.latent_energy = Scalar(-2260); // Latent heat released
            r.input_material = water_mat;
            return r;
        }

        // Generic Melting Rule (Solid -> Liquid)
        [[nodiscard]] inline PhaseRule melting(MaterialId mat, Scalar melt_temp = Scalar(0)) {
            PhaseRule r;
            r.trigger = TransitionTrigger::Temperature;
            r.threshold_temp_lo = melt_temp;
            r.threshold_temp_hi = melt_temp + Scalar(5);
            r.latent_energy = Scalar(334);
            r.input_material = mat;
            return r;
        }

        // Generic Freezing Rule (Liquid -> Solid)
        [[nodiscard]] inline PhaseRule freezing(MaterialId mat, Scalar melt_temp = Scalar(0)) {
            PhaseRule r;
            r.trigger = TransitionTrigger::Temperature;
            r.threshold_temp_lo = Scalar(-273);
            r.threshold_temp_hi = melt_temp;
            r.latent_energy = Scalar(-334);
            r.input_material = mat;
            return r;
        }
    } // namespace rules

    // Rule-Based Phase Transition Engine with EasyRules Integration
    class PhaseRuleEngine {
    public:
        PhaseRuleEngine() = default;

        void add_rule(PhaseRule rule) {
            rules_.push_back(std::move(rule));
        }

        // Access underlying Pebble EasyRules engine for high-level declarative facts & rules
        [[nodiscard]] easy_rules::EasyRuleEngine& easy_rules_engine() noexcept {
            return easy_rules_engine_;
        }

        [[nodiscard]] const easy_rules::EasyRuleEngine& easy_rules_engine() const noexcept {
            return easy_rules_engine_;
        }

        // Evaluate phase transition rules across the particle store
        void step(ParticleStore& P, const MaterialRegistry& materials, Scalar dt) {
            const Index n = P.size();

            for (Index i = 0; i < n; ++i) {
                const MaterialId mat_id = P.material[i];
                const MaterialParams& params = materials.view(mat_id);

                // 1. Evaluate baseline thermodynamic continuous phase fractions
                PhaseFractions pf = phase_from_temperature(P.temperature[i], params);

                // 2. Evaluate active user-defined and generic rules
                for (const auto& rule : rules_) {
                    if (rule.input_material != kInvalidMaterial && rule.input_material != mat_id) {
                        continue; // Skip rules for other materials
                    }

                    bool triggered = false;
                    switch (rule.trigger) {
                    case TransitionTrigger::Temperature:
                        triggered = (P.temperature[i] >= rule.threshold_temp_lo);
                        break;
                    case TransitionTrigger::Custom:
                        if (rule.custom_condition) {
                            triggered = rule.custom_condition(P, i);
                        }
                        break;
                    default:
                        break;
                    }

                    if (triggered) {
                        // Apply phase-dependent buoyancy acceleration
                        if (rule.buoyancy_factor != Scalar(0) && P.f_gas[i] > Scalar(0.2)) {
                            const Scalar buoyancy_force = rule.buoyancy_factor * P.f_gas[i]
                                * (Scalar(1) + (P.temperature[i] - rule.threshold_temp_lo) * Scalar(0.01));
                            P.vel_y[i] += buoyancy_force * dt;
                        }

                        // Apply material transition transformation (if output_material is set)
                        if (rule.output_material != kInvalidMaterial && P.f_gas[i] > Scalar(0.8)) {
                            P.material[i] = rule.output_material;
                        }

                        // Apply custom effect hook
                        if (rule.custom_effect) {
                            rule.custom_effect(P, i, dt);
                        }
                    }
                }

                // Write back evaluated phase fractions to store
                P.f_solid[i] = pf.solid();
                P.f_plastic[i] = pf.plastic();
                P.f_liquid[i] = pf.liquid();
                P.f_gas[i] = pf.gas();
            }
        }

    private:
        std::vector<PhaseRule> rules_;
        easy_rules::EasyRuleEngine easy_rules_engine_;
    };
} // namespace prakriti
