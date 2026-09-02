#include <catch_amalgamated.hpp>
#include <containers/matrix/matrix.hpp>
#include <cmath>

using namespace ga;

// ============================================================================
// test_expr.cpp — algebra EDSL expression fusion
// ============================================================================

TEST_CASE (
"expr: matrix addition A+B"
,
"[expr][add]"
)
 {
    Matrix<float> A(2, 2), B(2, 2);
    A(0,0)=1; A(0,1)=2; A(1,0)=3; A(1,1)=4;
    B(0,0)=5; B(0,1)=6; B(1,0)=7; B(1,1)=8;
    auto C = A + B;
    CHECK(C(0,0) == 6.f);
    CHECK(C(0,1) == 8.f);
    CHECK(C(1,0) == 10.f);
    CHECK(C(1,1) == 12.f);
}

TEST_CASE (
"expr: matrix subtraction A-B"
,
"[expr][sub]"
)
 {
    Matrix<float> A(2, 2, 5.f), B(2, 2, 3.f);
    auto C = A - B;
    for (std::size_t i=0;i<2;++i)
        for (std::size_t j=0;j<2;++j)
            CHECK(C(i,j) == 2.f);
}

TEST_CASE (
"expr: scalar multiply A*s"
,
"[expr][scale]"
)
 {
    Matrix<float> A(2, 2, 3.f);
    auto B = A * 2.f;
    for (std::size_t i=0;i<2;++i)
        for (std::size_t j=0;j<2;++j)
            CHECK(B(i,j) == 6.f);
}

TEST_CASE (
"expr: scalar multiply s*A (commutative)"
,
"[expr][scale_comm]"
)
 {
    Matrix<float> A(2, 2, 3.f);
    auto B = 4.f * A;
    for (std::size_t i=0;i<2;++i)
        for (std::size_t j=0;j<2;++j)
            CHECK(B(i,j) == 12.f);
}

TEST_CASE (
"expr: matrix multiply A*B (gemm)"
,
"[expr][matmul]"
)
 {
    Matrix<float> A(2, 3), B(3, 2);
    // A = [[1,2,3],[4,5,6]], B = [[7,8],[9,10],[11,12]]
    A(0,0)=1; A(0,1)=2; A(0,2)=3;
    A(1,0)=4; A(1,1)=5; A(1,2)=6;
    B(0,0)=7; B(0,1)=8; B(1,0)=9; B(1,1)=10; B(2,0)=11; B(2,1)=12;
    auto C = A * B;
    // C = [[58,64],[139,154]]
    CHECK(C(0,0) == Catch::Approx(58.f));
    CHECK(C(0,1) == Catch::Approx(64.f));
    CHECK(C(1,0) == Catch::Approx(139.f));
    CHECK(C(1,1) == Catch::Approx(154.f));
}

TEST_CASE (
"expr: transpose"
,
"[expr][transpose]"
)
 {
    Matrix<float> A(2, 3);
    A(0,0)=1; A(0,1)=2; A(0,2)=3;
    A(1,0)=4; A(1,1)=5; A(1,2)=6;
    auto At = A.transpose();
    REQUIRE(At.rows() == 3);
    REQUIRE(At.cols() == 2);
    CHECK(At(0,0) == 1.f);
    CHECK(At(1,0) == 2.f);
    CHECK(At(2,0) == 3.f);
    CHECK(At(2,1) == 6.f);
}

TEST_CASE (
"expr: chain A+B+C"
,
"[expr][chain_add]"
)
 {
    Matrix<float> A(2, 2, 1.f), B(2, 2, 2.f), C(2, 2, 3.f);
    auto D = A + B + C;
    for (std::size_t i=0;i<2;++i)
        for (std::size_t j=0;j<2;++j)
            CHECK(D(i,j) == 6.f);
}

TEST_CASE (
"expr: identity matrix multiply"
,
"[expr][identity_mul]"
)
 {
    constexpr std::size_t N = 3;
    Matrix<float> A(N, N, 0.f);
    A(0,0)=2; A(1,1)=3; A(2,2)=4;
    auto I = Matrix<float>::identity(N);
    auto B = A * I;
    for (std::size_t i=0;i<N;++i)
        for (std::size_t j=0;j<N;++j)
            CHECK(B(i,j) == Catch::Approx(A(i,j)));
}

TEST_CASE (
"expr: matrix trace"
,
"[expr][trace]"
)
 {
    Matrix<float> A(3, 3, 0.f);
    A(0,0)=1; A(1,1)=2; A(2,2)=3;
    CHECK(A.trace() == Catch::Approx(6.f));
}

TEST_CASE (
"expr: StaticMatrix + StaticMatrix"
,
"[expr][static_add]"
)
 {
    StaticMatrix<float, 2, 2> A, B;
    A(0,0)=1; A(0,1)=2; A(1,0)=3; A(1,1)=4;
    B(0,0)=5; B(0,1)=6; B(1,0)=7; B(1,1)=8;
    auto C = A + B;
    CHECK(C(0,0) == 6.f);
    CHECK(C(1,1) == 12.f);
}

TEST_CASE (
"expr: StaticMatrix * StaticMatrix"
,
"[expr][static_mul]"
)
 {
    StaticMatrix<float, 2, 2> A, B;
    A(0,0)=1; A(0,1)=0; A(1,0)=0; A(1,1)=2;
    B(0,0)=3; B(0,1)=0; B(1,0)=0; B(1,1)=4;
    auto C = A * B;
    CHECK(C(0,0) == Catch::Approx(3.f));
    CHECK(C(1,1) == Catch::Approx(8.f));
}
