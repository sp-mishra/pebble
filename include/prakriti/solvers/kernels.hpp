#pragma once
// ============================================================================
// prakriti/solvers/kernels.hpp — SPH smoothing kernels (2D poly6 / spiky gradient).
// Directly reuses pebble::math::vec2 and pebble::math::length.
// ============================================================================
#include "../core/config.hpp"
#include <containers/numeric/math_vector.hpp>
#include <containers/matrix/static.hpp>
#include <cmath>
#include <numbers>

namespace prakriti::kernels {
    // 2D poly6 kernel: W(r,h) = 4/(π h^8) (h²−r²)³ for 0<=r<=h else 0.
    [[nodiscard]] inline Scalar poly6(Scalar r2, Scalar h) noexcept {
        const Scalar h2 = h * h;
        if (r2 >= h2) return Scalar(0);
        const Scalar diff = h2 - r2;
        const Scalar h4 = h2 * h2;
        const Scalar h8 = h4 * h4;
        const Scalar coeff = Scalar(4) / (std::numbers::pi_v<Scalar> * h8);
        return coeff * diff * diff * diff;
    }

    // 2D spiky gradient: ∇W = −30/(π h^5) (h−r)² r̂.
    [[nodiscard]] inline pebble::math::vec2 spiky_grad(const pebble::math::vec2& rij, Scalar h) noexcept {
        const Scalar r2 = ga::nrm2_sq(ga::Vec2<Scalar>{rij[0], rij[1]});
        const Scalar h2 = h * h;
        if (r2 <= Scalar(1e-12) || r2 >= h2) return pebble::math::vec2{0.0f, 0.0f};
        const Scalar r = std::sqrt(r2);
        const Scalar h4 = h2 * h2;
        const Scalar h5 = h4 * h;
        const Scalar coeff = Scalar(-30) / (std::numbers::pi_v<Scalar> * h5);
        const Scalar diff = h - r;
        const Scalar factor = (coeff * diff * diff) / r;
        return pebble::math::vec2{rij[0] * factor, rij[1] * factor};
    }
} // namespace prakriti::kernels
