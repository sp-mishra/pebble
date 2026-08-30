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
#include <optional>
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

    // =======================================================================
    // Interior-point (Mehrotra predictor–corrector) for the convex QP/LP
    //   min ½ xᵀH x + cᵀx   s.t.  A x = b,  x ≥ 0.
    // Primal–dual variables (x, y, z) with x,z ≥ 0. At each iterate we solve
    // the reduced KKT system
    //     [ −(H + X⁻¹Z)   Aᵀ ] [ Δx ]   [ r_x ]
    //     [      A         0  ] [ Δy ] = [ r_y ]
    // twice (affine predictor, then centering+corrector). The (1,1) block is
    // negated so it is symmetric-negative-definite and the whole matrix is the
    // usual symmetric-indefinite KKT form ga::schur_solve handles (block-reorder
    // as in EqualityQP: place the invertible block in the D slot). The reduced
    // rhs is r_x = r_d + X⁻¹ r_c with r_y = −r_p, and Δz is recovered from the
    // complementarity row Z Δx + X Δz = −r_c, i.e. Δz = X⁻¹(−r_c − Z Δx). This
    // keeps the eliminated system algebraically identical to the full 3×3
    // Newton KKT (stationarity r_d, primal r_p, complementarity r_c = XZe − σμe)
    // — a mixed sign here silently doubles the r_c term and stalls the iterate.
    // LP is the H = 0 special case (block diagonal).
    // Reference: Mehrotra, "On the implementation of a primal-dual IPM" (1992).
    // =======================================================================
    template<typename T>
    struct InteriorPoint {
        std::size_t max_iter{100};
        T tol{static_cast<T>(1e-8)};
        T tau{static_cast<T>(0.995)};   // fraction-to-boundary
        bool crossover{true};           // LP: purify analytic centre → vertex

        [[nodiscard]] std::expected<Result<T>, Diagnosis>
        solve(const ga::Matrix<T>& H, const ga::Vector<T>& c,
              const ga::Matrix<T>& A, const ga::Vector<T>& b) const {
            const std::size_t n = c.size(), m = b.size();
            if (n == 0) return std::unexpected(Diagnosis{Cause::NumericalError, "empty problem", 0});

            // strictly-interior start
            ga::Vector<T> x(n, T{1}), z(n, T{1}), y(m, T{0});

            auto matvec = [](const ga::Matrix<T>& M, const ga::Vector<T>& v) {
                ga::Vector<T> r(M.rows(), T{0});
                for (std::size_t i = 0; i < M.rows(); ++i)
                    for (std::size_t j = 0; j < M.cols(); ++j) r[i] += M(i,j) * v[j];
                return r;
            };
            auto matTvec = [](const ga::Matrix<T>& M, const ga::Vector<T>& v) {
                ga::Vector<T> r(M.cols(), T{0});
                for (std::size_t i = 0; i < M.rows(); ++i)
                    for (std::size_t j = 0; j < M.cols(); ++j) r[j] += M(i,j) * v[i];
                return r;
            };

            // Aᵀ once (n×m) for the schur block reorder.
            ga::Matrix<T> At(n, m);
            for (std::size_t i = 0; i < n; ++i)
                for (std::size_t j = 0; j < m; ++j) At(i,j) = A(j,i);

            Result<T> r; r.status = Status::MaxIterations;

            for (std::size_t it = 0; it < max_iter; ++it) {
                // residuals
                ga::Vector<T> Hx = (H.rows() == n) ? matvec(H, x) : ga::Vector<T>(n, T{0});
                ga::Vector<T> Aty = matTvec(A, y);
                ga::Vector<T> Ax  = matvec(A, x);
                ga::Vector<T> rd(n), rp(m);
                for (std::size_t i = 0; i < n; ++i) rd[i] = Hx[i] + c[i] - Aty[i] - z[i]; // dual
                for (std::size_t i = 0; i < m; ++i) rp[i] = Ax[i] - b[i];                 // primal
                T mu{}; for (std::size_t i = 0; i < n; ++i) mu += x[i]*z[i];
                mu /= static_cast<T>(n);

                if (detail::nrm2(rd) < tol && detail::nrm2(rp) < tol && mu < tol) {
                    r.status = Status::Converged; r.iterations = it;
                    // certificate: dual residual = stationarity, primal residual
                    // = Ax−b, complementarity = xᵀz (=n·μ), duals y.
                    r.kkt_stationarity     = detail::nrm2(rd);
                    r.primal_infeasibility = detail::nrm2(rp);
                    r.complementarity      = mu * static_cast<T>(n);
                    r.multipliers          = y;
                    break;
                }

                // reduced (negated) (1,1) block  M = −(H + X⁻¹Z)
                ga::Matrix<T> M(n, n, T{0});
                if (H.rows() == n) for (std::size_t i=0;i<n;++i) for (std::size_t j=0;j<n;++j) M(i,j) = -H(i,j);
                for (std::size_t i = 0; i < n; ++i) M(i,i) -= z[i] / x[i];
                // Primal regularization: for a pure LP (H = 0) the barrier term
                // X⁻¹Z is the entire diagonal, and as the iterate nears a vertex
                // one of each complementary pair (x_i, z_i) drives z_i/x_i → 0,
                // leaving a ~0 diagonal that trips the LDLT singular guard. A tiny
                // shift keeps M nonsingular without perturbing the optimum beyond
                // tolerance (it vanishes as the step lengths shrink at convergence).
                {
                    const T delta = std::sqrt(std::numeric_limits<T>::epsilon());
                    for (std::size_t i = 0; i < n; ++i) M(i,i) -= delta;
                }

                // solve KKT for a given complementarity target rc = X Z e − σμ e.
                // rhs_x = −rd + X⁻¹ rc ;  rhs_y = −rp.  Reorder like EqualityQP:
                //   [ 0  A ][Δy]   [ rhs_y ]
                //   [ Aᵀ M ][Δx] = [ rhs_x ]  → schur_solve(Z0,A,At,M, rhs_y, rhs_x)
                ga::Matrix<T> Z0(m, m, T{0});
                auto kkt = [&](const ga::Vector<T>& rc,
                               ga::Vector<T>& dx, ga::Vector<T>& dy, ga::Vector<T>& dz)
                    -> bool {
                    ga::Vector<T> rhs_x(n), rhs_y(m);
                    for (std::size_t i = 0; i < n; ++i) rhs_x[i] = rd[i] + rc[i] / x[i];
                    for (std::size_t i = 0; i < m; ++i) rhs_y[i] = -rp[i];
                    auto sol = ga::schur_solve(Z0, A, At, M, rhs_y, rhs_x, ga::MatrixKind::SymIndefinite);
                    for (std::size_t i = 0; i < n; ++i) if (!std::isfinite(sol.y[i])) return false;
                    dx = sol.y; dy = sol.x;
                    // Δz = X⁻¹(−rc − Z Δx)   (complementarity row  Z Δx + X Δz = −rc)
                    for (std::size_t i = 0; i < n; ++i) dz[i] = (-rc[i] - z[i]*dx[i]) / x[i];
                    return true;
                };

                // fraction-to-boundary step length for a (Δx,Δz) pair
                auto step_len = [&](const ga::Vector<T>& dx, const ga::Vector<T>& dz) {
                    T a = T{1};
                    for (std::size_t i = 0; i < n; ++i) {
                        if (dx[i] < T{0}) a = std::min(a, -tau * x[i] / dx[i]);
                        if (dz[i] < T{0}) a = std::min(a, -tau * z[i] / dz[i]);
                    }
                    return a;
                };

                // --- affine (predictor): rc = X Z e ---
                ga::Vector<T> rc_aff(n);
                for (std::size_t i = 0; i < n; ++i) rc_aff[i] = x[i]*z[i];
                ga::Vector<T> dxa(n), dya(m), dza(n);
                if (!kkt(rc_aff, dxa, dya, dza))
                    return std::unexpected(Diagnosis{Cause::SingularKKT, "IPM KKT singular (affine)", it});
                const T a_aff = step_len(dxa, dza);
                T mu_aff{};
                for (std::size_t i = 0; i < n; ++i) mu_aff += (x[i]+a_aff*dxa[i]) * (z[i]+a_aff*dza[i]);
                mu_aff /= static_cast<T>(n);
                const T sigma = (mu > T{0}) ? std::pow(mu_aff / mu, T{3}) : T{0};

                // --- corrector: rc = X Z e + ΔXaff ΔZaff e − σμ e ---
                ga::Vector<T> rc_cor(n);
                for (std::size_t i = 0; i < n; ++i)
                    rc_cor[i] = x[i]*z[i] + dxa[i]*dza[i] - sigma*mu;
                ga::Vector<T> dx(n), dy(m), dz(n);
                if (!kkt(rc_cor, dx, dy, dz))
                    return std::unexpected(Diagnosis{Cause::SingularKKT, "IPM KKT singular (corrector)", it});
                const T alpha = step_len(dx, dz);

                for (std::size_t i = 0; i < n; ++i) { x[i] += alpha*dx[i]; z[i] += alpha*dz[i]; }
                for (std::size_t i = 0; i < m; ++i) y[i] += alpha*dy[i];
                r.iterations = it + 1;
            }

            // --- Crossover / basis identification (LP only) -------------------
            // On a pure LP (H = 0) the optimal face can be a higher-dimensional
            // facet, and a primal–dual IPM converges to its analytic centre, not
            // a vertex (e.g. x+y=4, 0≤x≤3 → centre ≈ (2,2) rather than a corner).
            // A single purification step recovers a basic optimal solution: pick
            // the m largest primal coordinates as a candidate basis, repair rank
            // by swapping in further columns, then solve A_B x_B = b, x_N = 0.
            // Accepted only if the resulting vertex stays feasible (x ≥ 0) and no
            // worse in objective — otherwise the interior point is kept. This is
            // inert for strictly-convex QPs (unique interior optimum), so it runs
            // only when H is (numerically) zero.
            if (crossover && r.status == Status::Converged && m > 0 && m <= n) {
                bool h_zero = true;
                if (H.rows() == n)
                    for (std::size_t i = 0; i < n && h_zero; ++i)
                        for (std::size_t j = 0; j < n && h_zero; ++j)
                            if (std::abs(H(i,j)) > tol) h_zero = false;
                if (h_zero) {
                    if (auto v = purify_to_vertex(A, b, c, x); v) x = *v;
                }
            }

            r.x = x;
            T obj{};
            if (H.rows() == n)
                for (std::size_t i = 0; i < n; ++i) { T hx{}; for (std::size_t j=0;j<n;++j) hx += H(i,j)*x[j]; obj += T{0.5}*x[i]*hx; }
            for (std::size_t i = 0; i < n; ++i) obj += c[i]*x[i];
            r.f = obj;
            r.grad_norm = T{0};
            return r;
        }

    private:
        // Basis identification: from a near-optimal interior point x_ipm, build a
        // vertex on the same optimal face. Columns are ranked by x_ipm value; the
        // m largest form the initial basis, and any rank deficiency is repaired by
        // swapping the least-promising basic column for the next candidate until
        // A_B is nonsingular. Returns the vertex iff it is feasible and its
        // objective does not exceed the interior point's (within tol); else null.
        [[nodiscard]] std::optional<ga::Vector<T>>
        purify_to_vertex(const ga::Matrix<T>& A, const ga::Vector<T>& b,
                         const ga::Vector<T>& c, const ga::Vector<T>& x_ipm) const {
            const std::size_t m = A.rows(), n = A.cols();
            if (m == 0 || n < m) return std::nullopt;

            // candidate column order: primal coordinate descending
            std::vector<std::size_t> order(n);
            for (std::size_t j = 0; j < n; ++j) order[j] = j;
            std::stable_sort(order.begin(), order.end(),
                             [&](std::size_t a, std::size_t b2){ return x_ipm[a] > x_ipm[b2]; });

            std::vector<std::size_t> basis(order.begin(), order.begin() + m);
            std::size_t next = m;                        // next untried candidate
            ga::Vector<T> xB(m);
            bool ok = false;
            for (std::size_t guard = 0; guard <= n && !ok; ++guard) {
                ga::Matrix<T> B(m, m);
                for (std::size_t i = 0; i < m; ++i)
                    for (std::size_t k = 0; k < m; ++k) B(i,k) = A(i, basis[k]);
                auto lu = ga::lu(B);
                if (lu.info.ok) {
                    ga::Vector<T> rhs(m);
                    for (std::size_t i = 0; i < m; ++i) rhs[i] = b[i];
                    xB = ga::lu_solve(lu, rhs);
                    ok = true;
                    break;
                }
                if (next >= n) return std::nullopt;      // exhausted candidates
                basis.back() = order[next++];            // swap weakest col, retry
            }
            if (!ok) return std::nullopt;

            ga::Vector<T> xv(n, T{0});
            for (std::size_t i = 0; i < m; ++i) {
                if (xB[i] < -static_cast<T>(1e-7)) return std::nullopt;   // infeasible vertex
                xv[basis[i]] = xB[i];
            }
            // objective must not worsen (optimal face ⇒ equal within tol)
            T f_ipm{}, f_v{};
            for (std::size_t j = 0; j < n; ++j) { f_ipm += c[j]*x_ipm[j]; f_v += c[j]*xv[j]; }
            if (f_v > f_ipm + static_cast<T>(1e-6)) return std::nullopt;
            return xv;
        }
    };

    // =======================================================================
    // SQP — sequential quadratic programming for the equality-constrained NLP
    //   min f(x)  s.t.  cᵢ(x) = 0,  i = 1..m.
    // Each iterate solves the equality-constrained QP subproblem
    //   min ½ pᵀW p + ∇fᵀp   s.t.  J p + c = 0
    // via the existing EqualityQP::solve(W, ∇f, J, −c) → (p, λ). W is the
    // objective Hessian assembled matrix-free (deriv.hessian_vec, as Newton);
    // J is the constraint Jacobian built row-by-row with deriv.grad on each
    // constraint functor (runtime Derivatives<Dual> — NOT the compile-time-N
    // ga::grad_vec). Line search on the ℓ₁ merit φ = f + μ‖c‖₁.
    // cons is a random-access container of constraint functors, each callable
    // like the objective (Dual-vector callable). Constraint functors must be
    // file/namespace-scope template functors (local classes can't be templated).
    // =======================================================================
    template<typename T>
    struct SQP {
        std::size_t max_iter{50};
        T tol{static_cast<T>(1e-8)};
        T merit_mu{static_cast<T>(10)};

        template<typename F, typename Deriv, typename ConSet>
        [[nodiscard]] std::expected<Result<T>, Diagnosis>
        solve(const F& f, const ga::Vector<T>& x0, const Deriv& deriv, const ConSet& cons) const {
            const std::size_t n = x0.size();
            const std::size_t m = cons.size();
            ga::Vector<T> x = x0, g(n);
            EqualityQP<T> qp;
            Result<T> r; r.status = Status::MaxIterations;

            auto eval_c = [&](const ga::Vector<T>& xx) {
                ga::Vector<T> cv(m);
                for (std::size_t i = 0; i < m; ++i) cv[i] = cons[i](xx);
                return cv;
            };
            auto c1norm = [&](const ga::Vector<T>& cv) {
                T s{}; for (std::size_t i = 0; i < cv.size(); ++i) s += std::abs(cv[i]); return s;
            };

            for (std::size_t it = 0; it < max_iter; ++it) {
                deriv.grad(f, x, g);
                ga::Vector<T> cv = eval_c(x);

                // objective Hessian W, matrix-free columns (Newton pattern)
                ga::Matrix<T> W(n, n, T{0});
                { ga::Vector<T> e(n, T{0}), col(n);
                  for (std::size_t j = 0; j < n; ++j) {
                      e[j] = T{1}; deriv.hessian_vec(f, x, e, col);
                      for (std::size_t i = 0; i < n; ++i) W(i,j) = col[i];
                      e[j] = T{0};
                  }
                  for (std::size_t i = 0; i < n; ++i)
                      for (std::size_t j = i+1; j < n; ++j) { T a=T{0.5}*(W(i,j)+W(j,i)); W(i,j)=a; W(j,i)=a; } }

                // constraint Jacobian J (m×n), row i = ∇cᵢ(x)
                ga::Matrix<T> J(m, n, T{0});
                { ga::Vector<T> row(n);
                  for (std::size_t i = 0; i < m; ++i) {
                      deriv.grad(cons[i], x, row);
                      for (std::size_t j = 0; j < n; ++j) J(i,j) = row[j];
                  } }

                // QP subproblem:  min ½pᵀWp + gᵀp  s.t.  J p = −c
                ga::Vector<T> negc(m);
                for (std::size_t i = 0; i < m; ++i) negc[i] = -cv[i];
                auto sub = qp.solve(W, g, J, negc);
                if (!sub) return std::unexpected(sub.error());
                const ga::Vector<T>& p   = sub->first;
                const ga::Vector<T>& lam = sub->second;

                // KKT stationarity ‖∇f − Jᵀλ‖ + feasibility ‖c‖
                ga::Vector<T> stat = g;
                for (std::size_t j = 0; j < n; ++j)
                    for (std::size_t i = 0; i < m; ++i) stat[j] -= J(i,j)*lam[i];
                const T kkt = detail::nrm2(stat) + c1norm(cv);
                // certificate (append-only; overwritten each iteration, so the
                // last one reflects the returned point)
                r.kkt_stationarity     = detail::nrm2(stat);
                r.primal_infeasibility = c1norm(cv);
                r.multipliers          = lam;
                if (kkt < tol) { r.status = Status::Converged; r.iterations = it; break; }

                // ℓ₁-merit backtracking on the full step p
                T alpha = T{1};
                const T phi0 = f(x) + merit_mu * c1norm(cv);
                ga::Vector<T> xn(n);
                for (std::size_t bt = 0; bt < 30; ++bt) {
                    for (std::size_t j = 0; j < n; ++j) xn[j] = x[j] + alpha*p[j];
                    const T phi = f(xn) + merit_mu * c1norm(eval_c(xn));
                    if (std::isfinite(phi) && phi <= phi0) break;
                    alpha *= T{0.5};
                }
                for (std::size_t j = 0; j < n; ++j) x[j] += alpha*p[j];
                r.iterations = it + 1;
            }

            r.x = x; r.f = f(x);
            deriv.grad(f, x, g);
            r.grad_norm = detail::nrm2(g);
            return r;
        }
    };

    // =======================================================================
    // Inequality-constrained SQP  (SLSQP-grade)
    //   min f(x)  s.t.  c_eq(x) = 0,  c_ineq(x) ≤ 0
    // Each major iteration builds the QP subproblem
    //   min ½pᵀWp + gᵀp   s.t.  J_eq p = −c_eq,  J_ineq p ≤ −c_ineq
    // and solves it with an active-set loop: the working set holds all equality
    // rows plus the currently-active inequality rows, and each pass is an
    // equality-constrained KKT solve (delegated to EqualityQP / ga::schur_solve).
    // Inactive inequalities are added when a step would violate them (nearest
    // blocking row via a ratio test); active inequalities with a negative
    // multiplier are dropped (they want to become slack). W is a damped-BFGS
    // approximation of the *Lagrangian* Hessian — Powell damping keeps it SPD so
    // every inner QP is convex. Steps are accepted by an ℓ₁-merit backtrack whose
    // penalty covers both ‖c_eq‖₁ and Σ max(0, c_ineq). Convention c_ineq ≤ 0
    // matches the EDSL subject_to residual (Le → l−r, feasible ⇔ ≤ 0).
    //
    // c_eq / c_ineq are random-access containers of Dual-vector-callable
    // functors (like SQP's cons); either may be empty.
    // =======================================================================
    template<typename T>
    struct SQP_Ineq {
        std::size_t max_iter{50};
        std::size_t inner_iter{50};        // active-set passes per major step
        T tol{static_cast<T>(1e-8)};
        T merit_mu{static_cast<T>(10)};
        T active_tol{static_cast<T>(1e-8)};

        template<typename F, typename Deriv, typename EqSet, typename IneqSet>
        [[nodiscard]] std::expected<Result<T>, Diagnosis>
        solve(const F& f, const ga::Vector<T>& x0, const Deriv& deriv,
              const EqSet& c_eq, const IneqSet& c_ineq) const {
            const std::size_t n  = x0.size();
            const std::size_t me = c_eq.size();
            const std::size_t mi = c_ineq.size();
            ga::Vector<T> x = x0, g(n);
            EqualityQP<T> qp;
            Result<T> r; r.status = Status::MaxIterations;

            auto eval_set = [&](const auto& set, const ga::Vector<T>& xx) {
                ga::Vector<T> cv(set.size());
                for (std::size_t i = 0; i < set.size(); ++i) cv[i] = set[i](xx);
                return cv;
            };
            auto jac_set = [&](const auto& set, const ga::Vector<T>& xx) {
                ga::Matrix<T> J(set.size(), n, T{0});
                ga::Vector<T> row(n);
                for (std::size_t i = 0; i < set.size(); ++i) {
                    deriv.grad(set[i], xx, row);
                    for (std::size_t j = 0; j < n; ++j) J(i,j) = row[j];
                }
                return J;
            };
            auto c1norm = [&](const ga::Vector<T>& cv) {
                T s{}; for (std::size_t i = 0; i < cv.size(); ++i) s += std::abs(cv[i]); return s;
            };
            auto viol1 = [&](const ga::Vector<T>& cv) {          // Σ max(0, cᵢ)
                T s{}; for (std::size_t i = 0; i < cv.size(); ++i) s += std::max(T{0}, cv[i]); return s;
            };

            // damped-BFGS Lagrangian-Hessian approximation, initialized to I.
            ga::Matrix<T> W(n, n, T{0});
            for (std::size_t i = 0; i < n; ++i) W(i,i) = T{1};
            ga::Vector<T> x_prev, gL_prev;   // for the BFGS pair (∇ₓL)
            bool have_prev = false;

            // Lagrangian gradient at (xx) given multipliers. The inner QP is
            // assembled with active inequalities linearized as  Jᵢ p = −cᵢ  and
            // returns multipliers satisfying  W p + g = Awᵀ·qp_lam, so the μ
            // block enters stationarity with the SAME sign as the assembled row,
            // i.e. ∇ₓL = ∇f − J_eqᵀλ + J_ineqᵀμ. (Using −J_ineqᵀμ leaves a
            // residual of 2·J_ineqᵀμ at the optimum — e.g. ‖(4,4)‖ on the
            // x₀+x₁≥2 active-constraint case, which read as non-stationary.)
            auto lag_grad = [&](const ga::Vector<T>& gx, const ga::Matrix<T>& Je,
                                const ga::Vector<T>& lam, const ga::Matrix<T>& Ji,
                                const ga::Vector<T>& mu) {
                ga::Vector<T> gl = gx;
                for (std::size_t j = 0; j < n; ++j) {
                    for (std::size_t i = 0; i < me; ++i) gl[j] -= Je(i,j)*lam[i];
                    for (std::size_t i = 0; i < mi; ++i) gl[j] += Ji(i,j)*mu[i];
                }
                return gl;
            };

            for (std::size_t it = 0; it < max_iter; ++it) {
                deriv.grad(f, x, g);
                ga::Vector<T> ce = eval_set(c_eq,   x);
                ga::Vector<T> ci = eval_set(c_ineq, x);
                ga::Matrix<T> Je = jac_set(c_eq,   x);
                ga::Matrix<T> Ji = jac_set(c_ineq, x);

                // ---- inner active-set QP -----------------------------------
                // working set = all eq rows + active ineq rows. Start with the
                // inequalities that are (near-)violated at x (cᵢ ≥ −active_tol).
                std::vector<char> active(mi, 0);
                for (std::size_t i = 0; i < mi; ++i)
                    active[i] = (ci[i] >= -active_tol) ? 1 : 0;

                ga::Vector<T> p(n, T{0});
                ga::Vector<T> lam(me, T{0});     // equality multipliers
                ga::Vector<T> mu(mi, T{0});      // inequality multipliers (≥ 0)

                bool qp_ok = false;
                for (std::size_t inner = 0; inner < inner_iter; ++inner) {
                    // assemble working-set constraint matrix Aw p = bw
                    std::vector<std::size_t> act_idx;
                    for (std::size_t i = 0; i < mi; ++i) if (active[i]) act_idx.push_back(i);
                    const std::size_t mw = me + act_idx.size();

                    ga::Matrix<T> Aw(mw, n, T{0});
                    ga::Vector<T> bw(mw);
                    for (std::size_t i = 0; i < me; ++i) {
                        for (std::size_t j = 0; j < n; ++j) Aw(i,j) = Je(i,j);
                        bw[i] = -ce[i];
                    }
                    for (std::size_t k = 0; k < act_idx.size(); ++k) {
                        const std::size_t i = act_idx[k];
                        for (std::size_t j = 0; j < n; ++j) Aw(me+k,j) = Ji(i,j);
                        bw[me+k] = -ci[i];              // active ⇒ treated as equality
                    }

                    ga::Vector<T> qp_lam;
                    if (mw == 0) {
                        // unconstrained QP step: W p = −g  (W SPD)
                        ga::Vector<T> negg(n);
                        for (std::size_t j = 0; j < n; ++j) negg[j] = -g[j];
                        p = ga::solve(W, negg, ga::MatrixKind::SPD);
                        qp_lam = ga::Vector<T>(0);
                    } else {
                        auto sub = qp.solve(W, g, Aw, bw);
                        if (!sub) return std::unexpected(sub.error());
                        p       = sub->first;
                        qp_lam  = sub->second;
                    }

                    // split working-set multipliers back into lam / mu
                    for (std::size_t i = 0; i < me; ++i) lam[i] = qp_lam[i];
                    for (std::size_t i = 0; i < mi; ++i) mu[i] = T{0};
                    for (std::size_t k = 0; k < act_idx.size(); ++k)
                        mu[act_idx[k]] = qp_lam[me+k];

                    // check inactive inequalities for violation by the step:
                    // Jᵢ p + cᵢ > 0 ⇒ blocked. Add the most-violated to the set.
                    std::size_t add = mi; T worst = active_tol;
                    for (std::size_t i = 0; i < mi; ++i) {
                        if (active[i]) continue;
                        T jp = ci[i];
                        for (std::size_t j = 0; j < n; ++j) jp += Ji(i,j)*p[j];
                        if (jp > worst) { worst = jp; add = i; }
                    }
                    if (add != mi) { active[add] = 1; continue; }   // grow set, resolve

                    // all inactive satisfied. Drop an active row with μ < 0
                    // (it prefers to be slack), else the QP is optimal.
                    std::size_t drop = mi; T most_neg = -active_tol;
                    for (std::size_t i = 0; i < mi; ++i) {
                        if (active[i] && mu[i] < most_neg) { most_neg = mu[i]; drop = i; }
                    }
                    if (drop != mi) { active[drop] = 0; continue; }
                    qp_ok = true; break;                            // KKT of the QP met
                }
                if (!qp_ok)
                    return std::unexpected(Diagnosis{Cause::NumericalError,
                        "SQP_Ineq: inner active-set QP did not converge", it});

                // ---- KKT certificate at the current point ------------------
                ga::Vector<T> stat = lag_grad(g, Je, lam, Ji, mu);
                T compl_gap{};  for (std::size_t i = 0; i < mi; ++i) compl_gap += mu[i]*ci[i];
                T dual_inf{};   for (std::size_t i = 0; i < mi; ++i) dual_inf += std::min(T{0}, mu[i])*std::min(T{0}, mu[i]);
                const T primal = c1norm(ce) + viol1(ci);
                r.kkt_stationarity     = detail::nrm2(stat);
                r.primal_infeasibility = primal;
                r.complementarity      = std::abs(compl_gap);
                r.dual_infeasibility   = std::sqrt(dual_inf);
                { ga::Vector<T> mult(me+mi);
                  for (std::size_t i = 0; i < me; ++i) mult[i] = lam[i];
                  for (std::size_t i = 0; i < mi; ++i) mult[me+i] = mu[i];
                  r.multipliers = mult; }

                const T kkt = r.kkt_stationarity + primal;
                if (kkt < tol) { r.status = Status::Converged; r.iterations = it; break; }

                // ---- ℓ₁-merit backtracking on the step p -------------------
                auto merit = [&](const ga::Vector<T>& xx) {
                    return f(xx) + merit_mu * (c1norm(eval_set(c_eq, xx)) + viol1(eval_set(c_ineq, xx)));
                };
                const T phi0 = merit(x);
                T alpha = T{1};
                ga::Vector<T> xn(n);
                for (std::size_t bt = 0; bt < 30; ++bt) {
                    for (std::size_t j = 0; j < n; ++j) xn[j] = x[j] + alpha*p[j];
                    const T phi = merit(xn);
                    if (std::isfinite(phi) && phi <= phi0) break;
                    alpha *= T{0.5};
                }
                for (std::size_t j = 0; j < n; ++j) xn[j] = x[j] + alpha*p[j];

                // ---- damped-BFGS update of W from the Lagrangian gradient --
                ga::Vector<T> gxn(n); deriv.grad(f, xn, gxn);
                ga::Matrix<T> Je_n = jac_set(c_eq, xn);
                ga::Matrix<T> Ji_n = jac_set(c_ineq, xn);
                ga::Vector<T> gL_new = lag_grad(gxn, Je_n, lam, Ji_n, mu);
                if (have_prev) {
                    ga::Vector<T> s(n), y(n);
                    for (std::size_t j = 0; j < n; ++j) { s[j] = xn[j]-x_prev[j]; y[j] = gL_new[j]-gL_prev[j]; }
                    powell_bfgs_update(W, s, y);
                }
                x_prev = xn; gL_prev = gL_new; have_prev = true;

                x = xn;
                r.iterations = it + 1;
            }

            r.x = x; r.f = f(x);
            deriv.grad(f, x, g);
            r.grad_norm = detail::nrm2(g);
            return r;
        }

    private:
        // Powell-damped BFGS update of W (keeps W SPD even when sᵀy is small or
        // negative under active inequalities). Nocedal–Wright Procedure 18.2.
        static void powell_bfgs_update(ga::Matrix<T>& W, const ga::Vector<T>& s,
                                       const ga::Vector<T>& y) {
            const std::size_t n = s.size();
            ga::Vector<T> Ws(n, T{0});
            for (std::size_t i = 0; i < n; ++i)
                for (std::size_t j = 0; j < n; ++j) Ws[i] += W(i,j)*s[j];
            T sWs{}, sy{};
            for (std::size_t j = 0; j < n; ++j) { sWs += s[j]*Ws[j]; sy += s[j]*y[j]; }
            if (sWs <= T{0}) return;                          // degenerate; skip
            T theta = T{1};
            if (sy < static_cast<T>(0.2)*sWs)
                theta = static_cast<T>(0.8)*sWs / (sWs - sy);
            // damped curvature vector  r = θ y + (1−θ) W s
            ga::Vector<T> rr(n);
            for (std::size_t j = 0; j < n; ++j) rr[j] = theta*y[j] + (T{1}-theta)*Ws[j];
            T sr{}; for (std::size_t j = 0; j < n; ++j) sr += s[j]*rr[j];
            if (sr <= T{0} || sWs <= T{0}) return;
            // W ← W − (Ws)(Ws)ᵀ/sWs + rrᵀ/sr
            for (std::size_t i = 0; i < n; ++i)
                for (std::size_t j = 0; j < n; ++j)
                    W(i,j) += rr[i]*rr[j]/sr - Ws[i]*Ws[j]/sWs;
        }
    };

    // =======================================================================
    // Augmented Lagrangian (method of multipliers) for equality constraints
    //   min f(x)  s.t.  cᵢ(x) = 0.
    // Outer loop minimizes the augmented Lagrangian
    //   L_A(x; λ, ρ) = f(x) − Σ λᵢ cᵢ(x) + (ρ/2) Σ cᵢ(x)²
    // over x with an inner unconstrained Solver (caller supplies it, e.g.
    // Solver<LBFGS<T>>), then updates λᵢ ← λᵢ − ρ cᵢ(x) and grows ρ when the
    // constraint norm stalls. The augmented objective is a generic lambda
    // closing over f, cons, λ, ρ — an unnamed closure whose operator() is a
    // template, which (unlike a *named* local class) is legal and gives the
    // Dual gradient path the templated call it needs.
    // constrained.hpp stays self-contained: the inner Solver type is a template
    // parameter, so there is no hard dependence on unconstrained.hpp here.
    // =======================================================================
    template<typename T>
    struct AugmentedLagrangian {
        std::size_t outer_iter{30};
        T rho0{T{1}};
        T rho_grow{static_cast<T>(10)};
        T tol{static_cast<T>(1e-8)};

        // Objective F and each constraint functor must be Dual-vector callable.
        // InnerSolver is any kalpa Solver whose Algorithm is unconstrained
        // (e.g. Solver<LBFGS<T>>). It is passed by value and reused each outer.
        template<typename F, typename Deriv, typename ConSet, typename InnerSolver>
        [[nodiscard]] std::expected<Result<T>, Diagnosis>
        solve(const F& f, const ga::Vector<T>& x0, const Deriv& deriv,
              const ConSet& cons, InnerSolver inner) const {
            const std::size_t n = x0.size();
            const std::size_t m = cons.size();
            ga::Vector<T> x = x0, lambda(m, T{0});
            T rho = rho0;
            T prev_cnorm = std::numeric_limits<T>::infinity();
            Result<T> r; r.status = Status::MaxIterations;

            auto cnorm = [&](const ga::Vector<T>& xx) {
                T s{}; for (std::size_t i = 0; i < m; ++i) { const T ci = cons[i](xx); s += ci*ci; }
                return std::sqrt(s);
            };

            for (std::size_t k = 0; k < outer_iter; ++k) {
                // augmented objective as a template lambda (namespace-scope-equivalent:
                // a generic lambda is a unique closure type with a templated operator(),
                // which the local-class restriction does not forbid — only *named*
                // local classes cannot have member templates).
                auto La = [&f, &cons, &lambda, rho, m](const auto& xx) {
                    using S = std::decay_t<decltype(xx[0])>;
                    S val = f(xx);
                    for (std::size_t i = 0; i < m; ++i) {
                        S ci = cons[i](xx);
                        val = val - static_cast<S>(lambda[i]) * ci
                                  + static_cast<S>(rho) * static_cast<S>(0.5) * ci * ci;
                    }
                    return val;
                };
                auto prob = make_problem<T>(La);
                auto res = inner.solve(prob, x);
                if (!res) return std::unexpected(res.error());
                x = res->x;

                const T cn = cnorm(x);
                if (cn < tol) { r.status = Status::Converged; r.iterations = k; break; }

                // multiplier update  λᵢ ← λᵢ − ρ cᵢ(x)
                for (std::size_t i = 0; i < m; ++i) lambda[i] -= rho * cons[i](x);
                // grow ρ if feasibility did not improve enough
                if (cn > static_cast<T>(0.25) * prev_cnorm) rho *= rho_grow;
                prev_cnorm = cn;
                r.iterations = k + 1;
            }

            r.x = x; r.f = f(x);
            ga::Vector<T> g(n); deriv.grad(f, x, g);
            r.grad_norm = detail::nrm2(g);
            return r;
        }
    };

} // namespace kalpa

#endif // PEBBLE_KALPA_ALGO_CONSTRAINED_HPP
