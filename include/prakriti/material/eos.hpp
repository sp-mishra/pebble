#pragma once
// ============================================================================
// prakriti/material/eos.hpp — equation of state. Modified Tait + gas thermal expansion.
//   P = B((ρ/ρ0)^γ − 1) + R·f_gas·T
// ============================================================================
#include "../core/config.hpp"
#include "../state/material_registry.hpp"
#include "phase.hpp"
#include <cmath>

namespace prakriti {

[[nodiscard]] inline Scalar
tait_pressure(Scalar density, Scalar rest_density,
              const MaterialParams& p, Scalar f_gas, Scalar temp,
              bool clamp_negative) noexcept {
    const Scalar ratio = rest_density > Scalar(0) ? density / rest_density : Scalar(1);
    Scalar P = p.eos_B * (std::pow(ratio, p.eos_gamma) - Scalar(1))
             + p.eos_R * f_gas * temp;
    if (clamp_negative && P < Scalar(0)) P = Scalar(0);
    return P;
}

} // namespace prakriti
