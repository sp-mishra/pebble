#pragma once
// ============================================================================
// dual.hpp — Ganita Dual<T,N> forward-mode automatic differentiation
// ============================================================================
// Dual<T, N> = value T + derivative std::array<T,N>.
// All arithmetic operators + math functions overloaded.
// Any Ganita algorithm templated on T works for T = Dual<float,K>.
// grad(f, x) computes ∇f(x) in one forward pass (N=1 for scalar gradient).
// CPU-only (static_assert prevents GPU instantiation).
// ============================================================================

#ifndef PEBBLE_CONTAINERS_MATRIX_DUAL_HPP
#define PEBBLE_CONTAINERS_MATRIX_DUAL_HPP

#include <array>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <type_traits>

namespace ga {

    // -----------------------------------------------------------------------
    // Dual<T, N> — dual number with N derivative components
    // -----------------------------------------------------------------------
    template<typename T, std::size_t N = 1>
    class Dual {
        static_assert(std::is_floating_point_v<T>,
                      "Dual<T,N>: T must be a floating-point type (CPU-only)");
    public:
        using value_type = T;
        static constexpr std::size_t dim = N;

        T v{};                   // value
        std::array<T, N> d{};   // derivative tuple (∂/∂x₀, ..., ∂/∂x_{N-1})

        constexpr Dual() noexcept = default;
        constexpr explicit Dual(T val) noexcept : v(val) { d.fill(T{}); }
        constexpr Dual(T val, std::array<T,N> deriv) noexcept : v(val), d(deriv) {}

        // Seed for variable x_i: value = x, d[i] = 1
        static constexpr Dual variable(T val, std::size_t i = 0) noexcept {
            Dual r(val);
            r.d[i] = T{1};
            return r;
        }

        // Arithmetic
        [[nodiscard]] constexpr Dual operator+(const Dual& o) const noexcept {
            Dual r; r.v = v + o.v;
            for (std::size_t i=0;i<N;++i) r.d[i] = d[i] + o.d[i];
            return r;
        }
        [[nodiscard]] constexpr Dual operator-(const Dual& o) const noexcept {
            Dual r; r.v = v - o.v;
            for (std::size_t i=0;i<N;++i) r.d[i] = d[i] - o.d[i];
            return r;
        }
        [[nodiscard]] constexpr Dual operator-() const noexcept {
            Dual r; r.v = -v;
            for (std::size_t i=0;i<N;++i) r.d[i] = -d[i];
            return r;
        }
        [[nodiscard]] constexpr Dual operator*(const Dual& o) const noexcept {
            Dual r; r.v = v * o.v;
            for (std::size_t i=0;i<N;++i) r.d[i] = d[i]*o.v + v*o.d[i];
            return r;
        }
        [[nodiscard]] constexpr Dual operator/(const Dual& o) const noexcept {
            Dual r; r.v = v / o.v;
            T inv = T{1} / (o.v * o.v);
            for (std::size_t i=0;i<N;++i) r.d[i] = (d[i]*o.v - v*o.d[i]) * inv;
            return r;
        }

        // Scalar arithmetic
        template<typename S> requires std::is_arithmetic_v<S>
        [[nodiscard]] constexpr Dual operator+(S s) const noexcept { return *this + Dual(static_cast<T>(s)); }
        template<typename S> requires std::is_arithmetic_v<S>
        [[nodiscard]] constexpr Dual operator-(S s) const noexcept { return *this - Dual(static_cast<T>(s)); }
        template<typename S> requires std::is_arithmetic_v<S>
        [[nodiscard]] constexpr Dual operator*(S s) const noexcept {
            Dual r; r.v = v*static_cast<T>(s);
            for (std::size_t i=0;i<N;++i) r.d[i] = d[i]*static_cast<T>(s);
            return r;
        }
        template<typename S> requires std::is_arithmetic_v<S>
        [[nodiscard]] constexpr Dual operator/(S s) const noexcept {
            Dual r; r.v = v/static_cast<T>(s);
            for (std::size_t i=0;i<N;++i) r.d[i] = d[i]/static_cast<T>(s);
            return r;
        }
        template<typename S> requires std::is_arithmetic_v<S>
        [[nodiscard]] friend constexpr Dual operator+(S s, const Dual& x) noexcept { return x + s; }
        template<typename S> requires std::is_arithmetic_v<S>
        [[nodiscard]] friend constexpr Dual operator-(S s, const Dual& x) noexcept { return Dual(static_cast<T>(s)) - x; }
        template<typename S> requires std::is_arithmetic_v<S>
        [[nodiscard]] friend constexpr Dual operator*(S s, const Dual& x) noexcept { return x * s; }
        template<typename S> requires std::is_arithmetic_v<S>
        [[nodiscard]] friend constexpr Dual operator/(S s, const Dual& x) noexcept { return Dual(static_cast<T>(s)) / x; }

        // Compound assignment
        constexpr Dual& operator+=(const Dual& o) noexcept { *this = *this + o; return *this; }
        constexpr Dual& operator-=(const Dual& o) noexcept { *this = *this - o; return *this; }
        constexpr Dual& operator*=(const Dual& o) noexcept { *this = *this * o; return *this; }
        constexpr Dual& operator/=(const Dual& o) noexcept { *this = *this / o; return *this; }

