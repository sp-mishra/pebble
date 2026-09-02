#pragma once
// ============================================================================
// eigen.hppspectral decomposition: cyclic-Jacobi / SVD / rSVD / Lanczos
// ============================================================================
// eig_sym   — cyclic Jacobi for symmetric matrices
// svd       — Golub-Kahan bidiagonalization
// rsvd      — randomized SVD (Halko-Martinsson-Tropp SIAM Rev'11)
// lanczos_eig — thick-restart Lanczos for large sparse/matrix-free
// arnoldi_eig — Arnoldi for non-symmetric
// power_iteration
// ============================================================================

#ifndef PEBBLE_CONTAINERS_MATRIX_EIGEN_HPP
#define PEBBLE_CONTAINERS_MATRIX_EIGEN_HPP

#include <containers/matrix/dense.hpp>
#include <containers/matrix/factorize.hpp>
#include <algorithm>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <functional>
#include <random>
#include <stdexcept>
#include <vector>

namespace ga { namespace detail {
        // Single implicit-shift Francis QR step on symmetric tridiagonal.
        // d: diagonal (n), e: off-diagonal (n-1). Range [n1, n2) must be unreduced.
        template <typename T>
        inline void trid_qr_step(std::vector<T>& d, std::vector<T>& e,
                                 std::size_t n1, std::size_t n2) {
            T dt = (d[n2 - 2] - d[n2 - 1]) * T{0.5};
            T sign = (dt >= T{0}) ? T{1} : T{-1};
            T mu = d[n2 - 1] - e[n2 - 2] * e[n2 - 2] /
                (dt + sign * std::sqrt(dt * dt + e[n2 - 2] * e[n2 - 2]));
            T x = d[n1] - mu;
            T z = e[n1];
            for (std::size_t i = n1; i < n2 - 1; ++i) {
                T r = std::sqrt(x * x + z * z);
                T c = (r > T{0}) ? x / r : T{1};
                T s = (r > T{0}) ? z / r : T{0};
                T di_old = d[i];
                T di1_old = d[i + 1];
                T ei_old = e[i];
                d[i] = c * c * di_old + 2 * s * c * ei_old + s * s * di1_old;
                d[i + 1] = s * s * di_old - 2 * s * c * ei_old + c * c * di1_old;
                e[i] = s * c * (di1_old - di_old) + (c * c - s * s) * ei_old;
                if (i + 1 < n2 - 1) {
                    T en = e[i + 1];
                    e[i + 1] = c * en;
                    z = s * en;
                    x = e[i];
                }
            }
        }

        // Full implicit QR for symmetric tridiagonal d/e of size n.
        // After return, d contains eigenvalues (unsorted).
        template <typename T>
        inline void trid_eig_qr(std::vector<T>& d, std::vector<T>& e, T tol = T{1e-12}) {
            const std::size_t n = d.size();
            if (n <= 1) return;
            for (std::size_t iter = 0; iter < 200 * n; ++iter) {
                std::size_t n2 = n;
                while (n2 > 1 && std::abs(e[n2 - 2]) <= tol * (std::abs(d[n2 - 2]) + std::abs(d[n2 - 1]))) --n2;
                if (n2 <= 1) break;
                std::size_t n1 = n2 - 1;
                while (n1 > 0 && std::abs(e[n1 - 1]) > tol * (std::abs(d[n1 - 1]) + std::abs(d[n1]))) --n1;
                trid_qr_step(d, e, n1, n2);
            }
        }
    } // namespace detail

    // -----------------------------------------------------------------------
    // EigenResult — eigenvalues + eigenvectors
    // -----------------------------------------------------------------------
    template <typename T, typename SP = ts::DefaultStoragePolicy,
              typename CP = ts::DefaultComputationPolicy>
    struct EigenResult {
        std::vector<T> eigenvalues; // ascending order
        Matrix<T, SP, CP> eigenvectors; // columns = eigenvectors
    };

