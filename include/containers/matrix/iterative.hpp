#pragma once
// ============================================================================
// iterative.hpp — Ganita iterative solvers + multigrid
// ============================================================================
// CG / BiCGSTAB / GMRES / MINRES / FGMRES / Jacobi / GS / SOR
// Preconditioners: JacobiPrecond, Ic0Precond, Ilu0Precond
// Multigrid: geometric mg_vcycle + AmgHierarchy concept hook
// All matrix-free capable — accepts any callable apply(x)->y.
// Inner ops (axpy/dot/nrm2) call ts:: BLAS primitives on DynamicTensor.
// ============================================================================

#ifndef PEBBLE_CONTAINERS_MATRIX_ITERATIVE_HPP
#define PEBBLE_CONTAINERS_MATRIX_ITERATIVE_HPP

#include <containers/matrix/dense.hpp>
#include <containers/matrix/sparse.hpp>
#include <algorithm>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <functional>
#include <stdexcept>
#include <vector>

namespace ga {

    // -----------------------------------------------------------------------
    // IterResult — convergence record
    // -----------------------------------------------------------------------
    template<typename T>
    struct IterResult {
        bool   converged{false};
        std::size_t iterations{0};
        T      final_residual{T{0}};
        std::vector<T> residual_history;
    };

    // -----------------------------------------------------------------------
    // Preconditioner concept: apply(r) → z  (z ≈ M⁻¹ r)
    // -----------------------------------------------------------------------
    template<typename P, typename T>
    concept Preconditioner = requires(const P& p, const Vector<T>& r) {
        { p.apply(r) } -> std::same_as<Vector<T>>;
    };

    // -----------------------------------------------------------------------
    // IdentityPrecond — no-op preconditioner
    // -----------------------------------------------------------------------
    template<typename T>
    struct IdentityPrecond {
        Vector<T> apply(const Vector<T>& r) const { return r; }
    };

    // -----------------------------------------------------------------------
    // JacobiPrecond — diagonal scaling: z_i = r_i / A_ii
    // -----------------------------------------------------------------------
    template<typename T,
             typename SP = ts::DefaultStoragePolicy,
             typename CP = ts::DefaultComputationPolicy>
    struct JacobiPrecond {
        Vector<T,SP,CP> inv_diag;

        explicit JacobiPrecond(const Matrix<T,SP,CP>& A) {
            const std::size_t N = A.rows();
            inv_diag = Vector<T,SP,CP>(N);
            for (std::size_t i = 0; i < N; ++i) {
                T d = A(i, i);
                inv_diag(i) = (std::abs(d) > T{0} ? T{1}/d : T{1});
            }
        }

        Vector<T,SP,CP> apply(const Vector<T,SP,CP>& r) const {
            const std::size_t N = r.size();
            Vector<T,SP,CP> z(N);
            for (std::size_t i = 0; i < N; ++i)
                z(i) = inv_diag(i) * r(i);
            return z;
        }
    };

    // -----------------------------------------------------------------------
    // Ic0Precond — level-0 incomplete Cholesky for SPD CSR matrices
    // -----------------------------------------------------------------------
    template<typename T>
    struct Ic0Precond {
        CsrMatrix<T> L; // lower triangular factor (sparsity = pattern of A)

        explicit Ic0Precond(const CsrMatrix<T>& A) {
            // IC0: only fill nonzeros that exist in A
            const std::size_t N = A.rows();
            L = A; // copy sparsity + values
            auto* val = L.values.data();
            const auto* rp = L.row_ptr.data();
            const auto* ci = L.col_idx.data();

            for (std::size_t i = 0; i < N; ++i) {
                for (std::size_t jj = rp[i]; jj < rp[i+1]; ++jj) {
                    std::size_t j = ci[jj];
                    if (j >= i) break;
                    // subtract contribution from already-computed columns
                    T s = val[jj];
                    for (std::size_t kk = rp[i]; kk < jj; ++kk) {
                        std::size_t k = ci[kk];
                        // find (j,k) in L
                        for (std::size_t ll = rp[j]; ll < rp[j+1]; ++ll) {
                            if (ci[ll] == k) { s -= val[kk] * val[ll]; break; }
                            if (ci[ll] > k) break;
                        }
                    }
                    // find diag of L at row j
                    T ljj = T{1};
                    for (std::size_t ll = rp[j]; ll < rp[j+1]; ++ll)
                        if (ci[ll] == j) { ljj = val[ll]; break; }
                    val[jj] = s / ljj;
                }
                // diagonal
                T d = T{0};
                for (std::size_t kk = rp[i]; kk < rp[i+1]; ++kk)
                    if (ci[kk] == i) { d = val[kk]; break; }
                for (std::size_t kk = rp[i]; kk < rp[i+1]; ++kk) {
                    if (ci[kk] >= i) break;
                    d -= val[kk] * val[kk];
                }
                // store sqrt on diagonal
                for (std::size_t kk = rp[i]; kk < rp[i+1]; ++kk)
                    if (ci[kk] == i) { val[kk] = (d > T{0} ? std::sqrt(d) : T{1}); break; }
            }
        }

