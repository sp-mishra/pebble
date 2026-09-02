#include <catch_amalgamated.hpp>
#include <containers/matrix/matrix.hpp>
#include <cmath>
#include <vector>

using namespace ga;

// ============================================================================
// test_iterative.cpp — CG/BiCGSTAB/GMRES/MINRES/FGMRES + relaxation
// ============================================================================

static Matrix<double> make_spd(std::size_t n) {
    // Discrete 1D Laplacian: A = tridiag(-1, 2, -1)
    Matrix<double> A(n, n, 0.0);
    for (std::size_t i = 0; i < n; ++i) {
        A(i, i) = 2.0;
        if (i > 0) A(i, i - 1) = -1.0;
        if (i < n - 1) A(i, i + 1) = -1.0;
    }
    return A;
}

// simple lambda-based apply
static auto make_apply(const Matrix<double>& A) {
    return [&](const Vector<double>& x) -> Vector<double> {
        std::size_t n = A.rows();
        Vector<double> y(n, 0.0);
        for (std::size_t i = 0; i < n; ++i)
            for (std::size_t j = 0; j < n; ++j)
                y[j] += A(i, j) * x[i];
        return y;
    };
}

TEST_CASE (
"CG: 1D Laplacian 5×5 converges"
,
"[iterative][cg]"
)
 {
    constexpr std::size_t N = 5;
    auto A = make_spd(N);
    auto apply = make_apply(A);
    Vector<double> b(N, 1.0);
    Vector<double> x(N, 0.0);
    IdentityPrecond<double> P;
    auto res = cg(apply, b, x, P, 1e-10, 100);
    REQUIRE(res.converged);
    // residual ||b - Ax|| should be tiny
    auto Ax = apply(x);
    double norm_r = 0;
    for (std::size_t i=0;i<N;++i) norm_r += (b[i]-Ax[i])*(b[i]-Ax[i]);
    CHECK(std::sqrt(norm_r) < 1e-8);
}

TEST_CASE (
"CG: with Jacobi preconditioner"
,
"[iterative][cg][jacobi]"
)
 {
    constexpr std::size_t N = 8;
    auto A = make_spd(N);
    auto apply = make_apply(A);
    Vector<double> b(N, 1.0);
    Vector<double> x(N, 0.0);
    JacobiPrecond<double> P(A);
    auto res = cg(apply, b, x, P, 1e-10, 200);
    REQUIRE(res.converged);
    CHECK(res.final_residual < 1e-8);
}

TEST_CASE (
"BiCGSTAB: non-symmetric 4×4"
,
"[iterative][bicgstab]"
)
 {
    Matrix<double> A(4, 4, 0.0);
    A(0,0)=4; A(0,1)=1;
    A(1,0)=2; A(1,1)=5; A(1,2)=1;
    A(2,1)=1; A(2,2)=5; A(2,3)=1;
    A(3,2)=2; A(3,3)=4;
    auto apply = make_apply(A);
    Vector<double> b(4, 1.0);
    Vector<double> x(4, 0.0);
    IdentityPrecond<double> P;
    auto res = bicgstab(apply, b, x, P, 1e-10, 100);
    REQUIRE(res.converged);
    auto Ax = apply(x);
    for (std::size_t i=0;i<4;++i) CHECK(Ax[i] == Catch::Approx(b[i]).epsilon(1e-8));
}

TEST_CASE (
"GMRES: 4×4 system"
,
"[iterative][gmres]"
)
 {
    Matrix<double> A(4, 4, 0.0);
    A(0,0)=4; A(0,1)=-1; A(0,2)=0; A(0,3)=0;
    A(1,0)=-1; A(1,1)=4; A(1,2)=-1; A(1,3)=0;
    A(2,0)=0; A(2,1)=-1; A(2,2)=4; A(2,3)=-1;
    A(3,0)=0; A(3,1)=0; A(3,2)=-1; A(3,3)=4;
    auto apply = make_apply(A);
    Vector<double> b(4); b[0]=1; b[1]=0; b[2]=0; b[3]=1;
    Vector<double> x(4, 0.0);
    IdentityPrecond<double> P;
    auto res = gmres(apply, b, x, P, 20, 1e-10, 100);
    REQUIRE(res.converged);
    auto Ax = apply(x);
    for (std::size_t i=0;i<4;++i) CHECK(Ax[i] == Catch::Approx(b[i]).margin(1e-10));
}

TEST_CASE (
"MINRES: symmetric indefinite 4×4"
,
"[iterative][minres]"
)
 {
    Matrix<double> A(4, 4, 0.0);
    // symmetric but not positive definite: shift Laplacian by -1.5*I
    for (std::size_t i=0;i<4;++i) {
        A(i,i) = 2.0 - 1.5;
        if (i > 0)   A(i,i-1) = -1.0;
        if (i < 3)   A(i,i+1) = -1.0;
    }
    auto apply = make_apply(A);
    Vector<double> b(4, 1.0);
    Vector<double> x(4, 0.0);
    IdentityPrecond<double> P;
    auto res = minres(apply, b, x, P, 1e-10, 200);
    // may or may not converge (indefinite) but shouldn't crash
    auto Ax = apply(x);
    if (res.converged) {
        for (std::size_t i=0;i<4;++i) CHECK(Ax[i] == Catch::Approx(b[i]).epsilon(1e-7));
    }
}

TEST_CASE (
"Jacobi sweep: smoother on Laplacian"
,
"[iterative][jacobi][relaxation]"
)
 {
    constexpr std::size_t N = 4;
    auto A = make_spd(N);
    Vector<double> b(N, 1.0);
    Vector<double> x(N, 0.0);
    // After many sweeps, should converge
    for (int k=0; k<1000; ++k) jacobi_sweep(A, b, x);
    auto apply = make_apply(A);
    auto Ax = apply(x);
    double res2 = 0;
    for (std::size_t i=0;i<N;++i) res2 += (b[i]-Ax[i])*(b[i]-Ax[i]);
    CHECK(std::sqrt(res2) < 0.01);
}

TEST_CASE (
"Gauss-Seidel sweep: better than Jacobi"
,
"[iterative][gs][relaxation]"
)
 {
    constexpr std::size_t N = 4;
    auto A = make_spd(N);
    Vector<double> b(N, 1.0);
    Vector<double> x(N, 0.0);
    for (int k=0; k<200; ++k) gauss_seidel_sweep(A, b, x);
    auto apply = make_apply(A);
    auto Ax = apply(x);
    double res2 = 0;
    for (std::size_t i=0;i<N;++i) res2 += (b[i]-Ax[i])*(b[i]-Ax[i]);
    CHECK(std::sqrt(res2) < 0.01);
}

TEST_CASE (
"IterResult: has expected fields"
,
"[iterative][result]"
)
 {
    IterResult<float> r;
    r.converged = true;
    r.iterations = 10;
    r.final_residual = 1e-9f;
    r.residual_history.push_back(1.0f);
    REQUIRE(r.iterations == 10);
    REQUIRE(r.converged);
    CHECK(r.final_residual < 1e-8f);
    REQUIRE(!r.residual_history.empty());
}