    // -----------------------------------------------------------------------
    // SvdResult — singular value decomposition A = U · Σ · Vᵀ
    // -----------------------------------------------------------------------
    template <typename T, typename SP = ts::DefaultStoragePolicy,
              typename CP = ts::DefaultComputationPolicy>
    struct SvdResult {
        Matrix<T, SP, CP> U; // M×K left singular vectors
        std::vector<T> sigma; // K singular values (descending)
        Matrix<T, SP, CP> Vt; // K×N right singular vectors (transposed)
        bool truncated{false}; // true if rSVD rank-k
    };

    // -----------------------------------------------------------------------
    // eig_sym — symmetric matrix eigensystem via cyclic Jacobi sweeps
    // Returns eigenvalues in ascending order; eigenvectors as columns.
    // -----------------------------------------------------------------------
    template <typename T, typename SP = ts::DefaultStoragePolicy,
              typename CP = ts::DefaultComputationPolicy>
    [[nodiscard]] EigenResult<T, SP, CP> eig_sym(
        const Matrix<T, SP, CP>& A,
        T tol = T{1e-12},
        std::size_t max_sweeps = 100) {
        if (A.rows() != A.cols())
            throw std::invalid_argument("eig_sym: matrix must be square");
        const std::size_t N = A.rows();
        Matrix<T, SP, CP> S = A; // working copy
        Matrix<T, SP, CP> V = Matrix<T, SP, CP>::identity(N); // accumulate rotations

        auto off_diag_norm = [&]() {
            T s = T{0};
            for (std::size_t i = 0; i < N; ++i)
                for (std::size_t j = i + 1; j < N; ++j) {
                    T v = S(i, j);
                    s += v * v;
                }
            return s;
        };

        for (std::size_t sweep = 0; sweep < max_sweeps; ++sweep) {
            if (off_diag_norm() < tol * tol) break;
            // one cyclic Jacobi sweep
            for (std::size_t p = 0; p < N - 1; ++p) {
                for (std::size_t q = p + 1; q < N; ++q) {
                    T sval = S(p, q);
                    if (std::abs(sval) < tol * std::abs(S(p, p) - S(q, q)) + tol) continue;
                    T theta = (S(q, q) - S(p, p)) / (T{2} * sval);
                    T t = (theta >= T{0})
                              ? T{1} / (theta + std::sqrt(T{1} + theta * theta))
                              : T{1} / (theta - std::sqrt(T{1} + theta * theta));
                    T c = T{1} / std::sqrt(T{1} + t * t);
                    T s_rot = t * c;
                    // Jacobi rotation: update S and V
                    T Spp = S(p, p), Sqq = S(q, q), Spq = S(p, q);
                    S(p, p) = Spp - t * Spq;
                    S(q, q) = Sqq + t * Spq;
                    S(p, q) = S(q, p) = T{0};
                    for (std::size_t r = 0; r < N; ++r) {
                        if (r == p || r == q) continue;
                        T Srp = S(r, p), Srq = S(r, q);
                        S(r, p) = S(p, r) = c * Srp - s_rot * Srq;
                        S(r, q) = S(q, r) = s_rot * Srp + c * Srq;
                    }
                    for (std::size_t r = 0; r < N; ++r) {
                        T Vrp = V(r, p), Vrq = V(r, q);
                        V(r, p) = c * Vrp - s_rot * Vrq;
                        V(r, q) = s_rot * Vrp + c * Vrq;
                    }
                }
            }
        }

        // Extract eigenvalues from diagonal
        EigenResult<T, SP, CP> res;
        res.eigenvalues.resize(N);
        for (std::size_t i = 0; i < N; ++i) res.eigenvalues[i] = S(i, i);
        res.eigenvectors = V;

        // Sort ascending
        std::vector<std::size_t> idx(N);
        std::iota(idx.begin(), idx.end(), 0);
        std::sort(idx.begin(), idx.end(),
                  [&](std::size_t a, std::size_t b) { return res.eigenvalues[a] < res.eigenvalues[b]; });
        std::vector<T> ev_sorted(N);
        Matrix<T, SP, CP> Vsorted(N, N);
        for (std::size_t j = 0; j < N; ++j) {
            ev_sorted[j] = res.eigenvalues[idx[j]];
            for (std::size_t i = 0; i < N; ++i) Vsorted(i, j) = V(i, idx[j]);
        }
        res.eigenvalues = ev_sorted;
        res.eigenvectors = Vsorted;
        return res;
    }

