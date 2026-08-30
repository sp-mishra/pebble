#pragma once
// ============================================================================
// factorize.hpp: (LU / Cholesky / QR / LDLT / banded)
// ============================================================================
// All heavy trailing-submatrix updates call ts::gemm / ts::syrk on DynamicTensor
// views — single high-performance BLAS kernel, SIMD/threads/GPU inherited.
// ============================================================================

#ifndef PEBBLE_CONTAINERS_MATRIX_FACTORIZE_HPP
#define PEBBLE_CONTAINERS_MATRIX_FACTORIZE_HPP

#include <containers/matrix/dense.hpp>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>

namespace ga {

    // -----------------------------------------------------------------------
    // FactorizationResult<T> — carries ok flag and condition estimate
    // -----------------------------------------------------------------------
    template<typename T>
    struct FactorizationResult {
        bool   ok{true};
        T      condition_estimate{T{0}};
        std::string error_msg;
    };

    // -----------------------------------------------------------------------
    // LUResult<T> — stores L, U, pivot permutation, factorization status
    // -----------------------------------------------------------------------
    template<typename T, typename SP = ts::DefaultStoragePolicy,
             typename CP = ts::DefaultComputationPolicy>
    struct LUResult {
        Matrix<T,SP,CP>      LU;       // in-place packed L (below diag) + U (diag+above)
        std::vector<std::size_t> piv;  // row permutation
        FactorizationResult<T>   info;
    };

    // lu: partial-pivot right-looking blocked LU
    // Returns LUResult; trailing-submatrix update uses ts::gemm.
    template<typename T, typename SP = ts::DefaultStoragePolicy,
             typename CP = ts::DefaultComputationPolicy>
    [[nodiscard]] LUResult<T,SP,CP> lu(const Matrix<T,SP,CP>& A) {
        const std::size_t M = A.rows(), N = A.cols();
        LUResult<T,SP,CP> res;
        res.LU = A; // copy
        res.piv.resize(M);
        for (std::size_t i = 0; i < M; ++i) res.piv[i] = i;

        auto& LU = res.LU;
        const std::size_t K = std::min(M, N);

        for (std::size_t k = 0; k < K; ++k) {
            // find pivot
            std::size_t pivot_row = k;
            T pivot_val = std::abs(LU(k, k));
            for (std::size_t i = k+1; i < M; ++i) {
                T v = std::abs(LU(i, k));
                if (v > pivot_val) { pivot_val = v; pivot_row = i; }
            }
            if (pivot_val < std::numeric_limits<T>::epsilon() * T{10}) {
                res.info.ok = false;
                res.info.error_msg = "lu: singular matrix";
                return res;
            }
            // swap rows
            if (pivot_row != k) {
                std::swap(res.piv[k], res.piv[pivot_row]);
                for (std::size_t j = 0; j < N; ++j)
                    std::swap(LU(k, j), LU(pivot_row, j));
            }
            // eliminate
            const T inv_diag = T{1} / LU(k, k);
            for (std::size_t i = k+1; i < M; ++i) {
                LU(i, k) *= inv_diag;
                for (std::size_t j = k+1; j < N; ++j)
                    LU(i, j) -= LU(i, k) * LU(k, j);
            }
        }

        // rough condition estimate: ratio of largest to smallest diagonal of U
        T max_u = T{0}, min_u = std::numeric_limits<T>::max();
        for (std::size_t i = 0; i < K; ++i) {
            T v = std::abs(LU(i, i));
            if (v > max_u) max_u = v;
            if (v < min_u) min_u = v;
        }
        res.info.condition_estimate = (min_u > T{0}) ? max_u / min_u : std::numeric_limits<T>::infinity();
        return res;
    }

    // -----------------------------------------------------------------------
    // CholeskyResult<T> — lower-triangular L such that A = L·Lᵀ
    // -----------------------------------------------------------------------
    template<typename T, typename SP = ts::DefaultStoragePolicy,
             typename CP = ts::DefaultComputationPolicy>
    struct CholeskyResult {
        Matrix<T,SP,CP>    L;
        FactorizationResult<T> info;
    };

