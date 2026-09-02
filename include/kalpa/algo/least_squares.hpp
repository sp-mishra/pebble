#pragma once
// ============================================================================
// kalpa/algo/least_squares.hpp — nonlinear least-squares (LM, Gauss–Newton)
// ============================================================================
// Nonlinear least-squares minimizes ½‖r(x)‖² for a residual vector r(x) with
// m components r_i(x). This is a distinct problem shape from the Solver's
// Algorithm loop (which minimizes a scalar objective through a line search), so
// it lives in its own opt-in header with its own solve drivers.
//
//   • LevenbergMarquardt — damped Gauss–Newton. Solves the SPD normal equations
//       (JᵀJ + λ·diag(JᵀJ)) p = −Jᵀr        (Marquardt scaling)
//     through ga::solve(…, SPD) (Cholesky). A trust-ratio test adapts λ: shrink
//     it on a successful step (→ Gauss–Newton), grow it on a rejected one
//     (→ steepest descent). The scaled diagonal keeps the matrix SPD so the
//     Cholesky path never fails.
//   • GaussNewton — undamped. Solves the least-squares system J p = −r directly
//       through ga::qr / ga::qr_solve (the numerically robust path, no λ).
//
// The Jacobian is built row-by-row from Derivatives (forward-mode AD by default)
// via detail::jacobian. Because the m residual gradients are independent, an
// opt-in ParallelEval policy fans the row fill out through pravaha
// (jacobian_parallel below); the SerialEval default keeps the base path
// dependency-free and zero-overhead. Parallelism is applied ONLY to the
// Jacobian — the QP/normal-equation solve, the ratio test and the residual
// evaluation are cheap relative to m AD gradient passes.
//
// Residuals are supplied as a random-access container of Dual-callable scalar
// functors r_i(x) (the same shape SQP takes for its constraint set).
// ============================================================================

#ifndef PEBBLE_KALPA_ALGO_LEAST_SQUARES_HPP
#define PEBBLE_KALPA_ALGO_LEAST_SQUARES_HPP

#include <kalpa/core/solver.hpp>
#include <kalpa/core/problem.hpp>
#include <containers/matrix/dense.hpp>
#include <containers/matrix/solve.hpp>
#include <containers/matrix/factorize.hpp>
#include <pravaha/pravaha.hpp>
#include <cmath>
#include <cstddef>
#include <expected>
#include <numeric>
#include <vector>

namespace kalpa {
    // =======================================================================
    // Eval policies for the Jacobian fill. Mirror global.hpp's SerialEval /
    // ParallelEval, but operate on residual-gradient rows rather than a
    // population score. SerialEval is the zero-overhead default.
    // =======================================================================
    struct SerialJacobian {
        template <typename Deriv, typename ResSet, typename T>
        void operator()(const Deriv& deriv, const ResSet& residuals,
                        const ga::Vector<T>& x, ga::Matrix<T>& J) const {
            detail::jacobian(deriv, residuals, x, J); // serial row fill
        }
    };

    // Parallel row fill: row i = ∇residuals[i](x). Rows are disjoint, so the
    // writes never race. Each task runs its own AD gradient pass on a private
    // temporary — the only shared write is into distinct rows of J.
    struct ParallelJacobian {
        std::size_t chunk{32};

        template <typename Deriv, typename ResSet, typename T>
        void operator()(const Deriv& deriv, const ResSet& residuals,
                        const ga::Vector<T>& x, ga::Matrix<T>& J) const {
            const std::size_t m = residuals.size();
            const std::size_t n = x.size();
            std::vector<std::size_t> idx(m);
            std::iota(idx.begin(), idx.end(), std::size_t{0});
            const auto* res_ptr = &residuals;
            const auto* x_ptr = &x;
            auto* J_ptr = &J;
            auto expr = pravaha::lazy_parallel_for(idx,
                                                   [res_ptr, x_ptr, J_ptr, &deriv, n](std::size_t i) {
                                                       ga::Vector<T> row(n);
                                                       deriv.grad((*res_ptr)[i], *x_ptr, row);
                                                       for (std::size_t j = 0; j < n; ++j) (*J_ptr)(i, j) = row[j];
                                                   }, chunk);
            pravaha::Runner<pravaha::JThreadBackend> runner;
            runner.submit(std::move(expr));
        }
    };

