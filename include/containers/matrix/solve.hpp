#pragma once
// ============================================================================
// solve.hpp — Ganita unified linear solve with auto-dispatch
// ============================================================================
// solve(A, b): auto-picks best factorization based on matrix properties.
//   SPD (pos-def hint) → Cholesky
//   Symmetric indefinite → LDLT
//   General square → LU
//   Rectangular (M>N) → QR (least squares)
//   Triangular → direct forward/back solve
//   Tridiagonal → Thomas algorithm
// ============================================================================

#ifndef PEBBLE_CONTAINERS_MATRIX_SOLVE_HPP
#define PEBBLE_CONTAINERS_MATRIX_SOLVE_HPP

#include <containers/matrix/factorize.hpp>

namespace ga {

    // -----------------------------------------------------------------------
    // MatrixKind — caller hint for auto-dispatch
    // -----------------------------------------------------------------------
    enum class MatrixKind {
        General,        // default: use LU
        SPD,            // symmetric positive definite → Cholesky
        SymIndefinite,  // symmetric indefinite → LDLT
        LowerTriangular,
        UpperTriangular,
        Tridiagonal,    // pass as Matrix with lower=col0, diag=col1, upper=col2
        Overdetermined, // M>N → QR least squares
    };

    // -----------------------------------------------------------------------
    // triangular_solve — direct forward/back substitution
    // -----------------------------------------------------------------------
    template<typename T, typename SP, typename CP>
    [[nodiscard]] Vector<T,SP,CP> triangular_solve(
            const Matrix<T,SP,CP>& A,
            const Vector<T,SP,CP>& b,
            bool lower = true) {
        if (lower) return forward_solve(A, b);
        return back_solve(A, b);
    }

    // -----------------------------------------------------------------------
    // solve — unified dispatch
    // -----------------------------------------------------------------------
    template<typename T, typename SP, typename CP>
    [[nodiscard]] Vector<T,SP,CP> solve(
            const Matrix<T,SP,CP>& A,
            const Vector<T,SP,CP>& b,
            MatrixKind kind = MatrixKind::General) {

        const std::size_t M = A.rows(), N = A.cols();

        switch (kind) {
        case MatrixKind::SPD: {
            auto cr = cholesky(A);
            return cholesky_solve(cr, b);
        }
        case MatrixKind::SymIndefinite: {
            auto lr = ldlt(A);
            return ldlt_solve(lr, b);
        }
        case MatrixKind::LowerTriangular:
            return forward_solve(A, b);
        case MatrixKind::UpperTriangular:
            return back_solve(A, b);
        case MatrixKind::Overdetermined: {
            if (M < N) throw std::invalid_argument("solve: Overdetermined requires M>=N");
            auto qr_res = qr(A);
            return qr_solve(qr_res, b);
        }
        case MatrixKind::Tridiagonal: {
            // A encodes diagonals: col 0 = lower (size N-1 usable), col 1 = main, col 2 = upper
            if (N < 3) throw std::invalid_argument("solve(Tridiagonal): need at least 3 columns");
            std::vector<T> lower(M-1), diag(M), upper(M-1);
            for (std::size_t i = 0; i < M; ++i)   diag[i]  = A(i, 1);
            for (std::size_t i = 0; i < M-1; ++i) { lower[i] = A(i+1, 0); upper[i] = A(i, 2); }
            std::vector<T> rhs(M);
            for (std::size_t i = 0; i < M; ++i) rhs[i] = b[i];
            auto x = tridiagonal_solve(lower, diag, upper, rhs);
            Vector<T,SP,CP> result(M);
            for (std::size_t i = 0; i < M; ++i) result[i] = x[i];
            return result;
        }
        default: // General
            if (M != N) {
                // underdetermined or overdetermined → QR
                auto qr_res = qr(A);
                return qr_solve(qr_res, b);
            }
            auto lu_res = lu(A);
            return lu_solve(lu_res, b);
        }
    }

    // -----------------------------------------------------------------------
    // solve_multi — solve A·X = B for multiple RHS columns
    // -----------------------------------------------------------------------
    template<typename T, typename SP, typename CP>
    [[nodiscard]] Matrix<T,SP,CP> solve_multi(
            const Matrix<T,SP,CP>& A,
            const Matrix<T,SP,CP>& B,
            MatrixKind kind = MatrixKind::General) {
        const std::size_t M = A.rows(), K = B.cols();
        Matrix<T,SP,CP> X(A.cols(), K);
        Vector<T,SP,CP> col_b(M);
        for (std::size_t j = 0; j < K; ++j) {
            for (std::size_t i = 0; i < M; ++i) col_b[i] = B(i, j);
            auto x_j = solve(A, col_b, kind);
            for (std::size_t i = 0; i < x_j.size(); ++i) X(i, j) = x_j[i];
        }
        return X;
    }

    // -----------------------------------------------------------------------
    // schur_solve — block 2×2 Schur complement solve
    // Solves [ A  B ] [ x ]   [ f ]
    //        [ C  D ] [ y ] = [ g ]
    // using x = (A - B·D⁻¹·C)⁻¹·(f - B·D⁻¹·g)
    // -----------------------------------------------------------------------
    template<typename T, typename SP, typename CP>
    struct SchurSolution {
        Vector<T,SP,CP> x;
        Vector<T,SP,CP> y;
    };

    template<typename T, typename SP, typename CP>
    [[nodiscard]] SchurSolution<T,SP,CP> schur_solve(
            const Matrix<T,SP,CP>& A,
            const Matrix<T,SP,CP>& B,
            const Matrix<T,SP,CP>& C,
            const Matrix<T,SP,CP>& D,
            const Vector<T,SP,CP>& f,
            const Vector<T,SP,CP>& g,
            MatrixKind d_kind = MatrixKind::General) {

        // Solve D·y_tmp = g  (to get D⁻¹·g)
        auto y_tmp = solve(D, g, d_kind);

        // Solve D·Z = C  for each column of C  (D⁻¹·C)
        auto DinvC = solve_multi(D, C, d_kind);

        // Schur complement S = A - B·(D⁻¹·C)
        auto S = A - B * DinvC;

        // rhs = f - B·(D⁻¹·g)
        Vector<T,SP,CP> Bdinvg(f.size());
        ga::gemv<T,SP,CP>(T{1}, B, y_tmp, T{0}, Bdinvg);
        Vector<T,SP,CP> rhs(f.size());
        for (std::size_t i = 0; i < f.size(); ++i) rhs[i] = f[i] - Bdinvg[i];

        auto x = solve(S, rhs);

        // y = D⁻¹·(g - C·x)
        Vector<T,SP,CP> Cx(g.size());
        ga::gemv<T,SP,CP>(T{1}, C, x, T{0}, Cx);
        Vector<T,SP,CP> g_minus_cx(g.size());
        for (std::size_t i = 0; i < g.size(); ++i) g_minus_cx[i] = g[i] - Cx[i];
        auto y = solve(D, g_minus_cx, d_kind);

        return {std::move(x), std::move(y)};
    }

} // namespace ga

#endif // PEBBLE_CONTAINERS_MATRIX_SOLVE_HPP