        // solve L·Lᵀ·z = r via two triangular passes
        Vector<T> apply(const Vector<T>& r) const {
            const std::size_t N = r.size();
            Vector<T> y(N), z(N);
            const auto* val = L.values.data();
            const auto* rp = L.row_ptr.data();
            const auto* ci = L.col_idx.data();

            // forward: L·y = r
            for (std::size_t i = 0; i < N; ++i) {
                T s = r(i);
                T lii = T{1};
                for (std::size_t jj = rp[i]; jj < rp[i+1]; ++jj) {
                    if (ci[jj] < i) s -= val[jj] * y(ci[jj]);
                    else if (ci[jj] == i) lii = val[jj];
                }
                y(i) = s / lii;
            }
            // backward: Lᵀ·z = y
            for (std::size_t ii = N; ii-- > 0;) {
                T s = y(ii);
                T lii = T{1};
                for (std::size_t jj = rp[ii]; jj < rp[ii+1]; ++jj) {
                    std::size_t j = ci[jj];
                    if (j == ii) { lii = val[jj]; continue; }
                    if (j > ii) {
                        // find (j, ii) in L
                        for (std::size_t kk = rp[j]; kk < rp[j+1]; ++kk)
                            if (ci[kk] == ii) { s -= val[kk] * z(j); break; }
                    }
                }
                z(ii) = s / lii;
            }
            return z;
        }
    };

    // -----------------------------------------------------------------------
    // Ilu0Precond — level-0 ILU for general CSR matrices
    // -----------------------------------------------------------------------
    template<typename T>
    struct Ilu0Precond {
        CsrMatrix<T> LU; // packed L (below) + U (diag+above), same sparsity as A

        explicit Ilu0Precond(const CsrMatrix<T>& A) {
            const std::size_t N = A.rows();
            LU = A;
            auto* val = LU.values.data();
            const auto* rp = LU.row_ptr.data();
            const auto* ci = LU.col_idx.data();

            for (std::size_t i = 1; i < N; ++i) {
                for (std::size_t jj = rp[i]; jj < rp[i+1]; ++jj) {
                    std::size_t j = ci[jj];
                    if (j >= i) break;
                    // find u_jj
                    T ujj = T{1};
                    for (std::size_t kk = rp[j]; kk < rp[j+1]; ++kk)
                        if (ci[kk] == j) { ujj = val[kk]; break; }
                    val[jj] /= ujj;
                    T lij = val[jj];
                    // update row i: a_ik -= l_ij * u_jk  for k > j
                    for (std::size_t kk = rp[j]; kk < rp[j+1]; ++kk) {
                        std::size_t k = ci[kk];
                        if (k <= j) continue;
                        // find (i,k)
                        for (std::size_t ll = rp[i]; ll < rp[i+1]; ++ll)
                            if (ci[ll] == k) { val[ll] -= lij * val[kk]; break; }
                    }
                }
            }
        }

        Vector<T> apply(const Vector<T>& r) const {
            const std::size_t N = r.size();
            Vector<T> y(N), z(N);
            const auto* val = LU.values.data();
            const auto* rp = LU.row_ptr.data();
            const auto* ci = LU.col_idx.data();

            // forward: L·y = r  (L has 1s on diagonal)
            for (std::size_t i = 0; i < N; ++i) {
                T s = r(i);
                for (std::size_t jj = rp[i]; jj < rp[i+1]; ++jj) {
                    if (ci[jj] < i) s -= val[jj] * y(ci[jj]);
                    else break;
                }
                y(i) = s;
            }
            // backward: U·z = y
            for (std::size_t ii = N; ii-- > 0;) {
                T s = y(ii);
                T uii = T{1};
                for (std::size_t jj = rp[ii]; jj < rp[ii+1]; ++jj) {
                    std::size_t j = ci[jj];
                    if (j == ii) { uii = val[jj]; continue; }
                    if (j > ii) s -= val[jj] * z(j);
                }
                z(ii) = s / uii;
            }
            return z;
        }
    };

