#pragma once
// ============================================================================
// dual.hppDual<T,N> forward-mode automatic differentiation
// ============================================================================
// Dual<T, N> = value T + derivative std::array<T,N>.
// All arithmetic operators + math functions overloaded.
// Any algorithm templated on T works for T = Dual<float,K>.
// grad(f, x) computes ∇f(x) in one forward pass (N=1 for scalar gradient).
// CPU-only (static_assert prevents GPU instantiation).
// ============================================================================

#ifndef PEBBLE_CONTAINERS_MATRIX_DUAL_HPP
#define PEBBLE_CONTAINERS_MATRIX_DUAL_HPP

#include <array>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <limits>
#include <type_traits>

namespace ga {
    // -----------------------------------------------------------------------
    // Dual<T, N> — dual number with N derivative components
    // -----------------------------------------------------------------------
    template <typename T, std::size_t N = 1>
    class Dual {
        static_assert(std::is_floating_point_v<T>,
                      "Dual<T,N>: T must be a floating-point type (CPU-only)");

    public:
        using value_type = T;
        static constexpr std::size_t dim = N;

        T v{}; // value
        std::array<T, N> d{}; // derivative tuple (∂/∂x₀, ..., ∂/∂x_{N-1})

        constexpr Dual() noexcept = default;
        constexpr explicit Dual(T val) noexcept : v(val) { d.fill(T{}); }
        constexpr Dual(T val, std::array<T, N> deriv) noexcept : v(val), d(deriv) {}

        // Seed for variable x_i: value = x, d[i] = 1
        static constexpr Dual variable(T val, std::size_t i = 0) noexcept {
            Dual r(val);
            r.d[i] = T{1};
            return r;
        }

        // Arithmetic
        [[nodiscard]] constexpr Dual operator+(const Dual& o) const noexcept {
            Dual r;
            r.v = v + o.v;
            for (std::size_t i = 0; i < N; ++i) r.d[i] = d[i] + o.d[i];
            return r;
        }

        [[nodiscard]] constexpr Dual operator-(const Dual& o) const noexcept {
            Dual r;
            r.v = v - o.v;
            for (std::size_t i = 0; i < N; ++i) r.d[i] = d[i] - o.d[i];
            return r;
        }

        [[nodiscard]] constexpr Dual operator-() const noexcept {
            Dual r;
            r.v = -v;
            for (std::size_t i = 0; i < N; ++i) r.d[i] = -d[i];
            return r;
        }

        [[nodiscard]] constexpr Dual operator*(const Dual& o) const noexcept {
            Dual r;
            r.v = v * o.v;
            for (std::size_t i = 0; i < N; ++i) r.d[i] = d[i] * o.v + v * o.d[i];
            return r;
        }

        [[nodiscard]] constexpr Dual operator/(const Dual& o) const noexcept {
            Dual r;
            r.v = v / o.v;
            T inv = T{1} / (o.v * o.v);
            for (std::size_t i = 0; i < N; ++i) r.d[i] = (d[i] * o.v - v * o.d[i]) * inv;
            return r;
        }

        // Scalar arithmetic
        template <typename S> requires std::is_arithmetic_v<S>
        [[nodiscard]] constexpr Dual operator+(S s) const noexcept { return *this + Dual(static_cast<T>(s)); }

        template <typename S> requires std::is_arithmetic_v<S>
        [[nodiscard]] constexpr Dual operator-(S s) const noexcept { return *this - Dual(static_cast<T>(s)); }

        template <typename S> requires std::is_arithmetic_v<S>
        [[nodiscard]] constexpr Dual operator*(S s) const noexcept {
            Dual r;
            r.v = v * static_cast<T>(s);
            for (std::size_t i = 0; i < N; ++i) r.d[i] = d[i] * static_cast<T>(s);
            return r;
        }

        template <typename S> requires std::is_arithmetic_v<S>
        [[nodiscard]] constexpr Dual operator/(S s) const noexcept {
            Dual r;
            r.v = v / static_cast<T>(s);
            for (std::size_t i = 0; i < N; ++i) r.d[i] = d[i] / static_cast<T>(s);
            return r;
        }

        template <typename S> requires std::is_arithmetic_v<S>
        [[nodiscard]] friend constexpr Dual operator+(S s, const Dual& x) noexcept { return x + s; }

        template <typename S> requires std::is_arithmetic_v<S>
        [[nodiscard]] friend constexpr Dual operator-(S s, const Dual& x) noexcept {
            return Dual(static_cast<T>(s)) - x;
        }

        template <typename S> requires std::is_arithmetic_v<S>
        [[nodiscard]] friend constexpr Dual operator*(S s, const Dual& x) noexcept { return x * s; }

