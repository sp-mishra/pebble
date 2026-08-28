#pragma once
// ============================================================================
// gati/math.hpp — Direct Pebble Math Integrations & Interpolation Utilities
// ============================================================================
// Zero wrapper types: uses pebble::math directly from containers/numeric/math_vector.hpp.
// ============================================================================

#include "containers/numeric/math_vector.hpp"
#include <cmath>

namespace gati {

using Scalar = float;
using Vec2   = pebble::math::vec2;
using Mat2   = pebble::math::mat2;
using AABB   = pebble::math::aabb2;

// Scalar lerp
[[nodiscard]] constexpr Scalar lerp(Scalar a, Scalar b, Scalar t) noexcept {
    return a + (b - a) * t;
}

// Vec2 lerp using pebble::math::lerp
[[nodiscard]] constexpr Vec2 lerp(const Vec2& a, const Vec2& b, Scalar t) noexcept {
    return pebble::math::lerp(a, b, t);
}

// Shortest-arc angle interpolation (radians). Wraps delta to (-pi, pi] before blending.
[[nodiscard]] inline Scalar angle_lerp(Scalar a, Scalar b, Scalar t) noexcept {
    constexpr Scalar pi = Scalar(3.14159265358979323846);
    Scalar d = b - a;
    while (d > pi)  d -= Scalar(2) * pi;
    while (d < -pi) d += Scalar(2) * pi;
    return a + d * t;
}

} // namespace gati