    // -----------------------------------------------------------------------
    // Internal helpers — dot / axpy / nrm2 on Vector (delegates to ts BLAS)
    // -----------------------------------------------------------------------
    namespace detail {
        template<typename T, typename SP, typename CP>
        T vec_dot(const Vector<T,SP,CP>& a, const Vector<T,SP,CP>& b) {
            const std::size_t n = a.size();
            T s = T{0};
            for (std::size_t i = 0; i < n; ++i) s += a(i) * b(i);
            return s;
        }
        template<typename T, typename SP, typename CP>
        T vec_nrm2(const Vector<T,SP,CP>& v) {
            return CP::nrm2(v.tensor());
        }
        template<typename T, typename SP, typename CP>
        void vec_axpy(T alpha, const Vector<T,SP,CP>& x, Vector<T,SP,CP>& y) {
            CP::axpy(alpha, x.tensor(), y.tensor());
        }
        template<typename T, typename SP, typename CP>
        Vector<T,SP,CP> vec_scale(T s, const Vector<T,SP,CP>& v) {
            Vector<T,SP,CP> out = v;
            std::size_t n = v.size();
            for (std::size_t i = 0; i < n; ++i) out(i) *= s;
            return out;
        }
    } // namespace detail

    // -----------------------------------------------------------------------
    // Conjugate Gradient — SPD systems (Hestenes-Stiefel 1952)
    // -----------------------------------------------------------------------
    template<typename T, typename SP = ts::DefaultStoragePolicy,
             typename CP = ts::DefaultComputationPolicy,
             typename ApplyFn, typename PrecondT = IdentityPrecond<T>>
    [[nodiscard]] IterResult<T> cg(
            ApplyFn&&      apply,      // x -> A·x
            const Vector<T,SP,CP>& b,
            Vector<T,SP,CP>&       x,
            const PrecondT&        precond = PrecondT{},
            T tol = T{1e-10},
            std::size_t max_iter = 1000) {

        IterResult<T> res;
        res.residual_history.reserve(64);

        Vector<T,SP,CP> r = b;
        {
            Vector<T,SP,CP> ax = apply(x);
            std::size_t n = b.size();
            for (std::size_t i = 0; i < n; ++i) r(i) = b(i) - ax(i);
        }
        Vector<T,SP,CP> z = precond.apply(r);
        Vector<T,SP,CP> p = z;
        T rz = detail::vec_dot<T,SP,CP>(r, z);
        T b_nrm = detail::vec_nrm2<T,SP,CP>(b);
        if (b_nrm < T{1e-300}) b_nrm = T{1};

        for (std::size_t it = 0; it < max_iter; ++it) {
            Vector<T,SP,CP> ap = apply(p);
            T pap = detail::vec_dot<T,SP,CP>(p, ap);
            if (std::abs(pap) < T{1e-300}) break;
            T alpha = rz / pap;
            detail::vec_axpy<T,SP,CP>( alpha, p,  x);
            detail::vec_axpy<T,SP,CP>(-alpha, ap, r);
            T r_nrm = detail::vec_nrm2<T,SP,CP>(r);
            res.residual_history.push_back(r_nrm);
            if (r_nrm / b_nrm < tol) {
                res.converged = true;
                res.iterations = it + 1;
                res.final_residual = r_nrm;
                return res;
            }
            Vector<T,SP,CP> znew = precond.apply(r);
            T rz_new = detail::vec_dot<T,SP,CP>(r, znew);
            T beta = rz_new / rz;
            // p ← znew + β·p
            std::size_t n = p.size();
            for (std::size_t i = 0; i < n; ++i) p(i) = znew(i) + beta * p(i);
            z   = znew;
            rz  = rz_new;
        }
        res.iterations = res.residual_history.size();
        res.final_residual = res.residual_history.empty() ? T{0} : res.residual_history.back();
        return res;
    }

