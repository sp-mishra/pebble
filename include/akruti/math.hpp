#pragma once
// akruti/math.hpp — 2D Math Layer for the Akruti shape system.
// Directly reuses Pebble's core tensor math (pebble::math / math_vector.hpp).
// Eliminates duplicate vector/matrix math implementations across Pebble.
#include <containers/numeric/math_vector.hpp>
#include <cmath>
#include <algorithm>

namespace akruti {

// Precision scalar
using Scalar = float;

// ── Reusing Pebble Math Types Directly ─────────────────────────────────────────
using Vec2_base = pebble::math::vec2;
using Mat2_base = pebble::math::mat2;

// Concrete POD Vector type extending pebble::math::vec2 with named .x, .y accessors
template <class T = Scalar>
struct Vec2 {
    T x{};
    T y{};

    constexpr Vec2() noexcept = default;
    constexpr Vec2(T x_, T y_) noexcept : x(x_), y(y_) {}

    // Zero-overhead constructor from pebble::math::vec2 / static_tensor
    template <class Storage, class Comp>
    constexpr Vec2(const ts::static_tensor<T, Storage, Comp, 2>& t) noexcept
        : x(t[0]), y(t[1]) {}

    // Implicit conversion to pebble::math::vec2
    [[nodiscard]] constexpr operator pebble::math::vec2() const noexcept {
        return pebble::math::vec2(static_cast<float>(x), static_cast<float>(y));
    }

    [[nodiscard]] constexpr Vec2 operator+(const Vec2& o) const noexcept { return {x + o.x, y + o.y}; }
    [[nodiscard]] constexpr Vec2 operator-(const Vec2& o) const noexcept { return {x - o.x, y - o.y}; }
    [[nodiscard]] constexpr Vec2 operator-() const noexcept { return {-x, -y}; }
    [[nodiscard]] constexpr Vec2 operator*(T s) const noexcept { return {x * s, y * s}; }
    [[nodiscard]] constexpr Vec2 operator/(T s) const noexcept { return {x / s, y / s}; }

    constexpr Vec2& operator+=(const Vec2& o) noexcept { x += o.x; y += o.y; return *this; }
    constexpr Vec2& operator-=(const Vec2& o) noexcept { x -= o.x; y -= o.y; return *this; }
    constexpr Vec2& operator*=(T s) noexcept { x *= s; y *= s; return *this; }

    [[nodiscard]] constexpr T dot(const Vec2& o) const noexcept {
        return pebble::math::dot(static_cast<pebble::math::vec2>(*this),
                                 static_cast<pebble::math::vec2>(o));
    }
    [[nodiscard]] constexpr T len2() const noexcept {
        return pebble::math::length_sq(static_cast<pebble::math::vec2>(*this));
    }
    [[nodiscard]] T len() const noexcept {
        return pebble::math::length(static_cast<pebble::math::vec2>(*this));
    }

    // 90-degree CCW perpendicular vector using pebble::math::perp
    [[nodiscard]] constexpr Vec2 perp() const noexcept {
        return Vec2(pebble::math::perp(static_cast<pebble::math::vec2>(*this)));
    }

    [[nodiscard]] Vec2 normalized(T eps = T(1e-12)) const noexcept {
        const T l2 = len2();
        if (l2 <= eps) return Vec2{};
        return Vec2(pebble::math::normalize(static_cast<pebble::math::vec2>(*this)));
    }

    friend constexpr bool operator==(const Vec2&, const Vec2&) = default;
};

template <class T>
[[nodiscard]] constexpr Vec2<T> operator*(T s, const Vec2<T>& v) noexcept { return v * s; }

template <class T>
[[nodiscard]] constexpr T dot(const Vec2<T>& a, const Vec2<T>& b) noexcept { return a.dot(b); }

// 2D scalar cross product using pebble::math::cross
template <class T>
[[nodiscard]] constexpr T cross(const Vec2<T>& a, const Vec2<T>& b) noexcept {
    return pebble::math::cross(static_cast<pebble::math::vec2>(a),
                               static_cast<pebble::math::vec2>(b));
}

template <class T>
[[nodiscard]] constexpr Vec2<T> perp(const Vec2<T>& v) noexcept {
    return v.perp();
}

template <class T>
[[nodiscard]] T distance(const Vec2<T>& a, const Vec2<T>& b) noexcept {
    return pebble::math::distance(static_cast<pebble::math::vec2>(a),
                                  static_cast<pebble::math::vec2>(b));
}

template <class T>
[[nodiscard]] constexpr T distance2(const Vec2<T>& a, const Vec2<T>& b) noexcept { return (a - b).len2(); }

// ── 2x2 Matrix using Pebble Math ──────────────────────────────────────────────
template <class T = Scalar>
struct Mat2 {
    T m00{1}, m01{};
    T m10{}, m11{1};

    constexpr Mat2() noexcept = default;
    constexpr Mat2(T m00_, T m01_, T m10_, T m11_) noexcept
        : m00(m00_), m01(m01_), m10(m10_), m11(m11_) {}

    template <class Storage, class Comp>
    constexpr Mat2(const ts::static_tensor<T, Storage, Comp, 2, 2>& tensor_m) noexcept
        : m00(tensor_m[0, 0]), m01(tensor_m[0, 1]), m10(tensor_m[1, 0]), m11(tensor_m[1, 1]) {}

    [[nodiscard]] constexpr operator pebble::math::mat2() const noexcept {
        return pebble::math::mat2(
            static_cast<float>(m00), static_cast<float>(m01),
            static_cast<float>(m10), static_cast<float>(m11)
        );
    }

    [[nodiscard]] constexpr Vec2<T> operator*(const Vec2<T>& v) const noexcept {
        return Vec2<T>(pebble::math::mul(static_cast<pebble::math::mat2>(*this),
                                         static_cast<pebble::math::vec2>(v)));
    }
    [[nodiscard]] constexpr Mat2 operator*(const Mat2& o) const noexcept {
        return Mat2(pebble::math::mul(static_cast<pebble::math::mat2>(*this),
                                      static_cast<pebble::math::mat2>(o)));
    }
    [[nodiscard]] constexpr T det() const noexcept {
        return pebble::math::determinant(static_cast<pebble::math::mat2>(*this));
    }
    [[nodiscard]] constexpr Mat2 transpose() const noexcept {
        return Mat2(pebble::math::transpose(static_cast<pebble::math::mat2>(*this)));
    }
    [[nodiscard]] constexpr Mat2 inverse() const noexcept {
        return Mat2(pebble::math::inverse(static_cast<pebble::math::mat2>(*this)));
    }

    [[nodiscard]] static Mat2 rotation(T radians) noexcept {
        return Mat2(pebble::math::rotation2d(radians));
    }
    [[nodiscard]] static constexpr Mat2 scale(T sx, T sy) noexcept {
        return Mat2(pebble::math::scaling2d(sx, sy));
    }
    [[nodiscard]] static constexpr Mat2 shear_x(T k) noexcept { return {1, k, 0, 1}; }
    [[nodiscard]] static constexpr Mat2 shear_y(T k) noexcept { return {1, 0, k, 1}; }

    friend constexpr bool operator==(const Mat2&, const Mat2&) = default;
};

// ── Axis-Aligned Bounding Box (AABB) using Pebble Math aabb2 ──────────────────
template <class T = Scalar>
using AABB = pebble::math::aabb2;

} // namespace akruti
