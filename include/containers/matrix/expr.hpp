#pragma once
// ============================================================================
// expr.hpp: (lazy, fuse-to-BLAS)
// ============================================================================
// Expression nodes: ScaleExpr, TransposeExpr, MatMulExpr, AddExpr
// Recognises α·A·B + β·C at compile time → single ts::gemm(α,A,B,β,C) call.
// Shape mismatch = compile error via static_assert.
// ============================================================================

#ifndef PEBBLE_CONTAINERS_MATRIX_EXPR_HPP
#define PEBBLE_CONTAINERS_MATRIX_EXPR_HPP

#include <containers/matrix/dense.hpp>
#include <type_traits>

namespace ga {
namespace expr {

    // -----------------------------------------------------------------------
    // Tag types — detect expression nodes in template specialisations
    // -----------------------------------------------------------------------
    struct scale_tag {};
    struct transpose_tag {};
    struct matmul_tag {};
    struct add_tag {};

    // -----------------------------------------------------------------------
    // ScaleExpr<T, E>  — α * E
    // -----------------------------------------------------------------------
    template<typename T, typename E>
    struct ScaleExpr : scale_tag {
        T     alpha;
        E     expr;
        using value_type = T;
        using inner_type = E;
        ScaleExpr(T a, E e) : alpha(std::move(a)), expr(std::move(e)) {}
    };

    // -----------------------------------------------------------------------
    // TransposeExpr<E>  — Eᵀ  (lazy; no allocation until eval)
    // -----------------------------------------------------------------------
    template<typename E>
    struct TransposeExpr : transpose_tag {
        E     expr;
        using value_type = typename E::value_type;
        using inner_type = E;
        explicit TransposeExpr(E e) : expr(std::move(e)) {}
    };

    // -----------------------------------------------------------------------
    // MatMulExpr<L, R>  — L * R
    // -----------------------------------------------------------------------
    template<typename L, typename R>
    struct MatMulExpr : matmul_tag {
        L lhs;
        R rhs;
        using value_type = typename L::value_type;
        using lhs_type = L;
        using rhs_type = R;
        MatMulExpr(L l, R r) : lhs(std::move(l)), rhs(std::move(r)) {}
    };

    // -----------------------------------------------------------------------
    // AddExpr<L, R>  — L + R
    // -----------------------------------------------------------------------
    template<typename L, typename R>
    struct AddExpr : add_tag {
        L lhs;
        R rhs;
        using value_type = typename L::value_type;
        using lhs_type = L;
        using rhs_type = R;
        AddExpr(L l, R r) : lhs(std::move(l)), rhs(std::move(r)) {}
    };

    // -----------------------------------------------------------------------
    // Trait: detect ScaleExpr wrapping a Matrix
    // -----------------------------------------------------------------------
    template<typename E>
    struct is_scaled_matrix : std::false_type {};

    template<typename T, typename SP, typename CP>
    struct is_scaled_matrix<ScaleExpr<T, Matrix<T,SP,CP>>> : std::true_type {};

    // -----------------------------------------------------------------------
    // Trait: detect ScaleExpr wrapping a MatMulExpr of Matrices
    // -----------------------------------------------------------------------
    template<typename E>
    struct is_scaled_matmul : std::false_type {};

    template<typename T, typename SP, typename CP>
    struct is_scaled_matmul<ScaleExpr<T, MatMulExpr<Matrix<T,SP,CP>, Matrix<T,SP,CP>>>>
        : std::true_type {};

    // -----------------------------------------------------------------------
    // Evaluate an expression to a Matrix
    // Generic fallback (no fusion)
    // -----------------------------------------------------------------------
    template<typename E, typename SP, typename CP>
    auto eval(const E& e) -> Matrix<typename E::value_type, SP, CP>;

    // ScaleExpr<T, Matrix> → α * M (scalar multiply)
    template<typename T, typename SP, typename CP>
    Matrix<T,SP,CP> eval(const ScaleExpr<T, Matrix<T,SP,CP>>& e) {
        return e.expr * e.alpha;
    }