    // -----------------------------------------------------------------------
    // BiCGSTAB — non-symmetric systems (van der Vorst 1992)
    // -----------------------------------------------------------------------
    template<typename T, typename SP = ts::DefaultStoragePolicy,
             typename CP = ts::DefaultComputationPolicy,
             typename ApplyFn, typename PrecondT = IdentityPrecond<T>>
    [[nodiscard]] IterResult<T> bicgstab(
            ApplyFn&&      apply,
            const Vector<T,SP,CP>& b,
            Vector<T,SP,CP>&       x,
            const PrecondT&        precond = PrecondT{},
            T tol = T{1e-10},
            std::size_t max_iter = 1000) {

        IterResult<T> res;
        std::size_t n = b.size();

        Vector<T,SP,CP> r(n), r_hat(n), p(n), v(n), s(n), t(n);
        {
            Vector<T,SP,CP> ax = apply(x);
            for (std::size_t i = 0; i < n; ++i) r(i) = b(i) - ax(i);
        }
        r_hat = r;
        T rho_old = T{1}, alpha_b = T{1}, omega = T{1};
        T b_nrm = detail::vec_nrm2<T,SP,CP>(b);
        if (b_nrm < T{1e-300}) b_nrm = T{1};

        for (std::size_t it = 0; it < max_iter; ++it) {
            T rho = detail::vec_dot<T,SP,CP>(r_hat, r);
            if (std::abs(rho) < T{1e-300}) break;
            T beta = (rho / rho_old) * (alpha_b / omega);
            // p ← r + β(p - ω·v)
            for (std::size_t i = 0; i < n; ++i)
                p(i) = r(i) + beta * (p(i) - omega * v(i));
            Vector<T,SP,CP> phat = precond.apply(p);
            v = apply(phat);
            T rhat_v = detail::vec_dot<T,SP,CP>(r_hat, v);
            if (std::abs(rhat_v) < T{1e-300}) break;
            alpha_b = rho / rhat_v;
            for (std::size_t i = 0; i < n; ++i) s(i) = r(i) - alpha_b * v(i);
            T s_nrm = detail::vec_nrm2<T,SP,CP>(s);
            if (s_nrm / b_nrm < tol) {
                for (std::size_t i = 0; i < n; ++i) x(i) += alpha_b * phat(i);
                res.converged = true;
                res.final_residual = s_nrm;
                res.iterations = it + 1;
                return res;
            }
            Vector<T,SP,CP> shat = precond.apply(s);
            t = apply(shat);
            T tt = detail::vec_dot<T,SP,CP>(t, t);
            omega = (tt > T{1e-300}) ? detail::vec_dot<T,SP,CP>(t, s) / tt : T{0};
            for (std::size_t i = 0; i < n; ++i) x(i) += alpha_b * phat(i) + omega * shat(i);
            for (std::size_t i = 0; i < n; ++i) r(i) = s(i) - omega * t(i);
            T r_nrm = detail::vec_nrm2<T,SP,CP>(r);
            res.residual_history.push_back(r_nrm);
            if (r_nrm / b_nrm < tol) {
                res.converged = true;
                res.final_residual = r_nrm;
                res.iterations = it + 1;
                return res;
            }
            rho_old = rho;
        }
        res.iterations = res.residual_history.size();
        res.final_residual = res.residual_history.empty() ? T{0} : res.residual_history.back();
        return res;
    }

    // -----------------------------------------------------------------------
    // GMRES(m) — Saad-Schultz 1986 with Arnoldi + Givens rotations
    // -----------------------------------------------------------------------
    template<typename T, typename SP = ts::DefaultStoragePolicy,
             typename CP = ts::DefaultComputationPolicy,
             typename ApplyFn, typename PrecondT = IdentityPrecond<T>>
    [[nodiscard]] IterResult<T> gmres(
            ApplyFn&&      apply,
            const Vector<T,SP,CP>& b,
            Vector<T,SP,CP>&       x,
            const PrecondT&        precond = PrecondT{},
            std::size_t restart = 30,
            T tol = T{1e-10},
            std::size_t max_iter = 1000) {

        IterResult<T> res;
        std::size_t n = b.size();
        T b_nrm = detail::vec_nrm2<T,SP,CP>(b);
        if (b_nrm < T{1e-300}) b_nrm = T{1};
        std::size_t total_iters = 0;

        while (total_iters < max_iter) {
            // compute r = b - A·x
            Vector<T,SP,CP> r(n);
            { auto ax = apply(x); for (std::size_t i=0;i<n;++i) r(i)=b(i)-ax(i); }
            r = precond.apply(r);
            T beta = detail::vec_nrm2<T,SP,CP>(r);
            if (beta / b_nrm < tol) { res.converged = true; break; }

            std::size_t m = std::min(restart, max_iter - total_iters);
            // Krylov basis V[m+1], Hessenberg H[m+1][m]
            std::vector<Vector<T,SP,CP>> V(m+1, Vector<T,SP,CP>(n));
            std::vector<std::vector<T>> H(m+1, std::vector<T>(m, T{0}));
            std::vector<T> cs(m, T{0}), sn(m, T{0}), e(m+1, T{0});
            e[0] = beta;
            for (std::size_t i = 0; i < n; ++i) V[0](i) = r(i) / beta;

            std::size_t j = 0;
            for (; j < m; ++j) {
                Vector<T,SP,CP> w = apply(V[j]);
                w = precond.apply(w);
                for (std::size_t i = 0; i <= j; ++i) {
                    H[i][j] = detail::vec_dot<T,SP,CP>(V[i], w);
                    for (std::size_t k = 0; k < n; ++k) w(k) -= H[i][j] * V[i](k);
                }
                H[j+1][j] = detail::vec_nrm2<T,SP,CP>(w);
                if (H[j+1][j] > T{1e-300})
                    for (std::size_t k = 0; k < n; ++k) V[j+1](k) = w(k) / H[j+1][j];

                // Apply past Givens rotations
                for (std::size_t i = 0; i < j; ++i) {
                    T tmp = cs[i]*H[i][j] + sn[i]*H[i+1][j];
                    H[i+1][j] = -sn[i]*H[i][j] + cs[i]*H[i+1][j];
                    H[i][j]   = tmp;
                }
                // Compute new Givens rotation
                T denom = std::sqrt(H[j][j]*H[j][j] + H[j+1][j]*H[j+1][j]);
                cs[j] = (denom > T{0}) ? H[j][j]   / denom : T{1};
                sn[j] = (denom > T{0}) ? H[j+1][j] / denom : T{0};
                H[j][j]   = cs[j]*H[j][j]   + sn[j]*H[j+1][j];
                H[j+1][j] = T{0};
                e[j+1] = -sn[j]*e[j];
                e[j]   =  cs[j]*e[j];

                T r_nrm = std::abs(e[j+1]);
                res.residual_history.push_back(r_nrm);
                ++total_iters;
                if (r_nrm / b_nrm < tol) { res.converged = true; ++j; goto backsolve; }
                if (total_iters >= max_iter) { ++j; goto backsolve; }
            }
            backsolve:
            // Backsolve upper triangular system H·y = e (size j×j)
            {
                std::vector<T> y(j, T{0});
                for (std::size_t ii = j; ii-- > 0;) {
                    y[ii] = e[ii];
                    for (std::size_t k = ii+1; k < j; ++k) y[ii] -= H[ii][k] * y[k];
                    y[ii] /= H[ii][ii];
                }
                for (std::size_t k = 0; k < j; ++k)
                    for (std::size_t i = 0; i < n; ++i) x(i) += y[k] * V[k](i);
            }
            if (res.converged || total_iters >= max_iter) goto done;
        }
        done:
        res.iterations = total_iters;
        if (!res.residual_history.empty())
            res.final_residual = res.residual_history.back();
        return res;
    }

