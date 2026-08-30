#pragma once
// ============================================================================
// kalpa/algo/constrained.hpp — constrained optimization
// ============================================================================
// Two shapes of algorithm live here:
//
//  (1) Domain policies + descent-loop algorithms that reuse the Solver.
//      A Domain models kalpa::Domain (project(x,out)). The Solver already
//      projects each iterate onto prob.domain, so a ProjectedGradient "just"
//      = GradientDescent + a non-trivial Domain. We provide the n-D domains
//      (Box, Halfspace-polytope, Ball) natively — akruti is strictly 2D and
//      has no project(), so n-D projection is kalpa-owned. FrankWolfe is a
//      descent Algorithm needing a linear-minimization oracle over the domain.
//
//  (2) Standalone solvers for problems the descent loop does not fit:
//      linear programs (revised simplex over ga::lu; plus an exact rational
//      simplex on native rational arrays — ga:: matrices cannot hold an exact
//      rational, so this path does its own pivoting), and equality-constrained
//      QP / KKT systems (delegated to ga::schur_solve).
//
// Heavy linear algebra delegates to ga:: (solve / lu / lu_solve / schur_solve).
// ============================================================================

#ifndef PEBBLE_KALPA_ALGO_CONSTRAINED_HPP
#define PEBBLE_KALPA_ALGO_CONSTRAINED_HPP

#include <kalpa/core/solver.hpp>
#include <containers/matrix/dense.hpp>
#include <containers/matrix/solve.hpp>
#include <containers/matrix/factorize.hpp>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <expected>
#include <limits>
#include <vector>

namespace kalpa {

    // =======================================================================
    // Domains (n-D, native). Each models kalpa::Domain: project(x, out).
    // =======================================================================

    // Box  ℓ ≤ x ≤ u  (per-coordinate clamp; ±inf bounds allowed).
    template<typename T>
    struct Box {
        ga::Vector<T> lo, hi;
        Box() = default;
        Box(ga::Vector<T> l, ga::Vector<T> u) : lo(std::move(l)), hi(std::move(u)) {}
        void project(const ga::Vector<T>& x, ga::Vector<T>& out) const {
            for (std::size_t i = 0; i < x.size(); ++i) {
                T xi = x[i];
                if (i < lo.size() && xi < lo[i]) xi = lo[i];
                if (i < hi.size() && xi > hi[i]) xi = hi[i];
                out[i] = xi;
            }
        }
    };

    // Euclidean ball  ‖x − c‖ ≤ r  (radial clamp).
    template<typename T>
    struct Ball {
        ga::Vector<T> center;
        T radius{T{1}};
        void project(const ga::Vector<T>& x, ga::Vector<T>& out) const {
            const std::size_t n = x.size();
            T d2{};
            for (std::size_t i = 0; i < n; ++i) {
                const T c = (i < center.size()) ? center[i] : T{0};
                out[i] = x[i]; d2 += (x[i]-c)*(x[i]-c);
            }
            const T d = std::sqrt(d2);
            if (d <= radius || d == T{0}) return;
            const T s = radius / d;
            for (std::size_t i = 0; i < n; ++i) {
                const T c = (i < center.size()) ? center[i] : T{0};
                out[i] = c + s * (x[i] - c);
            }
        }
    };

    // Polytope  {x : aᵢᵀx ≤ bᵢ}  — projection by cyclic Dykstra/alternating
    // half-space projections (Hildreth-style). Converges for consistent sets.
    template<typename T>
    struct Polytope {
        ga::Matrix<T> A;         // m×n rows are the constraint normals aᵢᵀ
        ga::Vector<T> b;         // m
        std::size_t sweeps{100};
        T tol{static_cast<T>(1e-10)};

        void project(const ga::Vector<T>& x, ga::Vector<T>& out) const {
            const std::size_t m = A.rows(), n = A.cols();
            for (std::size_t i = 0; i < n; ++i) out[i] = x[i];
            std::vector<T> a2(m, T{0});
            for (std::size_t r = 0; r < m; ++r) {
                T s{}; for (std::size_t j = 0; j < n; ++j) s += A(r,j)*A(r,j);
                a2[r] = s;
            }
            for (std::size_t sweep = 0; sweep < sweeps; ++sweep) {
                T max_viol{};
                for (std::size_t r = 0; r < m; ++r) {
                    if (a2[r] == T{0}) continue;
                    T ax{}; for (std::size_t j = 0; j < n; ++j) ax += A(r,j)*out[j];
                    const T viol = ax - b[r];
                    if (viol > T{0}) {
                        const T t = viol / a2[r];
                        for (std::size_t j = 0; j < n; ++j) out[j] -= t * A(r,j);
                        max_viol = std::max(max_viol, viol);
                    }
                }
                if (max_viol < tol) break;
            }
        }
    };