        template <typename S> requires std::is_arithmetic_v<S>
        [[nodiscard]] friend constexpr Dual operator/(S s, const Dual& x) noexcept {
            return Dual(static_cast<T>(s)) / x;
        }

        // Compound assignment
        constexpr Dual& operator+=(const Dual& o) noexcept {
            *this = *this + o;
            return *this;
        }

        constexpr Dual& operator-=(const Dual& o) noexcept {
            *this = *this - o;
            return *this;
        }

        constexpr Dual& operator*=(const Dual& o) noexcept {
            *this = *this * o;
            return *this;
        }

        constexpr Dual& operator/=(const Dual& o) noexcept {
            *this = *this / o;
            return *this;
        }

        // Comparison (by value only — needed for std algorithms)
        [[nodiscard]] constexpr bool operator<(const Dual& o) const noexcept { return v < o.v; }
        [[nodiscard]] constexpr bool operator>(const Dual& o) const noexcept { return v > o.v; }
        [[nodiscard]] constexpr bool operator<=(const Dual& o) const noexcept { return v <= o.v; }
        [[nodiscard]] constexpr bool operator>=(const Dual& o) const noexcept { return v >= o.v; }
        [[nodiscard]] constexpr bool operator==(const Dual& o) const noexcept { return v == o.v; }
    };

    // -----------------------------------------------------------------------
    // Math functions on Dual (chain rule for each)
    // -----------------------------------------------------------------------
    template <typename T, std::size_t N>
    [[nodiscard]] Dual<T, N> sqrt(const Dual<T, N>& x) noexcept {
        T sv = std::sqrt(x.v);
        Dual<T, N> r;
        r.v = sv;
        T inv2 = (sv > T{0}) ? T{0.5} / sv : T{0};
        for (std::size_t i = 0; i < N; ++i) r.d[i] = inv2 * x.d[i];
        return r;
    }

    template <typename T, std::size_t N>
    [[nodiscard]] Dual<T, N> exp(const Dual<T, N>& x) noexcept {
        T ev = std::exp(x.v);
        Dual<T, N> r;
        r.v = ev;
        for (std::size_t i = 0; i < N; ++i) r.d[i] = ev * x.d[i];
        return r;
    }

    template <typename T, std::size_t N>
    [[nodiscard]] Dual<T, N> log(const Dual<T, N>& x) noexcept {
        Dual<T, N> r;
        r.v = std::log(x.v);
        T inv = (x.v > T{0}) ? T{1} / x.v : T{0};
        for (std::size_t i = 0; i < N; ++i) r.d[i] = inv * x.d[i];
        return r;
    }

    template <typename T, std::size_t N>
    [[nodiscard]] Dual<T, N> sin(const Dual<T, N>& x) noexcept {
        T c = std::cos(x.v);
        Dual<T, N> r;
        r.v = std::sin(x.v);
        for (std::size_t i = 0; i < N; ++i) r.d[i] = c * x.d[i];
        return r;
    }

    template <typename T, std::size_t N>
    [[nodiscard]] Dual<T, N> cos(const Dual<T, N>& x) noexcept {
        T s = -std::sin(x.v);
        Dual<T, N> r;
        r.v = std::cos(x.v);
        for (std::size_t i = 0; i < N; ++i) r.d[i] = s * x.d[i];
        return r;
    }

    template <typename T, std::size_t N>
    [[nodiscard]] Dual<T, N> tan(const Dual<T, N>& x) noexcept {
        T c = std::cos(x.v);
        T sec2 = (c != T{0}) ? T{1} / (c * c) : T{0};
        Dual<T, N> r;
        r.v = std::tan(x.v);
        for (std::size_t i = 0; i < N; ++i) r.d[i] = sec2 * x.d[i];
        return r;
    }

    template <typename T, std::size_t N>
    [[nodiscard]] Dual<T, N> abs(const Dual<T, N>& x) noexcept {
        T sign = (x.v >= T{0}) ? T{1} : T{-1};
        Dual<T, N> r;
        r.v = std::abs(x.v);
        for (std::size_t i = 0; i < N; ++i) r.d[i] = sign * x.d[i];
        return r;
    }

    template <typename T, std::size_t N>
    [[nodiscard]] Dual<T, N> pow(const Dual<T, N>& x, T p) noexcept {
        T pv = std::pow(x.v, p);
        T dpv = (x.v != T{0}) ? p * std::pow(x.v, p - T{1}) : T{0};
        Dual<T, N> r;
        r.v = pv;
        for (std::size_t i = 0; i < N; ++i) r.d[i] = dpv * x.d[i];
        return r;
    }