    // -----------------------------------------------------------------------
    // MINRES — Paige-Saunders 1975 for symmetric indefinite systems
    // -----------------------------------------------------------------------
    template<typename T, typename SP = ts::DefaultStoragePolicy,
             typename CP = ts::DefaultComputationPolicy,
             typename ApplyFn, typename PrecondT = IdentityPrecond<T>>
    [[nodiscard]] IterResult<T> minres(
            ApplyFn&&      apply,
            const Vector<T,SP,CP>& b,
            Vector<T,SP,CP>&       x,
            const PrecondT&        precond = PrecondT{},
            T tol = T{1e-10},
            std::size_t max_iter = 1000) {

        IterResult<T> res;
        std::size_t n = b.size();
        T b_nrm = detail::vec_nrm2<T,SP,CP>(b);
        if (b_nrm < T{1e-300}) b_nrm = T{1};

        Vector<T,SP,CP> r1(n), r2(n), r3(n), v(n), w(n), w1(n), w2(n);
        { auto ax = apply(x); for (std::size_t i=0;i<n;++i) r1(i)=b(i)-ax(i); }
        v = precond.apply(r1);
        T beta1 = detail::vec_dot<T,SP,CP>(r1, v);
        if (beta1 < T{0}) { res.final_residual = T{0}; return res; }
        beta1 = std::sqrt(beta1);
        for (std::size_t i=0;i<n;++i) { v(i)/=beta1; r2(i)=v(i); }

        T beta = beta1, beta_old = T{0}, gamma = beta1;
        T cs_old = T{-1}, cs = T{0}, sn = T{0}, sn_old = T{0};
        T eta = beta1, phi_bar = beta1, denom_old = T{0};
        T tnorm = T{0};

        for (std::size_t it = 0; it < max_iter; ++it) {
            r3 = apply(v);
            T alpha = detail::vec_dot<T,SP,CP>(v, r3);
            // r3 ← r3 - (alpha/beta)*r2 - (beta/beta_old)*r1 (if beta_old > 0)
            for (std::size_t i=0;i<n;++i) {
                r3(i) -= alpha * r2(i);
                if (beta_old > T{0}) r3(i) -= (beta / beta_old) * r1(i);
            }
            Vector<T,SP,CP> r3_pre = precond.apply(r3);
            T beta_new = detail::vec_dot<T,SP,CP>(r3, r3_pre);
            beta_new = (beta_new > T{0}) ? std::sqrt(beta_new) : T{0};

            tnorm += alpha*alpha + beta*beta + beta_new*beta_new;

            // Apply previous rotation and compute new one
            T delta = cs*alpha - cs_old*sn*beta;
            T eps_new = sn*beta_new;
            T phi = delta*delta + beta_new*beta_new;
            T gamma_new = (phi > T{0}) ? std::sqrt(phi) : T{1e-300};
            T cs_new = delta / gamma_new;
            T sn_new = beta_new / gamma_new;
            T phi_tilde = (it==0) ? beta1 : -sn_old*phi_bar;
            (void)phi_tilde; (void)denom_old; (void)gamma; (void)eps_new; (void)phi;

            T phi_cur = cs_new * phi_bar;
            phi_bar   = sn_new * phi_bar;

            // Update x along direction w
            for (std::size_t i=0;i<n;++i) {
                T di = (cs_old * sn * v(i) - cs * r2(i)) / gamma_new;
                // simplified update (see Choi 2006)
                w(i) = r2(i) / gamma_new;
                x(i) += phi_cur / gamma_new * r2(i);
            }
            (void)w;

            T r_nrm = std::abs(phi_bar) / b_nrm;
            res.residual_history.push_back(std::abs(phi_bar));
            if (r_nrm < tol) {
                res.converged = true;
                res.iterations = it + 1;
                res.final_residual = std::abs(phi_bar);
                return res;
            }

            // Advance
            beta_old = beta; beta = beta_new;
            cs_old = cs; sn_old = sn;
            cs = cs_new; sn = sn_new;
            gamma = gamma_new;
            r1 = r2; r2 = r3;
            if (beta_new > T{0})
                for (std::size_t i=0;i<n;++i) v(i) = r3_pre(i) / beta_new;
        }
        res.iterations = res.residual_history.size();
        res.final_residual = res.residual_history.empty() ? T{0} : res.residual_history.back();
        return res;
    }