        // Comparison (by value only — needed for std algorithms)
        [[nodiscard]] constexpr bool operator<(const Dual& o)  const noexcept { return v < o.v; }
        [[nodiscard]] constexpr bool operator>(const Dual& o)  const noexcept { return v > o.v; }
        [[nodiscard]] constexpr bool operator<=(const Dual& o) const noexcept { return v <= o.v; }
        [[nodiscard]] constexpr bool operator>=(const Dual& o) const noexcept { return v >= o.v; }
        [[nodiscard]] constexpr bool operator==(const Dual& o) const noexcept { return v == o.v; }
    };

    // -----------------------------------------------------------------------
    // Math functions on Dual (chain rule for each)
    // -----------------------------------------------------------------------
    template<typename T, std::size_t N>
    [[nodiscard]] Dual<T,N> sqrt(const Dual<T,N>& x) noexcept {
        T sv = std::sqrt(x.v);
        Dual<T,N> r; r.v = sv;
        T inv2 = (sv > T{0}) ? T{0.5}/sv : T{0};
        for (std::size_t i=0;i<N;++i) r.d[i] = inv2 * x.d[i];
        return r;
    }
    template<typename T, std::size_t N>
    [[nodiscard]] Dual<T,N> exp(const Dual<T,N>& x) noexcept {
        T ev = std::exp(x.v);
        Dual<T,N> r; r.v = ev;
        for (std::size_t i=0;i<N;++i) r.d[i] = ev * x.d[i];
        return r;
    }
    template<typename T, std::size_t N>
    [[nodiscard]] Dual<T,N> log(const Dual<T,N>& x) noexcept {
        Dual<T,N> r; r.v = std::log(x.v);
        T inv = (x.v > T{0}) ? T{1}/x.v : T{0};
        for (std::size_t i=0;i<N;++i) r.d[i] = inv * x.d[i];
        return r;
    }
    template<typename T, std::size_t N>
    [[nodiscard]] Dual<T,N> sin(const Dual<T,N>& x) noexcept {
        T c = std::cos(x.v);
        Dual<T,N> r; r.v = std::sin(x.v);
        for (std::size_t i=0;i<N;++i) r.d[i] = c * x.d[i];
        return r;
    }
    template<typename T, std::size_t N>
    [[nodiscard]] Dual<T,N> cos(const Dual<T,N>& x) noexcept {
        T s = -std::sin(x.v);
        Dual<T,N> r; r.v = std::cos(x.v);
        for (std::size_t i=0;i<N;++i) r.d[i] = s * x.d[i];
        return r;
    }
    template<typename T, std::size_t N>
    [[nodiscard]] Dual<T,N> tan(const Dual<T,N>& x) noexcept {
        T c = std::cos(x.v);
        T sec2 = (c != T{0}) ? T{1}/(c*c) : T{0};
        Dual<T,N> r; r.v = std::tan(x.v);
        for (std::size_t i=0;i<N;++i) r.d[i] = sec2 * x.d[i];
        return r;
    }
    template<typename T, std::size_t N>
    [[nodiscard]] Dual<T,N> abs(const Dual<T,N>& x) noexcept {
        T sign = (x.v >= T{0}) ? T{1} : T{-1};
        Dual<T,N> r; r.v = std::abs(x.v);
        for (std::size_t i=0;i<N;++i) r.d[i] = sign * x.d[i];
        return r;
    }
    template<typename T, std::size_t N>
    [[nodiscard]] Dual<T,N> pow(const Dual<T,N>& x, T p) noexcept {
        T pv = std::pow(x.v, p);
        T dpv = (x.v != T{0}) ? p * std::pow(x.v, p - T{1}) : T{0};
        Dual<T,N> r; r.v = pv;
        for (std::size_t i=0;i<N;++i) r.d[i] = dpv * x.d[i];
        return r;
    }

    // -----------------------------------------------------------------------
    // grad — compute ∇f(x) via Dual<T,1> (scalar gradient, N=1)
    // f: callable (Dual<T,1>) -> Dual<T,1>
    // Returns std::array<T,1> = {df/dx}
    // -----------------------------------------------------------------------
    template<typename T, typename F>
    [[nodiscard]] T grad(F&& f, T x) {
        auto d = Dual<T,1>::variable(x, 0);
        auto res = f(d);
        return res.d[0];
    }

    // grad for vector x (returns Jacobian column j via Dual<T,1> seeding)
    template<typename T, std::size_t N, typename F>
    [[nodiscard]] std::array<T,N> grad_vec(F&& f,
                                             const std::array<T,N>& x) {
        std::array<T,N> g{};
        for (std::size_t i = 0; i < N; ++i) {
            // Seed only component i; all others have zero derivative
            std::array<Dual<T,1>, N> dx;
            for (std::size_t j = 0; j < N; ++j) {
                dx[j].v    = x[j];
                dx[j].d[0] = (j == i) ? T{1} : T{0};
            }
            auto result = f(dx);
            g[i] = result.d[0];
        }
        return g;
    }

    // -----------------------------------------------------------------------
    // Type alias for common use
    // -----------------------------------------------------------------------
    template<typename T> using DualScalar = Dual<T, 1>;

} // namespace ga

#endif // PEBBLE_CONTAINERS_MATRIX_DUAL_HPP