    // =======================================================================
    // ProjectedGradient — descent Algorithm. d = −g; the Solver's domain
    // projection does the feasibility restoration after the step.
    //   Solver<ProjectedGradient<T>, Deriv, Armijo<T>, ...> with prob.domain
    //   set to a Box / Ball / Polytope.
    // (Identical direction to GradientDescent; named for intent + to pair with
    //  a projection-aware Armijo that evaluates f at the projected trial.)
    // =======================================================================
    template<typename T>
    struct ProjectedGradient {
        void reset(std::size_t) {}
        template<typename D, typename F>
        void direction(const D&, const F&, const IterState<T>& s, ga::Vector<T>& out) {
            for (std::size_t i = 0; i < s.g.size(); ++i) out[i] = -s.g[i];
        }
        void update(const ga::Vector<T>&, const ga::Vector<T>&,
                    const ga::Vector<T>&, const ga::Vector<T>&) {}
    };

    // =======================================================================
    // FrankWolfe (conditional gradient) — standalone driver over a compact
    // convex domain given a linear-minimization oracle  s = argmin_{s∈C} gᵀs.
    // Step  x ← x + γ(s − x),  γ = 2/(k+2). No projection needed.
    //   lmo(const Vector& g, Vector& s_out) → vertex minimizing gᵀs.
    // =======================================================================
    template<typename T>
    struct FrankWolfe {
        std::size_t max_iter{1000};
        T tol{static_cast<T>(1e-6)};

        template<typename Prob, typename Deriv, typename LMO>
        [[nodiscard]] std::expected<Result<T>, Diagnosis>
        solve(const Prob& prob, const ga::Vector<T>& x0, const Deriv& deriv, LMO&& lmo) const {
            const auto& f = prob.objective;
            const std::size_t n = x0.size();
            ga::Vector<T> x = x0, g(n), s(n);
            Result<T> r; r.status = Status::MaxIterations;
            for (std::size_t k = 0; k < max_iter; ++k) {
                deriv.grad(f, x, g);
                lmo(g, s);                                   // linear oracle
                // Frank-Wolfe gap  gᵀ(x − s)
                T gap{}; for (std::size_t i = 0; i < n; ++i) gap += g[i]*(x[i]-s[i]);
                if (gap <= tol) { r.status = Status::Converged; r.iterations = k; break; }
                const T gamma = T{2} / static_cast<T>(k + 2);
                for (std::size_t i = 0; i < n; ++i) x[i] += gamma * (s[i] - x[i]);
                r.iterations = k + 1;
            }
            r.x = x; r.f = f(x);
            deriv.grad(f, x, g);
            r.grad_norm = detail::nrm2(g);
            return r;
        }
    };

    // =======================================================================
    // Equality-constrained QP  /  KKT solve  — delegates to ga::schur_solve.
    //   min ½ xᵀ H x + cᵀx   s.t.  A x = b
    // KKT:  [ H  Aᵀ ] [ x ]   [ −c ]
    //       [ A  0  ] [ λ ] = [  b ]
    // schur_solve wants the (2,2) block invertible; we place H in the (2,2)
    // slot by solving the reordered system  [0 A; Aᵀ H][λ;x]=[b;−c] so D=H.
    // =======================================================================
    template<typename T>
    struct EqualityQP {
        [[nodiscard]] std::expected<std::pair<ga::Vector<T>, ga::Vector<T>>, Diagnosis>
        solve(const ga::Matrix<T>& H, const ga::Vector<T>& c,
              const ga::Matrix<T>& A, const ga::Vector<T>& b) const {
            const std::size_t n = H.rows(), m = A.rows();
            // Blocks of  [ Z  A ] [ λ ]   [ b  ]     (Z = m×m zero)
            //            [ Aᵀ H ] [ x ] = [ −c ]
            ga::Matrix<T> Z(m, m, T{0});
            ga::Matrix<T> At(n, m);
            for (std::size_t i = 0; i < n; ++i)
                for (std::size_t j = 0; j < m; ++j) At(i,j) = A(j,i);
            ga::Vector<T> negc(n);
            for (std::size_t i = 0; i < n; ++i) negc[i] = -c[i];
            auto sol = ga::schur_solve(Z, A, At, H, b, negc, ga::MatrixKind::SymIndefinite);
            // sol.x = λ (top block), sol.y = x (bottom block)
            for (std::size_t i = 0; i < n; ++i)
                if (!std::isfinite(sol.y[i]))
                    return std::unexpected(Diagnosis{Cause::SingularKKT, "KKT system singular", 0});
            return std::pair{sol.y, sol.x};   // {x, λ}
        }
    };