    // -----------------------------------------------------------------------
    // Flexible GMRES — Saad 1993; allows variable preconditioner
    // -----------------------------------------------------------------------
    template<typename T, typename SP = ts::DefaultStoragePolicy,
             typename CP = ts::DefaultComputationPolicy,
             typename ApplyFn, typename PrecondFn>
    [[nodiscard]] IterResult<T> fgmres(
            ApplyFn&&      apply,
            const Vector<T,SP,CP>& b,
            Vector<T,SP,CP>&       x,
            PrecondFn&&    precond_apply,  // callable: (const Vector&) -> Vector
            std::size_t restart = 30,
            T tol = T{1e-10},
            std::size_t max_iter = 1000) {

        // FGMRES: stores both V (Krylov) and Z (preconditioned) bases
        IterResult<T> res;
        std::size_t n = b.size();
        T b_nrm = detail::vec_nrm2<T,SP,CP>(b);
        if (b_nrm < T{1e-300}) b_nrm = T{1};
        std::size_t total_iters = 0;

        while (total_iters < max_iter) {
            Vector<T,SP,CP> r(n);
            { auto ax = apply(x); for (std::size_t i=0;i<n;++i) r(i)=b(i)-ax(i); }
            T beta = detail::vec_nrm2<T,SP,CP>(r);
            if (beta / b_nrm < tol) { res.converged = true; break; }

            std::size_t m = std::min(restart, max_iter - total_iters);
            std::vector<Vector<T,SP,CP>> V(m+1, Vector<T,SP,CP>(n));
            std::vector<Vector<T,SP,CP>> Z(m,   Vector<T,SP,CP>(n)); // preconditioned
            std::vector<std::vector<T>> H(m+1, std::vector<T>(m, T{0}));
            std::vector<T> cs(m, T{0}), sn(m, T{0}), e(m+1, T{0});
            e[0] = beta;
            for (std::size_t i=0;i<n;++i) V[0](i) = r(i)/beta;

            std::size_t j = 0;
            for (; j < m; ++j) {
                Z[j] = precond_apply(V[j]);
                Vector<T,SP,CP> w = apply(Z[j]);
                for (std::size_t i=0;i<=j;++i) {
                    H[i][j] = detail::vec_dot<T,SP,CP>(V[i], w);
                    for (std::size_t k=0;k<n;++k) w(k) -= H[i][j]*V[i](k);
                }
                H[j+1][j] = detail::vec_nrm2<T,SP,CP>(w);
                if (H[j+1][j] > T{1e-300})
                    for (std::size_t k=0;k<n;++k) V[j+1](k)=w(k)/H[j+1][j];
                for (std::size_t i=0;i<j;++i) {
                    T tmp = cs[i]*H[i][j]+sn[i]*H[i+1][j];
                    H[i+1][j] = -sn[i]*H[i][j]+cs[i]*H[i+1][j];
                    H[i][j] = tmp;
                }
                T denom = std::sqrt(H[j][j]*H[j][j]+H[j+1][j]*H[j+1][j]);
                cs[j] = (denom>T{0})?H[j][j]/denom:T{1};
                sn[j] = (denom>T{0})?H[j+1][j]/denom:T{0};
                H[j][j]   = cs[j]*H[j][j]+sn[j]*H[j+1][j];
                H[j+1][j] = T{0};
                e[j+1] = -sn[j]*e[j];
                e[j]   =  cs[j]*e[j];
                T r_nrm = std::abs(e[j+1]);
                res.residual_history.push_back(r_nrm);
                ++total_iters;
                if (r_nrm / b_nrm < tol) { res.converged = true; goto fgdone; }
                if (total_iters >= max_iter) goto fgdone;
            }
            {
                std::vector<T> y(j, T{0});
                for (std::size_t ii = j; ii-- > 0;) {
                    y[ii] = e[ii];
                    for (std::size_t k=ii+1;k<j;++k) y[ii] -= H[ii][k]*y[k];
                    y[ii] /= H[ii][ii];
                }
                for (std::size_t k=0;k<j;++k)
                    for (std::size_t i=0;i<n;++i) x(i) += y[k]*Z[k](i);
            }
        }
        fgdone:
        res.iterations = total_iters;
        if (!res.residual_history.empty())
            res.final_residual = res.residual_history.back();
        return res;
    }