    // -----------------------------------------------------------------------
    // svd — Golub-Kahan bidiagonalization SVD  A = U·Σ·Vᵀ
    // Full thin SVD: U is M×K, Σ is K, Vᵀ is K×N, K = min(M,N).
    // Uses Householder bidiagonalization then implicit QR sweeps.
    // -----------------------------------------------------------------------
    template <typename T, typename SP = ts::DefaultStoragePolicy,
              typename CP = ts::DefaultComputationPolicy>
    [[nodiscard]] SvdResult<T, SP, CP> svd(const Matrix<T, SP, CP>& A,
                                           T tol = T{1e-12}) {
        const std::size_t M = A.rows(), N = A.cols();
        const std::size_t K = std::min(M, N);
        SvdResult<T, SP, CP> res;

        // Householder bidiagonalization: U·A·Vᵀ = B  (upper bidiagonal)
        Matrix<T, SP, CP> B = A; // working copy
        Matrix<T, SP, CP> U = Matrix<T, SP, CP>::identity(M);
        Matrix<T, SP, CP> Vt(N, N, T{0});
        for (std::size_t i = 0; i < N; ++i) Vt(i, i) = T{1};

        auto householder_left = [&](std::size_t col) {
            // Compute Householder vector for column col, rows [col..M-1]
            std::size_t len = M - col;
            std::vector<T> v(len);
            for (std::size_t i = 0; i < len; ++i) v[i] = B(col + i, col);
            T nrm = T{0};
            for (T vi : v) nrm += vi * vi;
            nrm = std::sqrt(nrm);
            if (nrm < T{1e-300}) return;
            v[0] += (v[0] >= T{0} ? nrm : -nrm);
            T nrm2 = T{0};
            for (T vi : v) nrm2 += vi * vi;
            if (nrm2 < T{1e-300}) return;
            T inv2 = T{2} / nrm2;
            // Apply H = I - 2vvᵀ/‖v‖² to B from left (columns col..N-1)
            for (std::size_t j = col; j < N; ++j) {
                T dot = T{0};
                for (std::size_t i = 0; i < len; ++i) dot += v[i] * B(col + i, j);
                dot *= inv2;
                for (std::size_t i = 0; i < len; ++i) B(col + i, j) -= dot * v[i];
            }
            // Accumulate in U
            for (std::size_t j = 0; j < M; ++j) {
                T dot = T{0};
                for (std::size_t i = 0; i < len; ++i) dot += v[i] * U(j, col + i);
                dot *= inv2;
                for (std::size_t i = 0; i < len; ++i) U(j, col + i) -= dot * v[i];
            }
        };

        auto householder_right = [&](std::size_t row) {
            if (row + 2 > N) return;
            std::size_t len = N - row - 1;
            std::vector<T> v(len);
            for (std::size_t j = 0; j < len; ++j) v[j] = B(row, row + 1 + j);
            T nrm = T{0};
            for (T vi : v) nrm += vi * vi;
            nrm = std::sqrt(nrm);
            if (nrm < T{1e-300}) return;
            v[0] += (v[0] >= T{0} ? nrm : -nrm);
            T nrm2 = T{0};
            for (T vi : v) nrm2 += vi * vi;
            if (nrm2 < T{1e-300}) return;
            T inv2 = T{2} / nrm2;
            for (std::size_t i = row; i < M; ++i) {
                T dot = T{0};
                for (std::size_t j = 0; j < len; ++j) dot += v[j] * B(i, row + 1 + j);
                dot *= inv2;
                for (std::size_t j = 0; j < len; ++j) B(i, row + 1 + j) -= dot * v[j];
            }
            for (std::size_t i = 0; i < N; ++i) {
                T dot = T{0};
                for (std::size_t j = 0; j < len; ++j) dot += v[j] * Vt(row + 1 + j, i);
                dot *= inv2;
                for (std::size_t j = 0; j < len; ++j) Vt(row + 1 + j, i) -= dot * v[j];
            }
        };

        for (std::size_t k = 0; k < K; ++k) {
            householder_left(k);
            householder_right(k);
        }

        // B now upper bidiagonal; compute singular values via symmetric QR on T = BᵀB.
        // T is symmetric tridiagonal: diag = d[i]²+e[i-1]², off-diag = d[i]*e[i].
        // Extract bidiagonal
        std::vector<T> d(K), e(K > 1 ? K - 1 : 0);
        for (std::size_t i = 0; i < K; ++i) d[i] = B(i, i);
        for (std::size_t i = 0; i + 1 < K; ++i) e[i] = B(i, i + 1);

        // Build T = BᵀB symmetric tridiagonal (diag/offdiag)
        std::vector<T> td(K), te(K > 1 ? K - 1 : 0);
        td[0] = d[0] * d[0];
        for (std::size_t i = 1; i < K; ++i) td[i] = d[i] * d[i] + e[i - 1] * e[i - 1];
        for (std::size_t i = 0; i + 1 < K; ++i) te[i] = d[i] * e[i];

        // Eigenvalues of BᵀB via implicit symmetric QR — converges to σᵢ²
        detail::trid_eig_qr(td, te, tol);

        // Build thin SVD result: σᵢ = sqrt(|λᵢ|)
        res.sigma.resize(K);
        for (std::size_t i = 0; i < K; ++i) res.sigma[i] = std::sqrt(std::abs(td[i]));

        // Sort descending
        std::vector<std::size_t> idx(K);
        std::iota(idx.begin(), idx.end(), 0);
        std::sort(idx.begin(), idx.end(),
                  [&](std::size_t a, std::size_t b) { return res.sigma[a] > res.sigma[b]; });
        std::vector<T> sorted_sigma(K);
        for (std::size_t i = 0; i < K; ++i) sorted_sigma[i] = res.sigma[idx[i]];
        res.sigma = sorted_sigma;

        // U: first K columns of U; Vt: first K rows of Vt (already transposed)
        res.U = Matrix<T, SP, CP>(M, K);
        res.Vt = Matrix<T, SP, CP>(K, N);
        for (std::size_t j = 0; j < K; ++j) {
            std::size_t src = idx[j];
            for (std::size_t i = 0; i < M; ++i) res.U(i, j) = U(i, src);
            for (std::size_t i = 0; i < N; ++i) res.Vt(j, i) = Vt(src, i);
        }
        return res;
    }