    // =======================================================================
    // Revised simplex (dense, primal, Bland anti-cycling) over ga::lu.
    // Solves  min cᵀx  s.t.  A x = b,  x ≥ 0   (standard form; caller adds
    // slacks). Returns the optimal vertex or a Diagnosis (Infeasible/Unbounded).
    // =======================================================================
    template<typename T>
    struct SimplexLP {
        std::size_t max_iter{10000};
        T tol{static_cast<T>(1e-9)};

        [[nodiscard]] std::expected<Result<T>, Diagnosis>
        solve(const ga::Matrix<T>& A, const ga::Vector<T>& b,
              const ga::Vector<T>& c) const {
            const std::size_t m = A.rows(), nvar = A.cols();
            if (nvar < m)
                return std::unexpected(Diagnosis{Cause::Infeasible, "simplex: n < m", 0});

            // Big-M phase-1/2 in one: append m artificials with cost M.
            const T M = big_m(c);
            const std::size_t n = nvar + m;
            ga::Matrix<T> Aa(m, n, T{0});
            for (std::size_t i = 0; i < m; ++i) {
                for (std::size_t j = 0; j < nvar; ++j) Aa(i,j) = A(i,j);
                Aa(i, nvar + i) = (b[i] >= T{0}) ? T{1} : T{-1};   // sign so basis ≥ 0
            }
            ga::Vector<T> cc(n);
            for (std::size_t j = 0; j < nvar; ++j) cc[j] = c[j];
            for (std::size_t j = nvar; j < n; ++j) cc[j] = M;

            std::vector<std::size_t> basis(m);
            for (std::size_t i = 0; i < m; ++i) basis[i] = nvar + i;

            for (std::size_t iter = 0; iter < max_iter; ++iter) {
                // B = basis columns
                ga::Matrix<T> B(m, m);
                for (std::size_t i = 0; i < m; ++i)
                    for (std::size_t k = 0; k < m; ++k) B(i,k) = Aa(i, basis[k]);
                auto lu = ga::lu(B);
                if (!lu.info.ok)
                    return std::unexpected(Diagnosis{Cause::SingularKKT, "simplex: singular basis", iter});

                // xB = B⁻¹ b   (fix signs so RHS ≥ 0 handled by artificial signs)
                ga::Vector<T> rhs(m);
                for (std::size_t i = 0; i < m; ++i) rhs[i] = b[i];
                auto xB = ga::lu_solve(lu, rhs);

                // simplex multipliers  yᵀ = cBᵀ B⁻¹  →  solve Bᵀ y = cB
                ga::Matrix<T> Bt(m, m);
                for (std::size_t i = 0; i < m; ++i)
                    for (std::size_t k = 0; k < m; ++k) Bt(i,k) = B(k,i);
                ga::Vector<T> cB(m);
                for (std::size_t i = 0; i < m; ++i) cB[i] = cc[basis[i]];
                auto lut = ga::lu(Bt);
                if (!lut.info.ok)
                    return std::unexpected(Diagnosis{Cause::SingularKKT, "simplex: singular Bᵀ", iter});
                auto y = ga::lu_solve(lut, cB);

                // reduced costs  c̄ⱼ = cⱼ − yᵀ Aⱼ ; Bland: first index with c̄ < 0
                std::size_t enter = n;
                for (std::size_t j = 0; j < n; ++j) {
                    if (in_basis(basis, j)) continue;
                    T yA{}; for (std::size_t i = 0; i < m; ++i) yA += y[i]*Aa(i,j);
                    if (cc[j] - yA < -tol) { enter = j; break; }   // Bland's rule
                }
                if (enter == n) {                                  // optimal
                    return assemble(basis, xB, cc, nvar, m);
                }

                // direction  d = B⁻¹ A_enter ; ratio test (Bland tie-break: min index)
                ga::Vector<T> Aj(m);
                for (std::size_t i = 0; i < m; ++i) Aj[i] = Aa(i, enter);
                auto d = ga::lu_solve(lu, Aj);
                std::size_t leave = m; T best = std::numeric_limits<T>::infinity();
                for (std::size_t i = 0; i < m; ++i) {
                    if (d[i] > tol) {
                        const T ratio = xB[i] / d[i];
                        if (ratio < best - tol ||
                            (std::abs(ratio - best) <= tol && (leave==m || basis[i] < basis[leave]))) {
                            best = ratio; leave = i;
                        }
                    }
                }
                if (leave == m)
                    return std::unexpected(Diagnosis{Cause::Unbounded, "simplex: unbounded objective", iter});
                basis[leave] = enter;
            }
            return std::unexpected(Diagnosis{Cause::NumericalError, "simplex: iteration limit", max_iter});
        }