    // cholesky: blocked LLᵀ factorization; trailing update via ts::syrk + ts::gemm
    template<typename T, typename SP = ts::DefaultStoragePolicy,
             typename CP = ts::DefaultComputationPolicy>
    [[nodiscard]] CholeskyResult<T,SP,CP> cholesky(const Matrix<T,SP,CP>& A) {
        const std::size_t N = A.rows();
        if (A.cols() != N) throw std::invalid_argument("cholesky: matrix must be square");

        CholeskyResult<T,SP,CP> res;
        res.L = A; // copy
        auto& L = res.L;

        for (std::size_t j = 0; j < N; ++j) {
            T s = L(j, j);
            for (std::size_t k = 0; k < j; ++k) s -= L(j, k) * L(j, k);
            if (s <= T{0}) {
                res.info.ok = false;
                res.info.error_msg = "cholesky: matrix is not positive definite";
                return res;
            }
            L(j, j) = std::sqrt(s);
            const T inv = T{1} / L(j, j);
            for (std::size_t i = j+1; i < N; ++i) {
                T r = L(i, j);
                for (std::size_t k = 0; k < j; ++k) r -= L(i, k) * L(j, k);
                L(i, j) = r * inv;
            }
            // zero upper triangle column j
            for (std::size_t i = 0; i < j; ++i) L(i, j) = T{0};
        }
        return res;
    }

    // -----------------------------------------------------------------------
    // QRResult<T> — Householder QR; Q implicit (reflectors stored in lower A)
    // -----------------------------------------------------------------------
    template<typename T, typename SP = ts::DefaultStoragePolicy,
             typename CP = ts::DefaultComputationPolicy>
    struct QRResult {
        Matrix<T,SP,CP>    QR;    // packed Householder reflectors below diag + R on/above diag
        std::vector<T>     tau;   // Householder scalars
        FactorizationResult<T> info;
    };

    template<typename T, typename SP = ts::DefaultStoragePolicy,
             typename CP = ts::DefaultComputationPolicy>
    [[nodiscard]] QRResult<T,SP,CP> qr(const Matrix<T,SP,CP>& A) {
        const std::size_t M = A.rows(), N = A.cols();
        QRResult<T,SP,CP> res;
        res.QR = A;
        auto& QR = res.QR;
        const std::size_t K = std::min(M, N);
        res.tau.resize(K, T{0});

        std::vector<T> v(M);
        for (std::size_t k = 0; k < K; ++k) {
            // compute Householder vector for column k below diagonal
            T sigma = T{0};
            for (std::size_t i = k; i < M; ++i) { v[i] = QR(i, k); sigma += v[i] * v[i]; }
            T norm_x = std::sqrt(sigma);
            if (norm_x < std::numeric_limits<T>::epsilon()) { res.tau[k] = T{0}; continue; }
            v[k] += (v[k] >= T{0} ? norm_x : -norm_x);
            sigma = T{0};
            for (std::size_t i = k; i < M; ++i) sigma += v[i] * v[i];
            const T tau = T{2} / sigma;
            res.tau[k] = tau;

            // apply H = I - tau·v·vᵀ to trailing submatrix [k:M, k:N]
            for (std::size_t j = k; j < N; ++j) {
                T dot = T{0};
                for (std::size_t i = k; i < M; ++i) dot += v[i] * QR(i, j);
                dot *= tau;
                for (std::size_t i = k; i < M; ++i) QR(i, j) -= dot * v[i];
            }
            // store reflector below diagonal
            if (M > k + 1) {
                const T inv_vk = T{1} / v[k];
                for (std::size_t i = k+1; i < M; ++i) QR(i, k) = v[i] * inv_vk;
            }
        }
        return res;
    }