    // -----------------------------------------------------------------------
    // grad — compute ∇f(x) via Dual<T,1> (scalar gradient, N=1)
    // f: callable (Dual<T,1>) -> Dual<T,1>
    // Returns std::array<T,1> = {df/dx}
    // -----------------------------------------------------------------------
    template <typename T, typename F>
    [[nodiscard]] T grad(F&& f, T x) {
        auto d = Dual<T, 1>::variable(x, 0);
        auto res = f(d);
        return res.d[0];
    }

    // grad for vector x (returns Jacobian column j via Dual<T,1> seeding)
    template <typename T, std::size_t N, typename F>
    [[nodiscard]] std::array<T, N> grad_vec(F&& f,
                                            const std::array<T, N>& x) {
        std::array<T, N> g{};
        for (std::size_t i = 0; i < N; ++i) {
            // Seed only component i; all others have zero derivative
            std::array<Dual<T, 1>, N> dx;
            for (std::size_t j = 0; j < N; ++j) {
                dx[j].v = x[j];
                dx[j].d[0] = (j == i) ? T{1} : T{0};
            }
            auto result = f(dx);
            g[i] = result.d[0];
        }
        return g;
    }

    // -----------------------------------------------------------------------
    // hessian_vec — Hessian-vector product ∇²f(x)·v  (matrix-free)
    // -----------------------------------------------------------------------
    // Second-order curvature information WITHOUT forming the full Hessian and
    // WITHOUT nested Dual<Dual<...>> (which does not compile — see the T-is-
    // floating-point static_assert above). Uses the standard forward-over-
    // central-difference identity on the (exact, forward-mode) gradient:
    //
    //     ∇²f(x)·v ≈ ( ∇f(x + ε·v) − ∇f(x − ε·v) ) / (2ε)
    //
    // The gradients ∇f are computed exactly by grad_vec (forward Dual), so the
    // only error is the O(ε²) difference in the outer directional step. Default
    // ε = ∛(machine-eps) balances truncation vs round-off for the central rule.
    //
    // f: callable (const std::array<Dual<T,1>,N>&) -> Dual<T,1>   (same as grad_vec)
    // Returns std::array<T,N> = ∇²f(x)·v.
    template <typename T, std::size_t N, typename F>
    [[nodiscard]] std::array<T, N> hessian_vec(F&& f,
                                               const std::array<T, N>& x,
                                               const std::array<T, N>& v,
                                               T eps = T{0}) {
        if (eps <= T{0})
            eps = std::cbrt(std::numeric_limits<T>::epsilon());
        std::array<T, N> xp{}, xm{};
        for (std::size_t i = 0; i < N; ++i) {
            xp[i] = x[i] + eps * v[i];
            xm[i] = x[i] - eps * v[i];
        }
        auto gp = grad_vec<T, N>(f, xp);
        auto gm = grad_vec<T, N>(f, xm);
        std::array<T, N> hv{};
        const T inv2e = T{1} / (T{2} * eps);
        for (std::size_t i = 0; i < N; ++i)
            hv[i] = (gp[i] - gm[i]) * inv2e;
        return hv;
    }

    // -----------------------------------------------------------------------
    // hessian — dense Hessian ∇²f(x) as N columns of hessian_vec against the
    // canonical basis. O(N) gradient evaluations (each O(N)); use only for
    // small N. Returned as std::array<std::array<T,N>,N>, row-major (H[i][j] =
    // ∂²f/∂x_i∂x_j). Symmetrized to counter difference noise.
    // -----------------------------------------------------------------------
    template <typename T, std::size_t N, typename F>
    [[nodiscard]] std::array<std::array<T, N>, N> hessian(F&& f,
                                                          const std::array<T, N>& x,
                                                          T eps = T{0}) {
        std::array<std::array<T, N>, N> H{};
        for (std::size_t j = 0; j < N; ++j) {
            std::array<T, N> e{};
            e[j] = T{1};
            auto col = hessian_vec<T, N>(f, x, e, eps);
            for (std::size_t i = 0; i < N; ++i) H[i][j] = col[i];
        }
        // symmetrize
        for (std::size_t i = 0; i < N; ++i)
            for (std::size_t j = i + 1; j < N; ++j) {
                T avg = (H[i][j] + H[j][i]) * T{0.5};
                H[i][j] = H[j][i] = avg;
            }
        return H;
    }

    // -----------------------------------------------------------------------
    // Type alias for common use
    // -----------------------------------------------------------------------
    template <typename T>
    using DualScalar = Dual<T, 1>;
} // namespace ga

#endif // PEBBLE_CONTAINERS_MATRIX_DUAL_HPP
