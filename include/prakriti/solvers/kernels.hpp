#pragma once
// ============================================================================
// prakriti/solvers/kernels.hpp — SPH smoothing kernels (2D poly6 / spiky gradient).
// Directly reuses pebble::math::vec2 and pebble::math::length.
// ============================================================================
#include "../core/config.hpp"
#include <containers/numeric/math_vector.hpp>
#include <cmath>
#include <numbers>

namespace prakriti::kernels {

// 2D poly6 kernel: W(r,h) = 4/(π h^8) (h²−r²)³ for 0<=r<=h else 0.
[[nodiscard]] inline Scalar poly6(Scalar r2, Scalar h) noexcept {
    const Scalar h2 = h * h;
    if (r2 >= h2) return Scalar(0);
    const Scalar diff = h2 - r2;
    const Scalar coeff = Scalar(4) / (std::numbers::pi_v<Scalar> * std::pow(h, Scalar(8)));
    return coeff * diff * diff * diff;
}

// 2D spiky gradient: ∇W = −30/(π h^5) (h−r)² r̂.
[[nodiscard]] inline pebble::math::vec2 spiky_grad(const pebble::math::vec2& rij, Scalar h) noexcept {
    const Scalar r = pebble::math::length(rij);
    if (r <= Scalar(1e-6) || r >= h) return pebble::math::vec2{0.0f, 0.0f};
    const Scalar coeff = Scalar(-30) / (std::numbers::pi_v<Scalar> * std::pow(h, Scalar(5)));
    const Scalar factor = coeff * (h - r) * (h - r) / r;
    return pebble::math::vec2{rij[0] * factor, rij[1] * factor};
}

} // namespace prakriti::kernels