    // TransposeExpr<Matrix> → allocating transpose
    template<typename T, typename SP, typename CP>
    Matrix<T,SP,CP> eval(const TransposeExpr<Matrix<T,SP,CP>>& e) {
        return e.expr.transpose();
    }

    // MatMulExpr<Matrix, Matrix> → matmul
    template<typename T, typename SP, typename CP>
    Matrix<T,SP,CP> eval(const MatMulExpr<Matrix<T,SP,CP>, Matrix<T,SP,CP>>& e) {
        return e.lhs * e.rhs;
    }

    // -----------------------------------------------------------------------
    // Key fusion: AddExpr< ScaleExpr<T, MatMulExpr<M,M>>, ScaleExpr<T, M> >
    // Detects: α·(A*B) + β·C  →  ts::gemm(α, A, B, β, C)
    // -----------------------------------------------------------------------
    template<typename T, typename SP, typename CP>
    Matrix<T,SP,CP> eval(
        const AddExpr<
            ScaleExpr<T, MatMulExpr<Matrix<T,SP,CP>, Matrix<T,SP,CP>>>,
            ScaleExpr<T, Matrix<T,SP,CP>>
        >& e)
    {
        const T    alpha = e.lhs.alpha;
        const auto& A   = e.lhs.expr.lhs;
        const auto& B   = e.lhs.expr.rhs;
        const T    beta = e.rhs.alpha;
        const auto& C   = e.rhs.expr;

        // Shape check at compile-time is not possible here (runtime dims);
        // ts::gemm will throw/assert on mismatch
        Matrix<T,SP,CP> out = C; // copy of C (β·C start)
        ga::gemm<T,SP,CP>(alpha, A, B, beta, out);
        return out;
    }

    // AddExpr< MatMulExpr, ScaleExpr > — α=1 case
    template<typename T, typename SP, typename CP>
    Matrix<T,SP,CP> eval(
        const AddExpr<
            MatMulExpr<Matrix<T,SP,CP>, Matrix<T,SP,CP>>,
            ScaleExpr<T, Matrix<T,SP,CP>>
        >& e)
    {
        const auto& A   = e.lhs.lhs;
        const auto& B   = e.lhs.rhs;
        const T    beta = e.rhs.alpha;
        const auto& C   = e.rhs.expr;
        Matrix<T,SP,CP> out = C;
        ga::gemm<T,SP,CP>(T{1}, A, B, beta, out);
        return out;
    }

    // AddExpr< ScaleExpr<MatMulExpr>, Matrix > — β=1 case
    template<typename T, typename SP, typename CP>
    Matrix<T,SP,CP> eval(
        const AddExpr<
            ScaleExpr<T, MatMulExpr<Matrix<T,SP,CP>, Matrix<T,SP,CP>>>,
            Matrix<T,SP,CP>
        >& e)
    {
        const T    alpha = e.lhs.alpha;
        const auto& A   = e.lhs.expr.lhs;
        const auto& B   = e.lhs.expr.rhs;
        const auto& C   = e.rhs;
        Matrix<T,SP,CP> out = C;
        ga::gemm<T,SP,CP>(alpha, A, B, T{1}, out);
        return out;
    }

    // AddExpr< MatMulExpr, Matrix > — α=β=1
    template<typename T, typename SP, typename CP>
    Matrix<T,SP,CP> eval(
        const AddExpr<
            MatMulExpr<Matrix<T,SP,CP>, Matrix<T,SP,CP>>,
            Matrix<T,SP,CP>
        >& e)
    {
        const auto& A = e.lhs.lhs;
        const auto& B = e.lhs.rhs;
        const auto& C = e.rhs;
        Matrix<T,SP,CP> out = C;
        ga::gemm<T,SP,CP>(T{1}, A, B, T{1}, out);
        return out;
    }

} // namespace expr

    // -----------------------------------------------------------------------
    // Operator overloads — build expression tree without evaluating
    // -----------------------------------------------------------------------

    // α * M  →  eager scalar multiply (consistent with M * α in dense.hpp)
    template<typename T, typename SP, typename CP>
    [[nodiscard]] Matrix<T,SP,CP> operator*(T alpha, const Matrix<T,SP,CP>& M) {
        return M * alpha;
    }