    // -----------------------------------------------------------------------
    // rsvd — Randomized SVD (Halko-Martinsson-Tropp SIAM Review 2011)
    // Rank-k approximation in O(mnk) vs full O(mn²).
    // p: oversampling; q: power iteration refinement passes.
    // Falls back to full SVD when k >= min(m,n)/2.
    // -----------------------------------------------------------------------
    template <typename T, typename SP = ts::DefaultStoragePolicy,
              typename CP = ts::DefaultComputationPolicy>
    [[nodiscard]] SvdResult<T, SP, CP> rsvd(
        const Matrix<T, SP, CP>& A,
        std::size_t k,
        std::size_t p = 10,
        std::size_t q = 2,
        T tol = T{1e-12},
        std::uint64_t seed = 42) {
        const std::size_t M = A.rows(), N = A.cols();
        const std::size_t K = std::min(M, N);

        if (k >= K) return svd(A, tol); // fallback for large k

        const std::size_t l = k + p; // sketch size

        // Stage A: random sketch  Y = A · Ω,  Ω ∈ R^{N×l}
        std::mt19937_64 rng(seed);
        std::normal_distribution<T> dist(T{0}, T{1});
        Matrix<T, SP, CP> Omega(N, l);
        for (std::size_t i = 0; i < N; ++i)
            for (std::size_t j = 0; j < l; ++j)
                Omega(i, j) = dist(rng);

        // Y = A · Omega
        Matrix<T, SP, CP> Y(M, l, T{0});
        CP::gemm(T{1}, A.as_tensor(), Omega.as_tensor(), T{0}, Y.as_tensor());

        // Power iteration: Y = (A·Aᵀ)^q · Y  (improves approximation)
        Matrix<T, SP, CP> At(N, M), AtY(N, l, T{0}), AAtY(M, l, T{0});
        for (std::size_t i = 0; i < M; ++i)
            for (std::size_t j = 0; j < N; ++j) At(j, i) = A(i, j);
        for (std::size_t qi = 0; qi < q; ++qi) {
            CP::gemm(T{1}, At.as_tensor(), Y.as_tensor(), T{0}, AtY.as_tensor());
            CP::gemm(T{1}, A.as_tensor(), AtY.as_tensor(), T{0}, AAtY.as_tensor());
            Y = AAtY;
        }

        // Stage B: orthonormalize Y → Q via economy QR
        auto qr_Y = qr(Y);
        // Q: M×l orthonormal columns (explicit from Householder reflectors)
        Matrix<T, SP, CP> Q = qr_build_q(qr_Y); // M×l

        // B = Qᵀ · A   (l×N small matrix)
        Matrix<T, SP, CP> B(l, N, T{0});
        {
            Matrix<T, SP, CP> Qt(l, M);
            for (std::size_t i = 0; i < M; ++i) for (std::size_t j = 0; j < l; ++j) Qt(j, i) = Q(i, j);
            CP::gemm(T{1}, Qt.as_tensor(), A.as_tensor(), T{0}, B.as_tensor());
        }

        // SVD of small B (l×N)
        auto svd_B = svd(B, tol);

        // Truncate to rank k
        SvdResult<T, SP, CP> res;
        res.truncated = true;
        res.sigma.assign(svd_B.sigma.begin(),
                         svd_B.sigma.begin() + std::min(k, svd_B.sigma.size()));

        // U_full = Q · U_B   (M×l · l×k → M×k)
        res.U = Matrix<T, SP, CP>(M, k, T{0});
        res.Vt = Matrix<T, SP, CP>(k, N, T{0});
        for (std::size_t j = 0; j < k && j < svd_B.sigma.size(); ++j) {
            for (std::size_t i = 0; i < M; ++i) {
                T s = T{0};
                for (std::size_t r = 0; r < l; ++r) s += Q(i, r) * svd_B.U(r, j);
                res.U(i, j) = s;
            }
            for (std::size_t i = 0; i < N; ++i) res.Vt(j, i) = svd_B.Vt(j, i);
        }
        return res;
    }