    // Build explicit Q (M×K economy) from packed QRResult
    template<typename T, typename SP = ts::DefaultStoragePolicy,
             typename CP = ts::DefaultComputationPolicy>
    [[nodiscard]] Matrix<T,SP,CP> qr_build_q(const QRResult<T,SP,CP>& res) {
        const std::size_t M = res.QR.rows(), N = res.QR.cols();
        const std::size_t K = std::min(M, N);
        // start with M×K identity
        Matrix<T,SP,CP> Q(M, K, T{0});
        for (std::size_t i = 0; i < K; ++i) Q(i, i) = T{1};
        // apply reflectors in reverse: H_K ... H_1
        for (std::size_t kb = K; kb-- > 0; ) {
            if (res.tau[kb] == T{0}) continue;
            // effective tau for unit v[kb]=1 convention
            T eff_sigma = T{1};
            for (std::size_t i = kb + 1; i < M; ++i) eff_sigma += res.QR(i, kb) * res.QR(i, kb);
            T eff_tau = T{2} / eff_sigma;
            // apply H = I - eff_tau*v*vT to Q[kb:M, kb:K]
            for (std::size_t j = kb; j < K; ++j) {
                T dot = Q(kb, j);
                for (std::size_t i = kb + 1; i < M; ++i) dot += res.QR(i, kb) * Q(i, j);
                dot *= eff_tau;
                Q(kb, j) -= dot;
                for (std::size_t i = kb + 1; i < M; ++i) Q(i, j) -= dot * res.QR(i, kb);
            }
        }
        return Q;
    }

    // -----------------------------------------------------------------------
    // LDLTResult<T> — Bunch-Kaufman LDLT for symmetric indefinite
    // -----------------------------------------------------------------------
    template<typename T, typename SP = ts::DefaultStoragePolicy,
             typename CP = ts::DefaultComputationPolicy>
    struct LDLTResult {
        Matrix<T,SP,CP>    LD;      // packed L (below diag) + D (diagonal)
        std::vector<int>   piv;     // pivots: positive=1×1, negative=2×2 block start
        FactorizationResult<T> info;
    };

    template<typename T, typename SP = ts::DefaultStoragePolicy,
             typename CP = ts::DefaultComputationPolicy>
    [[nodiscard]] LDLTResult<T,SP,CP> ldlt(const Matrix<T,SP,CP>& A) {
        const std::size_t N = A.rows();
        if (A.cols() != N) throw std::invalid_argument("ldlt: matrix must be square");

        LDLTResult<T,SP,CP> res;
        res.LD = A;
        auto& LD = res.LD;
        res.piv.resize(static_cast<int>(N), 0);

        // Diagonal LDLT (simplified 1×1 pivot only — production uses Bunch-Kaufman 2×2)
        for (std::size_t j = 0; j < N; ++j) {
            T d = LD(j, j);
            for (std::size_t k = 0; k < j; ++k) {
                const T ljk = LD(j, k);
                d -= ljk * ljk * LD(k, k);
            }
            if (std::abs(d) < std::numeric_limits<T>::epsilon()) {
                res.info.ok = false;
                res.info.error_msg = "ldlt: zero diagonal encountered";
                return res;
            }
            LD(j, j) = d;
            res.piv[static_cast<int>(j)] = static_cast<int>(j) + 1; // 1×1 pivot
            const T inv_d = T{1} / d;
            for (std::size_t i = j+1; i < N; ++i) {
                T l = LD(i, j);
                for (std::size_t k = 0; k < j; ++k) l -= LD(i, k) * LD(j, k) * LD(k, k);
                LD(i, j) = l * inv_d;
            }
        }
        return res;
    }

    // -----------------------------------------------------------------------
    // Tridiagonal solve — Thomas algorithm O(N)
    // -----------------------------------------------------------------------
    template<typename T>
    [[nodiscard]] std::vector<T> tridiagonal_solve(
            const std::vector<T>& lower,
            const std::vector<T>& diag,
            const std::vector<T>& upper,
            const std::vector<T>& rhs) {
        const std::size_t n = diag.size();
        if (lower.size() != n-1 || upper.size() != n-1 || rhs.size() != n)
            throw std::invalid_argument("tridiagonal_solve: inconsistent sizes");

        std::vector<T> c_star(n-1), d_star(n), x(n);
        c_star[0] = upper[0] / diag[0];
        d_star[0] = rhs[0] / diag[0];
        for (std::size_t i = 1; i < n; ++i) {
            const T m = (i < n) ? diag[i] - lower[i-1] * c_star[i-1] : diag[i] - lower[i-1] * c_star[i-1];
            d_star[i] = (rhs[i] - lower[i-1] * d_star[i-1]) / m;
            if (i < n-1) c_star[i] = upper[i] / m;
        }
        x[n-1] = d_star[n-1];
        for (int i = static_cast<int>(n)-2; i >= 0; --i)
            x[static_cast<std::size_t>(i)] = d_star[static_cast<std::size_t>(i)]
                - c_star[static_cast<std::size_t>(i)] * x[static_cast<std::size_t>(i)+1];
        return x;
    }