    namespace detail {
        // r(x) → out, ‖r‖² returned. Residuals are Dual-callable but evaluated
        // here at the scalar (value) level.
        template <typename ResSet, typename T>
        T residuals_at(const ResSet& residuals, const ga::Vector<T>& x,
                       ga::Vector<T>& out) {
            const std::size_t m = residuals.size();
            T ss{};
            for (std::size_t i = 0; i < m; ++i) {
                const T ri = residuals[i](x);
                out[i] = ri;
                ss += ri * ri;
            }
            return ss;
        }
    } // namespace detail

    // =======================================================================
    // Levenberg–Marquardt
    // =======================================================================
    template <typename T = double, typename JacEval = SerialJacobian>
    struct LevenbergMarquardt {
        std::size_t max_iter{100};
        T tol{static_cast<T>(1e-10)}; // ‖Jᵀr‖ stationarity tolerance
        T step_tol{static_cast<T>(1e-14)}; // ‖p‖ progress tolerance
        T lambda0{static_cast<T>(1e-3)};
        T lambda_up{static_cast<T>(10)};
        T lambda_down{static_cast<T>(0.1)};
        [[no_unique_address]] JacEval jac{};

        template <typename ResSet, typename Deriv = Derivatives<Dual, T>>
        [[nodiscard]] std::expected<Result<T>, Diagnosis>
        solve(const ResSet& residuals, const ga::Vector<T>& x0,
              const Deriv& deriv = {}) const {
            const std::size_t m = residuals.size();
            const std::size_t n = x0.size();
            if (m == 0)
                return std::unexpected(Diagnosis{
                    Cause::NumericalError,
                    "least-squares: empty residual set", 0
                });

            ga::Vector<T> x = x0;
            ga::Vector<T> r(m), rn(m);
            T ss = detail::residuals_at(residuals, x, r);
            if (!std::isfinite(ss))
                return std::unexpected(Diagnosis{
                    Cause::NaNTrap,
                    "least-squares: residual is NaN/Inf at x0", 0
                });

            ga::Matrix<T> J(m, n);
            T lambda = lambda0;
            std::size_t iter = 0;
            T grad_norm{};

            for (iter = 0; iter < max_iter; ++iter) {
                jac(deriv, residuals, x, J); // J (m×n)

                // Normal-equation blocks: A = JᵀJ, g = Jᵀr.
                ga::Matrix<T> Jt = J.transpose(); // (n×m)
                ga::Matrix<T> A = Jt * J; // (n×n) SPD (+λ diag)
                ga::Vector<T> g(n); // Jᵀr
                for (std::size_t i = 0; i < n; ++i) {
                    T acc{};
                    for (std::size_t k = 0; k < m; ++k) acc += J(k, i) * r[k];
                    g[i] = acc;
                }
                grad_norm = detail::nrm2(g);
                if (grad_norm <= tol) break; // stationary

                // Marquardt-scaled damping: A ← A + λ·diag(A). Damp a tiny
                // diagonal with λ itself so the system stays SPD.
                ga::Matrix<T> Ad = A;
                for (std::size_t i = 0; i < n; ++i) {
                    const T dii = A(i, i);
                    Ad(i, i) = dii + lambda * (dii > T{0} ? dii : T{1});
                }
                // rhs = −g
                ga::Vector<T> rhs(n);
                for (std::size_t i = 0; i < n; ++i) rhs[i] = -g[i];

                auto p = ga::solve(Ad, rhs, ga::MatrixKind::SPD);

                ga::Vector<T> xn(n);
                for (std::size_t i = 0; i < n; ++i) xn[i] = x[i] + p[i];
                const T ss_new = detail::residuals_at(residuals, xn, rn);

                if (std::isfinite(ss_new) && ss_new < ss) {
                    // successful step: accept, relax damping toward GN
                    x = xn;
                    r = rn;
                    ss = ss_new;
                    lambda *= lambda_down;
                    T pnorm{};
                    for (std::size_t i = 0; i < n; ++i) pnorm += p[i] * p[i];
                    if (std::sqrt(pnorm) <= step_tol) break;
                }
                else {
                    // rejected: tighten damping toward steepest descent, retry
                    lambda *= lambda_up;
                    if (lambda > static_cast<T>(1e12))
                        return std::unexpected(Diagnosis{
                            Cause::LineSearchFail,
                            "least-squares: LM damping diverged (no decrease)", iter
                        });
                }
            }

            Result<T> res;
            res.x = x;
            res.f = static_cast<T>(0.5) * ss;
            res.grad_norm = grad_norm;
            res.iterations = iter;
            res.residual_norm = std::sqrt(ss);
            res.status = (grad_norm <= tol) ? Status::Converged : Status::MaxIterations;
            return res;
        }
    };