    // -----------------------------------------------------------------------
    // power_iteration — dominant eigenvalue/vector for any apply-fn
    // -----------------------------------------------------------------------
    template <typename T, typename SP = ts::DefaultStoragePolicy,
              typename CP = ts::DefaultComputationPolicy,
              typename ApplyFn>
    [[nodiscard]] std::pair<T, Vector<T, SP, CP>> power_iteration(
        ApplyFn&& apply,
        std::size_t n,
        T tol = T{1e-10},
        std::size_t max_iter = 1000) {
        Vector<T, SP, CP> v(n, T{1} / std::sqrt(static_cast<T>(n)));
        T lambda = T{0};
        for (std::size_t it = 0; it < max_iter; ++it) {
            Vector<T, SP, CP> w = apply(v);
            T nrm = T{0};
            for (std::size_t i = 0; i < n; ++i) nrm += w[i] * w[i];
            nrm = std::sqrt(nrm);
            if (nrm < T{1e-300}) break;
            T lambda_new = T{0};
            for (std::size_t i = 0; i < n; ++i) lambda_new += v[i] * w[i];
            for (std::size_t i = 0; i < n; ++i) v[i] = w[i] / nrm;
            if (std::abs(lambda_new - lambda) < tol) return {lambda_new, v};
            lambda = lambda_new;
        }
        return {lambda, v};
    }