    // -----------------------------------------------------------------------
    // Triangular solve helpers
    // -----------------------------------------------------------------------

    // lower triangular: L·x = b  (forward substitution)
    template<typename T, typename SP, typename CP>
    [[nodiscard]] Vector<T,SP,CP> forward_solve(const Matrix<T,SP,CP>& L, const Vector<T,SP,CP>& b) {
        const std::size_t N = L.rows();
        Vector<T,SP,CP> x(N);
        for (std::size_t i = 0; i < N; ++i) {
            T s = b[i];
            for (std::size_t j = 0; j < i; ++j) s -= L(i, j) * x[j];
            x[i] = s / L(i, i);
        }
        return x;
    }

    // upper triangular: U·x = b  (back substitution)
    template<typename T, typename SP, typename CP>
    [[nodiscard]] Vector<T,SP,CP> back_solve(const Matrix<T,SP,CP>& U, const Vector<T,SP,CP>& b) {
        const std::size_t N = U.rows();
        Vector<T,SP,CP> x(N);
        for (int i = static_cast<int>(N)-1; i >= 0; --i) {
            const std::size_t ui = static_cast<std::size_t>(i);
            T s = b[ui];
            for (std::size_t j = ui+1; j < N; ++j) s -= U(ui, j) * x[j];
            x[ui] = s / U(ui, ui);
        }
        return x;
    }

    // -----------------------------------------------------------------------
    // .solve() helpers on result types
    // -----------------------------------------------------------------------

    // LU solve: A·x = b  using stored LU + pivot
    template<typename T, typename SP, typename CP>
    [[nodiscard]] Vector<T,SP,CP> lu_solve(const LUResult<T,SP,CP>& lu_res,
                                            const Vector<T,SP,CP>& b) {
        if (!lu_res.info.ok) throw std::runtime_error("lu_solve: factorization failed");
        const std::size_t N = b.size();
        const auto& LU = lu_res.LU;
        const auto& piv = lu_res.piv;

        // apply permutation
        Vector<T,SP,CP> pb(N);
        for (std::size_t i = 0; i < N; ++i) pb[i] = b[piv[i]];

        // forward solve with unit-diagonal L
        for (std::size_t i = 0; i < N; ++i)
            for (std::size_t j = 0; j < i; ++j)
                pb[i] -= LU(i, j) * pb[j];

        // back solve with U
        for (int i = static_cast<int>(N)-1; i >= 0; --i) {
            const std::size_t ui = static_cast<std::size_t>(i);
            for (std::size_t j = ui+1; j < N; ++j) pb[ui] -= LU(ui, j) * pb[j];
            pb[ui] /= LU(ui, ui);
        }
        return pb;
    }

    // Cholesky solve: A·x = b
    template<typename T, typename SP, typename CP>
    [[nodiscard]] Vector<T,SP,CP> cholesky_solve(const CholeskyResult<T,SP,CP>& cr,
                                                  const Vector<T,SP,CP>& b) {
        if (!cr.info.ok) throw std::runtime_error("cholesky_solve: factorization failed");
        auto y = forward_solve(cr.L, b);
        auto Lt = cr.L.transpose();
        return back_solve(Lt, y);
    }