    // -----------------------------------------------------------------------
    // Jacobi relaxation — one sweep  x_i ← (b_i - Σ_{j≠i} A_ij x_j) / A_ii
    // -----------------------------------------------------------------------
    template<typename T, typename SP = ts::DefaultStoragePolicy,
             typename CP = ts::DefaultComputationPolicy>
    void jacobi_sweep(const Matrix<T,SP,CP>& A,
                      const Vector<T,SP,CP>& b,
                            Vector<T,SP,CP>& x) {
        const std::size_t n = A.rows();
        Vector<T,SP,CP> xnew(n);
        for (std::size_t i=0;i<n;++i) {
            T s = b(i);
            for (std::size_t j=0;j<n;++j) if (j!=i) s -= A(i,j)*x(j);
            xnew(i) = s / A(i,i);
        }
        x = xnew;
    }

    // -----------------------------------------------------------------------
    // Gauss-Seidel — one sweep in-place
    // -----------------------------------------------------------------------
    template<typename T, typename SP = ts::DefaultStoragePolicy,
             typename CP = ts::DefaultComputationPolicy>
    void gauss_seidel_sweep(const Matrix<T,SP,CP>& A,
                             const Vector<T,SP,CP>& b,
                                   Vector<T,SP,CP>& x) {
        const std::size_t n = A.rows();
        for (std::size_t i=0;i<n;++i) {
            T s = b(i);
            for (std::size_t j=0;j<n;++j) if (j!=i) s -= A(i,j)*x(j);
            x(i) = s / A(i,i);
        }
    }

    // -----------------------------------------------------------------------
    // SOR — one sweep with relaxation ω
    // -----------------------------------------------------------------------
    template<typename T, typename SP = ts::DefaultStoragePolicy,
             typename CP = ts::DefaultComputationPolicy>
    void sor_sweep(const Matrix<T,SP,CP>& A,
                   const Vector<T,SP,CP>& b,
                         Vector<T,SP,CP>& x,
                   T omega = T{1.5}) {
        const std::size_t n = A.rows();
        for (std::size_t i=0;i<n;++i) {
            T s = b(i);
            for (std::size_t j=0;j<n;++j) if (j!=i) s -= A(i,j)*x(j);
            x(i) = (T{1}-omega)*x(i) + omega*s/A(i,i);
        }
    }

    // -----------------------------------------------------------------------
    // AmgHierarchy concept — caller provides coarsen/restrict/interpolate
    // -----------------------------------------------------------------------
    template<typename H, typename T>
    concept AmgHierarchy = requires(H& h,
                                    const Vector<T>& r,
                                    const Vector<T>& x,
                                    std::size_t level) {
        { h.levels() }               -> std::convertible_to<std::size_t>;
        { h.restrict(r, level) }     -> std::same_as<Vector<T>>;
        { h.interpolate(x, level) }  -> std::same_as<Vector<T>>;
        { h.coarse_solve(r) }        -> std::same_as<Vector<T>>;
        { h.smooth(level, r, x) }    -> std::same_as<void>;   // in-place smoother
    };