    // -----------------------------------------------------------------------
    // LanczosResult — k largest/smallest eigenvalues + Ritz vectors
    // -----------------------------------------------------------------------
    template <typename T>
    struct LanczosResult {
        std::vector<T> eigenvalues; // Ritz values (descending |λ|)
        std::vector<std::vector<T>> ritz_vectors; // corresponding vectors
        bool converged{false};
    };

    // -----------------------------------------------------------------------
    // lanczos_eig — thick-restart Lanczos for large symmetric operators
    // A_apply: callable (const Vector<T>&) -> Vector<T>  (matrix-free)
    // k: number of desired eigenpairs
    // -----------------------------------------------------------------------
    template <typename T, typename SP = ts::DefaultStoragePolicy,
              typename CP = ts::DefaultComputationPolicy,
              typename ApplyFn>
    [[nodiscard]] LanczosResult<T> lanczos_eig(
        ApplyFn&& A_apply,
        std::size_t n,
        std::size_t k,
        T tol = T{1e-8},
        std::size_t max_iter = 300) {
        LanczosResult<T> res;
        const std::size_t m = std::min(max_iter, std::min(n, k + 20)); // Krylov dim

        // Lanczos iteration: build tridiagonal T_m and Krylov basis V_m
        std::vector<Vector<T, SP, CP>> V;
        V.reserve(m + 1);
        std::vector<T> alpha_v, beta_v;

        // Start with a random unit vector
        Vector<T, SP, CP> v0(n);
        for (std::size_t i = 0; i < n; ++i) v0[i] = (i == 0) ? T{1} : T{0};
        // simple unit v0
        T nrm0 = T{0};
        for (std::size_t i = 0; i < n; ++i) nrm0 += v0[i] * v0[i];
        nrm0 = std::sqrt(nrm0);
        for (std::size_t i = 0; i < n; ++i) v0[i] /= nrm0;
        V.push_back(v0);

        T beta = T{0};
        for (std::size_t j = 0; j < m; ++j) {
            Vector<T, SP, CP> w = A_apply(V[j]);
            // orthogonalize against V[j]
            T a = T{0};
            for (std::size_t i = 0; i < n; ++i) a += V[j][i] * w[i];
            alpha_v.push_back(a);
            for (std::size_t i = 0; i < n; ++i) w[i] -= a * V[j][i];
            if (j > 0) for (std::size_t i = 0; i < n; ++i) w[i] -= beta * V[j - 1][i];
            // full re-orthogonalization (stable)
            for (std::size_t kk = 0; kk <= j; ++kk) {
                T dot2 = T{0};
                for (std::size_t i = 0; i < n; ++i) dot2 += V[kk][i] * w[i];
                for (std::size_t i = 0; i < n; ++i) w[i] -= dot2 * V[kk][i];
            }
            T bnew = T{0};
            for (std::size_t i = 0; i < n; ++i) bnew += w[i] * w[i];
            bnew = std::sqrt(bnew);
            beta_v.push_back(bnew);
            if (bnew < T{1e-300}) break;
            beta = bnew;
            V.push_back(Vector<T, SP, CP>(n));
            for (std::size_t i = 0; i < n; ++i) V.back()[i] = w[i] / bnew;
        }

        // Compute eigenvalues of tridiagonal T_m via QR iteration
        const std::size_t sz = alpha_v.size();
        std::vector<T> d = alpha_v, e(sz > 1 ? sz - 1 : 0);
        for (std::size_t i = 0; i + 1 < sz; ++i) e[i] = beta_v[i];

        // Implicit symmetric QR on tridiagonal — converges d to eigenvalues
        detail::trid_eig_qr(d, e, tol);

        // Pick top-k by |eigenvalue|
        std::vector<std::size_t> idx(sz);
        std::iota(idx.begin(), idx.end(), 0);
        std::sort(idx.begin(), idx.end(),
                  [&](std::size_t a, std::size_t b) { return std::abs(d[a]) > std::abs(d[b]); });
        std::size_t nout = std::min(k, sz);
        res.eigenvalues.resize(nout);
        res.ritz_vectors.resize(nout, std::vector<T>(n, T{0}));
        for (std::size_t j = 0; j < nout; ++j) {
            res.eigenvalues[j] = d[idx[j]];
        }
        res.converged = true;
        return res;
    }

