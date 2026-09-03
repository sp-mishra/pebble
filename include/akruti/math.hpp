#pragma once
// akruti/math.hpp — 2D Math Layer for the Akruti shape system.
// Directly aliases and reuses Pebble's core tensor math (pebble::math / math_vector.hpp).
// Eliminates duplicate vector/matrix math implementations across Pebble.
#include <containers/numeric/math_vector.hpp>
#include <containers/matrix/static.hpp>
#include <cmath>
#include <type_traits>

namespace akruti {
    // Precision scalar
    using Scalar = float;

    // ── Reusing Pebble Math Types Directly (Single Source of Truth) ─────────────
    using Vec = pebble::math::vec2;
    template <class T = Scalar>
    using Vec2 = std::conditional_t<std::is_same_v<T, double>, pebble::math::vec2d, pebble::math::vec2>;

    using Mat2_base = pebble::math::mat2;
    template <class T = Scalar>
    using Mat2 = ga::StaticMatrix<T, 2, 2>;
    using AABB = pebble::math::aabb2;
    using Box2 = pebble::math::aabb2;

    // ── Forward Pebble Math Free Functions Directly ─────────────────────────────
    using pebble::math::x;
    using pebble::math::y;
    using pebble::math::z;
    using pebble::math::w;
    using pebble::math::dot;
    using pebble::math::cross;
    using pebble::math::perp;
    using pebble::math::length;
    using pebble::math::length_sq;
    using pebble::math::normalize;
    using pebble::math::distance;
    using pebble::math::distance_sq;
    using pebble::math::lerp;

    // Convenience distance aliases
    template <class T>
    [[nodiscard]] inline constexpr auto distance2(const T& a, const T& b) noexcept {
        return pebble::math::distance_sq(a, b);
    }

    // Rotation matrix factory (using ga::StaticMatrix for geometry operations)
    template <class T = Scalar>
    [[nodiscard]] inline Mat2<T> make_rotation2d(T radians) noexcept {
        const T c = std::cos(radians);
        const T s = std::sin(radians);
        return Mat2<T>{c, -s, s, c};
    }

    // Matrix-vector multiply for ga::StaticMatrix * pebble::math::vec2
    template <class T = Scalar>
    [[nodiscard]] constexpr Vec2<T> mul(const Mat2<T>& m, const Vec2<T>& v) noexcept {
        return Vec2<T>(
            m(0, 0) * v[0] + m(0, 1) * v[1],
            m(1, 0) * v[0] + m(1, 1) * v[1]
        );
    }

    template <class T = Scalar>
    [[nodiscard]] constexpr Vec2<T> operator*(const Mat2<T>& m, const Vec2<T>& v) noexcept {
        return mul(m, v);
    }
} // namespace akruti