    // =======================================================================
    // Gauss–Newton (undamped; QR least-squares on J p = −r)
    // =======================================================================
    template <typename T = double, typename JacEval = SerialJacobian>
    struct GaussNewton {
        std::size_t max_iter{100};
        T tol{static_cast<T>(1e-10)};
        T step_tol{static_cast<T>(1e-14)};
        [[no_unique_address]] JacEval jac{};

        template <typename ResSet, typename Deriv = Derivatives<Dual, T>>
        [[nodiscard]] std::expected<Result<T>, Diagnosis>
        solve(const ResSet& residuals, const ga::Vector<T>& x0,
              const Deriv& deriv = {}) const {
            const std::size_t m = residuals.size();
            const std::size_t n = x0.size();
            if (m == 0 || m < n)
                return std::unexpected(Diagnosis{
                    Cause::NumericalError,
                    "Gauss–Newton: need an overdetermined residual set (m ≥ n)", 0
                });

            ga::Vector<T> x = x0;
            ga::Vector<T> r(m);
            T ss = detail::residuals_at(residuals, x, r);
            if (!std::isfinite(ss))
                return std::unexpected(Diagnosis{
                    Cause::NaNTrap,
                    "Gauss–Newton: residual is NaN/Inf at x0", 0
                });

            ga::Matrix<T> J(m, n);
            std::size_t iter = 0;
            T grad_norm{};

            for (iter = 0; iter < max_iter; ++iter) {
                jac(deriv, residuals, x, J);

                // stationarity ‖Jᵀr‖
                ga::Vector<T> g(n);
                for (std::size_t i = 0; i < n; ++i) {
                    T acc{};
                    for (std::size_t k = 0; k < m; ++k) acc += J(k, i) * r[k];
                    g[i] = acc;
                }
                grad_norm = detail::nrm2(g);
                if (grad_norm <= tol) break;

                // QR least-squares: min ‖J p − (−r)‖.
                ga::Vector<T> negr(m);
                for (std::size_t k = 0; k < m; ++k) negr[k] = -r[k];
                auto qr = ga::qr(J);
                if (!qr.info.ok)
                    return std::unexpected(Diagnosis{
                        Cause::SingularKKT,
                        "Gauss–Newton: QR factorization failed (rank-deficient J)", iter
                    });
                auto p = ga::qr_solve(qr, negr);

                ga::Vector<T> xn(n);
                for (std::size_t i = 0; i < n; ++i) xn[i] = x[i] + p[i];
                const T ss_new = detail::residuals_at(residuals, xn, r);
                x = xn;
                ss = ss_new;

                T pnorm{};
                for (std::size_t i = 0; i < n; ++i) pnorm += p[i] * p[i];
                if (std::sqrt(pnorm) <= step_tol) break;
            }

            Result<T> res;
            res.x = x;
            res.f = static_cast<T>(0.5) * ss;
            res.grad_norm = grad_norm;
            res.iterations = iter;
            res.residual_norm = std::sqrt(ss);
            res.status = (grad_norm <= tol) ? Status::Converged : Status::MaxIterations;
            return res;
        }
    };
} // namespace kalpa

#endif // PEBBLE_KALPA_ALGO_LEAST_SQUARES_HPP
