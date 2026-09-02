#include <catch_amalgamated.hpp>
#include <containers/matrix/matrix.hpp>
#include <cmath>

using namespace ga;

// ============================================================================
// test_solve.cpp — LU/Cholesky/QR/LDLT factorizations + direct solve
// ============================================================================

TEST_CASE (
"LU factorization: 3×3"
,
"[solve][lu]"
)
 {
    Matrix<double> A(3, 3);
    A(0,0)=2; A(0,1)=1; A(0,2)=-1;
    A(1,0)=-3; A(1,1)=-1; A(1,2)=2;
    A(2,0)=-2; A(2,1)=1; A(2,2)=2;
    Vector<double> b(3); b[0]=8; b[1]=-11; b[2]=-3;
    auto x = solve(A, b, MatrixKind::General);
    // solution: x=[2,3,-1]
    REQUIRE(x.size() == 3);
    CHECK(x[0] == Catch::Approx(2.0).epsilon(1e-8));
    CHECK(x[1] == Catch::Approx(3.0).epsilon(1e-8));
    CHECK(x[2] == Catch::Approx(-1.0).epsilon(1e-8));
}

TEST_CASE (
"LU factorization: identity system"
,
"[solve][lu]"
)
 {
    constexpr std::size_t N = 4;
    auto A = Matrix<float>::identity(N);
    Vector<float> b(N); for (std::size_t i=0;i<N;++i) b[i]=float(i+1);
    auto x = solve(A, b, MatrixKind::General);
    for (std::size_t i=0;i<N;++i) CHECK(x[i] == Catch::Approx(float(i+1)).epsilon(1e-5f));
}

TEST_CASE (
"LU: forward and back solve"
,
"[solve][lu][triangular]"
)
 {
    // lower triangular
    Matrix<float> L(3, 3, 0.f);
    L(0,0)=1; L(1,0)=2; L(1,1)=1; L(2,0)=3; L(2,1)=4; L(2,2)=1;
    Vector<float> b(3); b[0]=1; b[1]=5; b[2]=14;
    auto x = forward_solve(L, b);
    CHECK(x[0] == Catch::Approx(1.f).epsilon(1e-5f));
    CHECK(x[1] == Catch::Approx(3.f).epsilon(1e-5f));
    CHECK(x[2] == Catch::Approx(-1.f).epsilon(1e-5f));
}

TEST_CASE (
"Cholesky: SPD 3×3"
,
"[solve][cholesky]"
)
 {
    // A = [[4,2,2],[2,10,4],[2,4,11]]
    Matrix<double> A(3, 3);
    A(0,0)=4; A(0,1)=2; A(0,2)=2;
    A(1,0)=2; A(1,1)=10; A(1,2)=4;
    A(2,0)=2; A(2,1)=4; A(2,2)=11;
    Vector<double> b(3); b[0]=8; b[1]=16; b[2]=17;
    auto x = solve(A, b, MatrixKind::SPD);
    // residual: Ax - b
    for (std::size_t i=0;i<3;++i) {
        double ax = 0;
        for (std::size_t j=0;j<3;++j) ax += A(i,j)*x[j];
        CHECK(ax == Catch::Approx(b[i]).epsilon(1e-8));
    }
}

TEST_CASE (
"QR factorization: overdetermined system (least squares)"
,
"[solve][qr]"
)
 {
    Matrix<double> A(4, 2);
    A(0,0)=1; A(0,1)=1;
    A(1,0)=1; A(1,1)=2;
    A(2,0)=1; A(2,1)=3;
    A(3,0)=1; A(3,1)=4;
    Vector<double> b(4); b[0]=6; b[1]=5; b[2]=7; b[3]=10;
    // QR solve: min ||Ax-b||
    auto x = solve(A, b, MatrixKind::Overdetermined);
    // least-squares solution should have small residual
    double res = 0;
    for (std::size_t i=0;i<4;++i) {
        double ax = 0;
        for (std::size_t j=0;j<2;++j) ax += A(i,j)*x[j];
        res += (ax - b[i])*(ax - b[i]);
    }
    CHECK(std::sqrt(res) < 3.5);  // least-squares residual ≈ 2.9 for this data
}

TEST_CASE (
"LDLT: 3×3 indefinite-friendly SPD"
,
"[solve][ldlt]"
)
 {
    // Same SPD matrix as Cholesky test
    Matrix<double> A(3, 3);
    A(0,0)=4; A(0,1)=2; A(0,2)=0;
    A(1,0)=2; A(1,1)=5; A(1,2)=2;
    A(2,0)=0; A(2,1)=2; A(2,2)=5;
    Vector<double> b(3); b[0]=6; b[1]=9; b[2]=7;
    auto x = solve(A, b, MatrixKind::SymIndefinite);
    for (std::size_t i=0;i<3;++i) {
        double ax = 0;
        for (std::size_t j=0;j<3;++j) ax += A(i,j)*x[j];
        CHECK(ax == Catch::Approx(b[i]).epsilon(1e-8));
    }
}

TEST_CASE (
"solve dispatch: default uses LU"
,
"[solve][dispatch]"
)
 {
    Matrix<double> A(2, 2);
    A(0,0)=3; A(0,1)=1;
    A(1,0)=1; A(1,1)=2;
    Vector<double> b(2); b[0]=5; b[1]=4;
    // x=[6/5, 7/5]=1.2, 1.4
    auto x = solve(A, b);
    CHECK(x[0] == Catch::Approx(1.2).epsilon(1e-8));
    CHECK(x[1] == Catch::Approx(1.4).epsilon(1e-8));
}
