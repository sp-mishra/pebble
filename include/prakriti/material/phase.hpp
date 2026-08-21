#pragma once
// ============================================================================
// prakriti/material/phase.hpp — continuous 4-fraction phase model + barycentric blends.
// Phase fractions {solid,plastic,liquid,gas} sum to 1; behaviour blends smoothly.
// ============================================================================
#include "../core/config.hpp"
#include "../state/material_registry.hpp"
#include <array>
#include <algorithm>

namespace prakriti {

struct PhaseFractions {
    std::array<Scalar, kPhaseCount> f{Scalar(1), Scalar(0), Scalar(0), Scalar(0)};

    [[nodiscard]] constexpr Scalar solid()   const noexcept { return f[kSolid]; }
    [[nodiscard]] constexpr Scalar plastic() const noexcept { return f[kPlastic]; }
    [[nodiscard]] constexpr Scalar liquid()  const noexcept { return f[kLiquid]; }
    [[nodiscard]] constexpr Scalar gas()     const noexcept { return f[kGas]; }
};

// Barycentric blend of a per-phase quantity by the current fractions.
[[nodiscard]] constexpr Scalar
phase_blend(const std::array<Scalar, kPhaseCount>& per_phase, const PhaseFractions& pf) noexcept {
    Scalar acc = Scalar(0);
    for (int i = 0; i < kPhaseCount; ++i) acc += pf.f[i] * per_phase[i];
    return acc;
}

// Smooth ramp helper: 0 below lo, 1 above hi, smoothstep between.
[[nodiscard]] constexpr Scalar smoothramp(Scalar t, Scalar lo, Scalar hi) noexcept {
    if (hi <= lo) return t >= hi ? Scalar(1) : Scalar(0);
    const Scalar u = std::clamp((t - lo) / (hi - lo), Scalar(0), Scalar(1));
    return u * u * (Scalar(3) - Scalar(2) * u); // smoothstep
}

// Map temperature to phase fractions through melt/boil transitions.
// A small transition band around each threshold gives continuous (not binary) fractions.
[[nodiscard]] inline PhaseFractions
phase_from_temperature(Scalar temp, const MaterialParams& p,
                       Scalar plastic_band = Scalar(0.15)) noexcept {
    const Scalar melt_band = std::max(Scalar(1), (p.boil_temp - p.melt_temp) * Scalar(0.05));
    const Scalar boil_band = std::max(Scalar(1), (p.boil_temp - p.melt_temp) * Scalar(0.05));

    const Scalar liquid_amt = smoothramp(temp, p.melt_temp, p.melt_temp + melt_band);
    const Scalar gas_amt    = smoothramp(temp, p.boil_temp, p.boil_temp + boil_band);

    PhaseFractions pf;
    // gas dominates above boil; liquid between melt and boil; solid below melt.
    const Scalar g = gas_amt;
    const Scalar l = liquid_amt * (Scalar(1) - g);
    const Scalar remaining = Scalar(1) - g - l; // solid + plastic share
    // Near yield the solid softens into a plastic sliver as it approaches melt.
    const Scalar pl = remaining * plastic_band * liquid_amt;
    const Scalar so = remaining - pl;
    pf.f = {so, pl, l, g};
    return pf;
}

} // namespace prakriti