    private:
        static bool in_basis(const std::vector<std::size_t>& basis, std::size_t j) {
            return std::find(basis.begin(), basis.end(), j) != basis.end();
        }
        static T big_m(const ga::Vector<T>& c) {
            T mx{}; for (std::size_t i = 0; i < c.size(); ++i) mx = std::max(mx, std::abs(c[i]));
            return (mx > T{0}) ? static_cast<T>(1e6) * mx : static_cast<T>(1e6);
        }
        [[nodiscard]] std::expected<Result<T>, Diagnosis>
        assemble(const std::vector<std::size_t>& basis, const ga::Vector<T>& xB,
                 const ga::Vector<T>& cc, std::size_t nvar, std::size_t m) const {
            // artificials still basic at positive level → infeasible
            for (std::size_t i = 0; i < m; ++i)
                if (basis[i] >= nvar && xB[i] > static_cast<T>(1e-6))
                    return std::unexpected(Diagnosis{Cause::Infeasible, "simplex: no feasible point", 0});
            Result<T> r; r.x = ga::Vector<T>(nvar, T{0});
            for (std::size_t i = 0; i < m; ++i)
                if (basis[i] < nvar) r.x[basis[i]] = xB[i];
            T obj{}; for (std::size_t j = 0; j < nvar; ++j) obj += cc[j]*r.x[j];
            r.f = obj; r.status = Status::Converged;
            return r;
        }
    };

    // =======================================================================
    // Exact rational simplex — native, on rational arrays (NOT ga:: matrices:
    // ga:: requires numeric_limits/sqrt on T, which exact_rational lacks).
    // Fraction is a self-contained int rational; the tableau is std::vector.
    // Bland's rule guarantees termination with zero round-off.
    // =======================================================================
    struct Fraction {
        long long num{0}, den{1};
        Fraction() = default;
        Fraction(long long n) : num(n), den(1) {}
        Fraction(long long n, long long d) : num(n), den(d) { normalize(); }
        void normalize() {
            if (den < 0) { num = -num; den = -den; }
            long long g = gcd(num < 0 ? -num : num, den);
            if (g > 1) { num /= g; den /= g; }
            if (num == 0) den = 1;
        }
        static long long gcd(long long a, long long b) { while (b){ auto t=a%b; a=b; b=t; } return a?a:1; }
        Fraction operator+(const Fraction& o) const { return {num*o.den + o.num*den, den*o.den}; }
        Fraction operator-(const Fraction& o) const { return {num*o.den - o.num*den, den*o.den}; }
        Fraction operator*(const Fraction& o) const { return {num*o.num, den*o.den}; }
        Fraction operator/(const Fraction& o) const { return {num*o.den, den*o.num}; }
        bool operator<(const Fraction& o) const { return num*o.den < o.num*den; }
        bool operator>(const Fraction& o) const { return num*o.den > o.num*den; }
        bool operator==(const Fraction& o) const { return num*o.den == o.num*den; }
        bool positive() const { return num > 0; }
        bool negative() const { return num < 0; }
        [[nodiscard]] double to_double() const { return static_cast<double>(num)/static_cast<double>(den); }
    };

    struct ExactSimplexResult {
        std::vector<Fraction> x;
        Fraction              objective;
        Status                status{Status::MaxIterations};
        Cause                 cause{Cause::NumericalError};   // valid only if !ok
        bool                  ok{false};
    };

    struct ExactSimplexLP {
        std::size_t max_iter{100000};

