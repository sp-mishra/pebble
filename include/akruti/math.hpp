#pragma once
// akruti/math.hpp — 2D Math Layer for the Akruti shape system.
// Directly reuses Pebble's core tensor math (pebble::math / math_vector.hpp).
// Eliminates duplicate vector/matrix math implementations across Pebble.
#include <containers/numeric/math_vector.hpp>
#include <containers/matrix/static.hpp>
#include <cmath>
#include <algorithm>

namespace akruti {

// Precision scalar
using Scalar = float;

// Forward declaration & alias
template <class T> struct Vec2;
using Vec = Vec2<Scalar>;

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

// ── 2x2 Matrix: alias to ga::StaticMatrix<T,2,2> ─────────────────────────────
// ga::StaticMatrix<T,2,2> provides constexpr .det()/.inv()/.transpose()/operator*/operator*(vec),
// zero-heap std::array storage, and optional SIMD specialization — identical interface to the
// former hand-written Mat2<T> body, but canonical and shared with the matrix library.
template <class T = Scalar>
using Mat2 = ga::StaticMatrix<T, 2, 2>;

// Rotation matrix factory (not provided by ga::StaticMatrix directly)
template <class T = Scalar>
[[nodiscard]] inline Mat2<T> make_rotation2d(T radians) noexcept {
    const T c = std::cos(radians);
    const T s = std::sin(radians);
    return Mat2<T>{c, -s, s, c};
}

// mul(Mat2, Vec2) — matrix-vector multiply returning akruti::Vec2<T>
// Bridges ga::StaticMatrix<T,2,2> * akruti::Vec2<T> for primitives/narrowphase call sites.
template <class T = Scalar>
[[nodiscard]] constexpr Vec2<T> mul(const Mat2<T>& m, const Vec2<T>& v) noexcept {
    return Vec2<T>{m(0,0) * v.x + m(0,1) * v.y,
                   m(1,0) * v.x + m(1,1) * v.y};
}

// operator* overload so existing `rot * vec` syntax compiles
// (ga::StaticMatrix * ga::StaticMatrix<T,2,1> works natively; this handles akruti::Vec2)
template <class T = Scalar>
[[nodiscard]] constexpr Vec2<T> operator*(const Mat2<T>& m, const Vec2<T>& v) noexcept {
    return mul(m, v);
}

// ── Axis-Aligned Bounding Box (AABB) using Pebble Math aabb2 ──────────────────
template <class T = Scalar>
using AABB = pebble::math::aabb2;

} // namespace akruti