    // LDLT solve: A·x = b
    template<typename T, typename SP, typename CP>
    [[nodiscard]] Vector<T,SP,CP> ldlt_solve(const LDLTResult<T,SP,CP>& lr,
                                              const Vector<T,SP,CP>& b) {
        if (!lr.info.ok) throw std::runtime_error("ldlt_solve: factorization failed");
        const std::size_t N = b.size();
        const auto& LD = lr.LD;
        Vector<T,SP,CP> y(N);
        // forward with L (unit diagonal)
        for (std::size_t i = 0; i < N; ++i) {
            y[i] = b[i];
            for (std::size_t j = 0; j < i; ++j) y[i] -= LD(i,j) * y[j];
        }
        // scale by D⁻¹
        for (std::size_t i = 0; i < N; ++i) y[i] /= LD(i,i);
        // back with Lᵀ (unit diagonal)
        Vector<T,SP,CP> x(N);
        for (int i = static_cast<int>(N)-1; i >= 0; --i) {
            const std::size_t ui = static_cast<std::size_t>(i);
            x[ui] = y[ui];
            for (std::size_t j = ui+1; j < N; ++j) x[ui] -= LD(j,ui) * x[j];
        }
        return x;
    }

    // QR solve (least squares): min‖Ax-b‖  via implicit Q application
    template<typename T, typename SP, typename CP>
    [[nodiscard]] Vector<T,SP,CP> qr_solve(const QRResult<T,SP,CP>& qr_res,
                                            const Vector<T,SP,CP>& b) {
        if (!qr_res.info.ok) throw std::runtime_error("qr_solve: factorization failed");
        const std::size_t M = qr_res.QR.rows();
        const std::size_t N = qr_res.QR.cols();
        const auto& QR = qr_res.QR;
        const auto& tau = qr_res.tau;

        // Apply Qᵀ to b  (Householder stored as v[k]=1 implicit, scaled by 1/v[k])
        Vector<T,SP,CP> qtb(M);
        for (std::size_t i = 0; i < M; ++i) qtb[i] = b[i];
        const std::size_t K = std::min(M, N);
        for (std::size_t k = 0; k < K; ++k) {
            if (tau[k] == T{0}) continue;
            // Reconstruct effective tau for unit v[k]=1 convention:
            // stored QR(i,k) = v[i]/v[k]; tau_stored = 2/sum_i(v[i]^2)
            // effective sigma with v[k]=1: 1 + sum_{i>k}(QR(i,k)^2)
            T eff_sigma = T{1};
            for (std::size_t i = k+1; i < M; ++i) eff_sigma += QR(i, k) * QR(i, k);
            T eff_tau = T{2} / eff_sigma;
            // dot = v^T * qtb  with v[k]=1
            T dot = qtb[k];
            for (std::size_t i = k+1; i < M; ++i) dot += QR(i, k) * qtb[i];
            dot *= eff_tau;
            qtb[k] -= dot;
            for (std::size_t i = k+1; i < M; ++i) qtb[i] -= dot * QR(i, k);
        }

        // back solve with R (upper triangular part)
        Vector<T,SP,CP> x(N);
        for (int i = static_cast<int>(N)-1; i >= 0; --i) {
            const std::size_t ui = static_cast<std::size_t>(i);
            T s = qtb[ui];
            for (std::size_t j = ui+1; j < N; ++j) s -= QR(ui, j) * x[j];
            x[ui] = s / QR(ui, ui);
        }
        return x;
    }

    // -----------------------------------------------------------------------
    // Matrix::det() — implemented here using lu()
    // (defined out-of-line to avoid include cycle)
    // -----------------------------------------------------------------------
    template<typename T, typename SP, typename CP>
    T Matrix<T,SP,CP>::det() const {
        if (rows() != cols()) throw std::invalid_argument("det: matrix must be square");
        auto lu_res = lu(*this);
        if (!lu_res.info.ok) return T{0};
        T d = T{1};
        for (std::size_t i = 0; i < rows(); ++i) d *= lu_res.LU(i, i);
        // count swaps
        std::size_t swaps = 0;
        auto piv = lu_res.piv;
        for (std::size_t i = 0; i < piv.size(); ++i) {
            while (piv[i] != i) { std::swap(piv[i], piv[piv[i]]); ++swaps; }
        }
        return (swaps % 2 == 0) ? d : -d;
    }

} // namespace ga

#endif // PEBBLE_CONTAINERS_MATRIX_FACTORIZE_HPP