        // min cᵀx s.t. A x = b, x ≥ 0 with rational data.
        [[nodiscard]] ExactSimplexResult
        solve(const std::vector<std::vector<Fraction>>& A,
              const std::vector<Fraction>& b,
              const std::vector<Fraction>& c) const {
            const std::size_t m = A.size();
            const std::size_t nvar = m ? A[0].size() : 0;
            const std::size_t n = nvar + m;                  // + artificials
            // Big-M with a symbolic-free large rational.
            const Fraction M = big_m(c);

            // tableau rows: m constraint rows over columns [0..n) plus RHS.
            std::vector<std::vector<Fraction>> Tb(m, std::vector<Fraction>(n + 1));
            for (std::size_t i = 0; i < m; ++i) {
                for (std::size_t j = 0; j < nvar; ++j) Tb[i][j] = A[i][j];
                const bool bneg = b[i].negative();
                Tb[i][nvar + i] = bneg ? Fraction(-1) : Fraction(1);
                Tb[i][n] = b[i];
            }
            std::vector<Fraction> cc(n);
            for (std::size_t j = 0; j < nvar; ++j) cc[j] = c[j];
            for (std::size_t j = nvar; j < n; ++j) cc[j] = M;
            std::vector<std::size_t> basis(m);
            for (std::size_t i = 0; i < m; ++i) basis[i] = nvar + i;

            // make RHS ≥ 0 (scale rows whose artificial got −1)
            for (std::size_t i = 0; i < m; ++i)
                if (Tb[i][nvar + i] == Fraction(-1))
                    for (std::size_t j = 0; j <= n; ++j) Tb[i][j] = Fraction(0) - Tb[i][j];

            for (std::size_t iter = 0; iter < max_iter; ++iter) {
                // reduced costs c̄ⱼ = cⱼ − Σ cB · (tableau col j)
                std::size_t enter = n;
                for (std::size_t j = 0; j < n; ++j) {
                    if (is_basic(basis, j)) continue;
                    Fraction red = cc[j];
                    for (std::size_t i = 0; i < m; ++i) red = red - cc[basis[i]] * Tb[i][j];
                    if (red.negative()) { enter = j; break; }         // Bland
                }
                if (enter == n) return finish(Tb, basis, cc, nvar, m, n);

                std::size_t leave = m; Fraction best;
                bool have = false;
                for (std::size_t i = 0; i < m; ++i) {
                    if (Tb[i][enter].positive()) {
                        Fraction ratio = Tb[i][n] / Tb[i][enter];
                        if (!have || ratio < best ||
                            (ratio == best && basis[i] < basis[leave])) {
                            best = ratio; leave = i; have = true;
                        }
                    }
                }
                if (!have) { ExactSimplexResult r; r.cause = Cause::Unbounded; return r; }
                pivot(Tb, leave, enter, n);
                basis[leave] = enter;
            }
            ExactSimplexResult r; r.cause = Cause::NumericalError; return r;
        }

    private:
        static bool is_basic(const std::vector<std::size_t>& basis, std::size_t j) {
            return std::find(basis.begin(), basis.end(), j) != basis.end();
        }
        static Fraction big_m(const std::vector<Fraction>& c) {
            long long mx = 1;
            for (const auto& f : c) { long long a = f.num < 0 ? -f.num : f.num; if (a > mx) mx = a; }
            return Fraction(mx * 1000000LL);
        }
        static void pivot(std::vector<std::vector<Fraction>>& Tb,
                          std::size_t pr, std::size_t pc, std::size_t n) {
            const Fraction piv = Tb[pr][pc];
            for (std::size_t j = 0; j <= n; ++j) Tb[pr][j] = Tb[pr][j] / piv;
            for (std::size_t i = 0; i < Tb.size(); ++i) {
                if (i == pr) continue;
                const Fraction f = Tb[i][pc];
                if (f == Fraction(0)) continue;
                for (std::size_t j = 0; j <= n; ++j) Tb[i][j] = Tb[i][j] - f * Tb[pr][j];
            }
        }
        static ExactSimplexResult finish(const std::vector<std::vector<Fraction>>& Tb,
                                         const std::vector<std::size_t>& basis,
                                         const std::vector<Fraction>& cc,
                                         std::size_t nvar, std::size_t m, std::size_t n) {
            ExactSimplexResult r;
            for (std::size_t i = 0; i < m; ++i)
                if (basis[i] >= nvar && Tb[i][n].positive()) { r.cause = Cause::Infeasible; return r; }
            r.x.assign(nvar, Fraction(0));
            for (std::size_t i = 0; i < m; ++i)
                if (basis[i] < nvar) r.x[basis[i]] = Tb[i][n];
            Fraction obj(0);
            for (std::size_t j = 0; j < nvar; ++j) obj = obj + cc[j] * r.x[j];
            r.objective = obj; r.status = Status::Converged; r.ok = true;
            return r;
        }
    };

} // namespace kalpa

#endif // PEBBLE_KALPA_ALGO_CONSTRAINED_HPP