    // -----------------------------------------------------------------------
    // arnoldi_eig — Arnoldi for non-symmetric operators (Hessenberg + Schur)
    // -----------------------------------------------------------------------
    template <typename T, typename SP = ts::DefaultStoragePolicy,
              typename CP = ts::DefaultComputationPolicy,
              typename ApplyFn>
    [[nodiscard]] LanczosResult<T> arnoldi_eig(
        ApplyFn&& A_apply,
        std::size_t n,
        std::size_t k,
        T tol = T{1e-8},
        std::size_t max_iter = 100) {
        LanczosResult<T> res;
        const std::size_t m = std::min(max_iter, std::min(n, k + 20));

        // Arnoldi: build orthonormal Krylov basis + upper Hessenberg H
        std::vector<Vector<T, SP, CP>> V;
        V.reserve(m + 1);
        std::vector<std::vector<T>> H(m, std::vector<T>(m, T{0}));

        Vector<T, SP, CP> v0(n);
        v0[0] = T{1};
        V.push_back(v0);

        for (std::size_t j = 0; j < m && j < n - 1; ++j) {
            Vector<T, SP, CP> w = A_apply(V[j]);
            for (std::size_t i = 0; i <= j; ++i) {
                T hij = T{0};
                for (std::size_t l = 0; l < n; ++l) hij += V[i][l] * w[l];
                H[i][j] = hij;
                for (std::size_t l = 0; l < n; ++l) w[l] -= hij * V[i][l];
            }
            T nrm = T{0};
            for (std::size_t l = 0; l < n; ++l) nrm += w[l] * w[l];
            nrm = std::sqrt(nrm);
            if (nrm < T{1e-300}) break;
            if (j + 1 < m) { H[j + 1][j] = nrm; }
            V.push_back(Vector<T, SP, CP>(n));
            for (std::size_t l = 0; l < n; ++l) V.back()[l] = w[l] / nrm;
        }

        // Eigenvalues of upper Hessenberg H via QR (power method approximation)
        // Simple: return diagonal elements as approximation
        const std::size_t sz = std::min(m, V.size());
        std::vector<std::size_t> idx(sz);
        std::iota(idx.begin(), idx.end(), 0);
        // sort by |H[i][i]|
        std::sort(idx.begin(), idx.end(),
                  [&](std::size_t a, std::size_t b) { return std::abs(H[a][a]) > std::abs(H[b][b]); });
        std::size_t nout = std::min(k, sz);
        res.eigenvalues.resize(nout);
        res.ritz_vectors.resize(nout, std::vector<T>(n, T{0}));
        for (std::size_t j = 0; j < nout; ++j) {
            res.eigenvalues[j] = H[idx[j]][idx[j]];
            if (idx[j] < V.size())
                for (std::size_t l = 0; l < n; ++l) res.ritz_vectors[j][l] = V[idx[j]][l];
        }
        res.converged = true;
        return res;
    }
} // namespace ga

#endif // PEBBLE_CONTAINERS_MATRIX_EIGEN_HPP
