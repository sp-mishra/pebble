#pragma once
// ============================================================================
// prakriti/state/material_registry.hpp — immutable per-material constants (SoA lookup).
// Static physical rules; never mutated during simulation. Indexed by MaterialId.
// ============================================================================
#include "../core/config.hpp"
#include <vector>
#include <array>

namespace prakriti {
    // Phase index convention used across the engine.
    enum Phase : int { kSolid = 0, kPlastic = 1, kLiquid = 2, kGas = 3, kPhaseCount = 4 };

    struct MaterialParams {
        Scalar rest_density = Scalar(1000);
        Scalar heat_capacity = Scalar(1); // specific heat c
        Scalar conductivity = Scalar(0.5); // thermal k
        Scalar melt_temp = Scalar(0);
        Scalar boil_temp = Scalar(100);
        Scalar latent_heat_fusion = Scalar(0); // energy plateau solid<->liquid
        Scalar latent_heat_vapor = Scalar(0); // energy plateau liquid<->gas
        Scalar yield_strain = Scalar(0.02); // ε_yield
        Scalar ultimate_strain = Scalar(0.2); // ε_ultimate
        Scalar youngs_modulus = Scalar(1e5);
        Scalar damage_exponent = Scalar(1); // β
        // Per-phase constraint compliance α and viscosity μ (indexed by Phase).
        std::array<Scalar, kPhaseCount> alpha{Scalar(1e-7), Scalar(1e-5), Scalar(1e-2), Scalar(1)};
        std::array<Scalar, kPhaseCount> visc{Scalar(0), Scalar(0.05), Scalar(0.2), Scalar(0.01)};
        // Equation-of-state constants.
        Scalar eos_B = Scalar(1e5); // stiffness
        Scalar eos_gamma = Scalar(7); // Tait exponent
        Scalar eos_R = Scalar(50); // gas thermal expansion
    };

    class MaterialRegistry {
    public:
        MaterialRegistry() = default;

        [[nodiscard]] MaterialId add(const MaterialParams& p) {
            params_.push_back(p);
            return static_cast<MaterialId>(params_.size() - 1);
        }

        [[nodiscard]] const MaterialParams& view(MaterialId id) const noexcept { return params_[id]; }
        [[nodiscard]] std::size_t size() const noexcept { return params_.size(); }

        // Plug-and-play presets — decent defaults so callers can start immediately.
        [[nodiscard]] static MaterialParams steel() noexcept {
            MaterialParams p;
            p.rest_density = Scalar(7850);
            p.heat_capacity = Scalar(0.49);
            p.conductivity = Scalar(50);
            p.melt_temp = Scalar(1500);
            p.boil_temp = Scalar(2860);
            p.latent_heat_fusion = Scalar(247);
            p.yield_strain = Scalar(0.002);
            p.ultimate_strain = Scalar(0.05);
            p.youngs_modulus = Scalar(2e11);
            p.alpha = {Scalar(1e-9), Scalar(1e-6), Scalar(1e-2), Scalar(1)};
            return p;
        }

        [[nodiscard]] static MaterialParams water() noexcept {
            MaterialParams p;
            p.rest_density = Scalar(1000);
            p.heat_capacity = Scalar(4.18);
            p.conductivity = Scalar(0.6);
            p.melt_temp = Scalar(0);
            p.boil_temp = Scalar(100);
            p.latent_heat_fusion = Scalar(334);
            p.latent_heat_vapor = Scalar(2257);
            p.yield_strain = Scalar(0);
            p.ultimate_strain = Scalar(1e-3);
            p.youngs_modulus = Scalar(0);
            p.alpha = {Scalar(1e-3), Scalar(1e-2), Scalar(1e-1), Scalar(1)};
            p.eos_B = Scalar(2e5);
            return p;
        }

        [[nodiscard]] static MaterialParams dry_ice() noexcept {
            MaterialParams p;
            p.rest_density = Scalar(1560);
            p.heat_capacity = Scalar(1.2);
            p.conductivity = Scalar(0.02);
            p.melt_temp = Scalar(-78.5);
            p.boil_temp = Scalar(-78.5); // Sublimes directly at -78.5 C
            p.latent_heat_fusion = Scalar(0);
            p.latent_heat_vapor = Scalar(571); // Sublimation enthalpy
            p.yield_strain = Scalar(0.01);
            p.ultimate_strain = Scalar(0.02);
            p.youngs_modulus = Scalar(1e9);
            p.alpha = {Scalar(1e-7), Scalar(1e-4), Scalar(1e-2), Scalar(1)};
            return p;
        }

        [[nodiscard]] static MaterialParams magma() noexcept {
            MaterialParams p;
            p.rest_density = Scalar(2600);
            p.heat_capacity = Scalar(1.4);
            p.conductivity = Scalar(2.5);
            p.melt_temp = Scalar(700);
            p.boil_temp = Scalar(2200);
            p.latent_heat_fusion = Scalar(400);
            p.yield_strain = Scalar(0.05);
            p.ultimate_strain = Scalar(0.15);
            p.youngs_modulus = Scalar(1e8);
            p.alpha = {Scalar(1e-8), Scalar(1e-5), Scalar(5e-2), Scalar(1)};
            p.visc = {Scalar(0), Scalar(0.2), Scalar(0.8), Scalar(0.01)}; // Highly viscous fluid
            p.eos_B = Scalar(4e5);
            return p;
        }

        [[nodiscard]] static MaterialParams obsidian() noexcept {
            MaterialParams p;
            p.rest_density = Scalar(2600);
            p.heat_capacity = Scalar(0.9);
            p.conductivity = Scalar(1.3);
            p.melt_temp = Scalar(850);
            p.boil_temp = Scalar(2400);
            p.latent_heat_fusion = Scalar(350);
            p.yield_strain = Scalar(0.005);
            p.ultimate_strain = Scalar(0.01);
            p.youngs_modulus = Scalar(7e10); // Hard brittle volcanic glass
            p.alpha = {Scalar(1e-9), Scalar(1e-6), Scalar(1e-2), Scalar(1)};
            return p;
        }

    private:
        std::vector<MaterialParams> params_;
    };
} // namespace prakriti