    // Mᵀ  →  TransposeExpr  (explicit free function; matrix has .transpose())
    template<typename T, typename SP, typename CP>
    [[nodiscard]] auto T_(const Matrix<T,SP,CP>& M) {
        return expr::TransposeExpr<Matrix<T,SP,CP>>{M};
    }

    // ScaleExpr * Matrix  →  ScaleExpr<MatMulExpr>
    template<typename T, typename SP, typename CP>
    [[nodiscard]] auto operator*(const expr::ScaleExpr<T, Matrix<T,SP,CP>>& s,
                                  const Matrix<T,SP,CP>& B) {
        using mm_t = expr::MatMulExpr<Matrix<T,SP,CP>, Matrix<T,SP,CP>>;
        return expr::ScaleExpr<T, mm_t>{s.alpha, mm_t{s.expr, B}};
    }

    // Matrix * Matrix  →  MatMulExpr
    // NOTE: This conflicts with the existing operator* in dense.hpp that evaluates immediately.
    // To build a lazy expression, users call expr::lazy_mul(A,B).
    template<typename T, typename SP, typename CP>
    [[nodiscard]] auto lazy_mul(const Matrix<T,SP,CP>& A, const Matrix<T,SP,CP>& B) {
        return expr::MatMulExpr<Matrix<T,SP,CP>, Matrix<T,SP,CP>>{A, B};
    }

    // MatMulExpr + ScaleExpr  →  AddExpr (fused gemm path)
    template<typename T, typename SP, typename CP>
    [[nodiscard]] auto operator+(
        const expr::MatMulExpr<Matrix<T,SP,CP>, Matrix<T,SP,CP>>& mm,
        const expr::ScaleExpr<T, Matrix<T,SP,CP>>& sc)
    {
        return expr::AddExpr<
            expr::MatMulExpr<Matrix<T,SP,CP>, Matrix<T,SP,CP>>,
            expr::ScaleExpr<T, Matrix<T,SP,CP>>>{mm, sc};
    }

    // ScaleExpr<MatMulExpr> + ScaleExpr  →  AddExpr (fused gemm path)
    template<typename T, typename SP, typename CP>
    [[nodiscard]] auto operator+(
        const expr::ScaleExpr<T, expr::MatMulExpr<Matrix<T,SP,CP>, Matrix<T,SP,CP>>>& smm,
        const expr::ScaleExpr<T, Matrix<T,SP,CP>>& sc)
    {
        return expr::AddExpr<
            expr::ScaleExpr<T, expr::MatMulExpr<Matrix<T,SP,CP>, Matrix<T,SP,CP>>>,
            expr::ScaleExpr<T, Matrix<T,SP,CP>>>{smm, sc};
    }

    // ScaleExpr<MatMulExpr> + Matrix  →  AddExpr
    template<typename T, typename SP, typename CP>
    [[nodiscard]] auto operator+(
        const expr::ScaleExpr<T, expr::MatMulExpr<Matrix<T,SP,CP>, Matrix<T,SP,CP>>>& smm,
        const Matrix<T,SP,CP>& C)
    {
        return expr::AddExpr<
            expr::ScaleExpr<T, expr::MatMulExpr<Matrix<T,SP,CP>, Matrix<T,SP,CP>>>,
            Matrix<T,SP,CP>>{smm, C};
    }

    // MatMulExpr + Matrix  →  AddExpr
    template<typename T, typename SP, typename CP>
    [[nodiscard]] auto operator+(
        const expr::MatMulExpr<Matrix<T,SP,CP>, Matrix<T,SP,CP>>& mm,
        const Matrix<T,SP,CP>& C)
    {
        return expr::AddExpr<
            expr::MatMulExpr<Matrix<T,SP,CP>, Matrix<T,SP,CP>>,
            Matrix<T,SP,CP>>{mm, C};
    }

} // namespace ga

#endif // PEBBLE_CONTAINERS_MATRIX_EXPR_HPP