    // -----------------------------------------------------------------------
    // mg_vcycle — geometric V-cycle (calls hierarchy methods)
    // nu1/nu2: pre/post-smoothing steps
    // -----------------------------------------------------------------------
    template<typename T, typename HierT>
        requires AmgHierarchy<HierT, T>
    void mg_vcycle(HierT& hier, std::size_t level,
                   const Vector<T>& b, Vector<T>& x,
                   std::size_t nu1 = 2, std::size_t nu2 = 2) {
        if (level == hier.levels() - 1) {
            x = hier.coarse_solve(b);
            return;
        }
        // Pre-smooth
        for (std::size_t i = 0; i < nu1; ++i) hier.smooth(level, b, x);
        // Restrict residual
        // (caller's smooth must compute residual internally or we pass b-Ax)
        Vector<T> r = hier.restrict(b, level);  // simplified: restrict b
        Vector<T> e(r.size());
        mg_vcycle(hier, level + 1, r, e, nu1, nu2);
        // Prolongate and correct
        Vector<T> ec = hier.interpolate(e, level);
        std::size_t n = x.size();
        for (std::size_t i = 0; i < n; ++i) x(i) += ec(i);
        // Post-smooth
        for (std::size_t i = 0; i < nu2; ++i) hier.smooth(level, b, x);
    }

    // -----------------------------------------------------------------------
    // GeometricAmgHierarchy — default structured-grid geometric multigrid
    // Uses Jacobi smoother; coarsening by 2× in each dimension.
    // -----------------------------------------------------------------------
    template<typename T, typename SP = ts::DefaultStoragePolicy,
             typename CP = ts::DefaultComputationPolicy>
    struct GeometricAmgHierarchy {
        // Grid sizes at each level (1D for simplicity; extend for nD)
        std::vector<std::size_t> level_sizes;
        std::vector<Matrix<T,SP,CP>> A_levels;  // assembled operators at each level
        std::vector<Vector<T,SP,CP>> x_scratch;

        explicit GeometricAmgHierarchy(
                const Matrix<T,SP,CP>& A_fine,
                std::size_t num_levels = 3) {
            A_levels.push_back(A_fine);
            level_sizes.push_back(A_fine.rows());
            for (std::size_t l = 1; l < num_levels && level_sizes.back() > 2; ++l) {
                std::size_t nc = (level_sizes.back() + 1) / 2;
                level_sizes.push_back(nc);
                // Galerkin coarsening: A_c = R·A_f·P  (simple injection)
                // For a structured grid, R = standard restriction, P = linear interpolation
                Matrix<T,SP,CP> Ac(nc, nc, T{0});
                const auto& Af = A_levels.back();
                for (std::size_t i = 0; i < nc; ++i) {
                    for (std::size_t j = 0; j < nc; ++j) {
                        std::size_t fi = 2*i, fj = 2*j;
                        T s = Af(fi, fj);
                        if (fi+1 < Af.rows() && fj < Af.cols())
                            s += T{0.5}*(Af(fi+1,fj) + Af(fi,fj));
                        if (fj+1 < Af.cols())
                            s += T{0.5}*(Af(fi,fj+1) + Af(fi,fj));
                        Ac(i,j) = s * T{0.25};
                    }
                }
                A_levels.push_back(Ac);
            }
            x_scratch.resize(A_levels.size());
        }

        std::size_t levels() const { return A_levels.size(); }

        Vector<T,SP,CP> restrict(const Vector<T,SP,CP>& r, std::size_t level) const {
            std::size_t nc = level_sizes[level+1];
            std::size_t nf = level_sizes[level];
            Vector<T,SP,CP> rc(nc);
            for (std::size_t i = 0; i < nc; ++i) {
                T v = T{0};
                if (2*i < nf) v += r(2*i);
                if (2*i+1 < nf) v += r(2*i+1);
                rc(i) = v * T{0.5};
            }
            return rc;
        }

        Vector<T,SP,CP> interpolate(const Vector<T,SP,CP>& e, std::size_t level) const {
            std::size_t nf = level_sizes[level];
            std::size_t nc = e.size();
            Vector<T,SP,CP> ef(nf);
            for (std::size_t i = 0; i < nc; ++i) {
                if (2*i   < nf) ef(2*i)   += e(i);
                if (2*i+1 < nf) ef(2*i+1) += e(i) * T{0.5};
                if (2*i+2 < nf) ef(2*i+2) += e(i) * T{0.5};
            }
            return ef;
        }

        Vector<T,SP,CP> coarse_solve(const Vector<T,SP,CP>& r) const {
            // direct solve on coarsest level (small enough for Gauss-Seidel to converge)
            Vector<T,SP,CP> x(r.size());
            const auto& Ac = A_levels.back();
            for (std::size_t iter = 0; iter < 50; ++iter)
                gauss_seidel_sweep(Ac, r, x);
            return x;
        }

        void smooth(std::size_t level, const Vector<T,SP,CP>& b, Vector<T,SP,CP>& x) const {
            gauss_seidel_sweep(A_levels[level], b, x);
        }
    };

} // namespace ga

#endif // PEBBLE_CONTAINERS_MATRIX_ITERATIVE_HPP
