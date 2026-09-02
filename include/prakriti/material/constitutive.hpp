#pragma once
// ============================================================================
// prakriti/material/constitutive.hpp — Material Law: state -> solver coefficients.
// Static polymorphism via the MaterialLaw concept; DefaultMaterialLaw is the plug-and-play default.
// No virtual.
// ============================================================================
#include "../core/config.hpp"
#include "../state/material_registry.hpp"
#include "phase.hpp"
#include <concepts>

namespace prakriti {
    // A MaterialLaw converts material state into instantaneous solver coefficients.
    template <class L>
    concept MaterialLaw = requires(const L l, const MaterialParams& p, const PhaseFractions& pf,
                                   Scalar base_alpha, Scalar damage) {
        { l.effective_compliance(p, pf) } -> std::convertible_to<Scalar>;
        { l.effective_viscosity(p, pf) } -> std::convertible_to<Scalar>;
        { l.structural_alpha(base_alpha, damage) } -> std::convertible_to<Scalar>;
        { l.target_density(p, pf) } -> std::convertible_to<Scalar>;
    };

    struct DefaultMaterialLaw {
        // Barycentric blend of per-phase compliance.
        [[nodiscard]] Scalar effective_compliance(const MaterialParams& p,
                                                  const PhaseFractions& pf) const noexcept {
            return phase_blend(p.alpha, pf);
        }

        // Barycentric blend of per-phase viscosity.
        [[nodiscard]] Scalar effective_viscosity(const MaterialParams& p,
                                                 const PhaseFractions& pf) const noexcept {
            return phase_blend(p.visc, pf);
        }

        // Damage scales structural compliance: α_struct = α_base / (1 − D).
        [[nodiscard]] Scalar structural_alpha(Scalar base_alpha, Scalar damage) const noexcept {
            const Scalar d = damage >= Scalar(1) ? Scalar(0.999999) : damage;
            return base_alpha / (Scalar(1) - d);
        }

        // Gas expansion lowers effective target density.
        [[nodiscard]] Scalar target_density(const MaterialParams& p,
                                            const PhaseFractions& pf) const noexcept {
            const Scalar gas_scale = Scalar(1) - Scalar(0.9) * pf.gas();
            return p.rest_density * gas_scale;
        }
    };

    static_assert(MaterialLaw<DefaultMaterialLaw>);
} // namespace prakriti
