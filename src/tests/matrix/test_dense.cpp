#include <catch_amalgamated.hpp>
#include <containers/matrix/ganita.hpp>
#include <cmath>

using namespace ga;

TEST_CASE("Matrix: construction and dimensions", "[matrix][dense]") {
    Matrix<float> m(3, 4);
    REQUIRE(m.rows() == 3);
    REQUIRE(m.cols() == 4);
}

TEST_CASE("Matrix: element access operator()(r,c)", "[matrix][dense]") {
    Matrix<float> m(3, 3);
    m(0, 0) = 1.f; m(0, 1) = 2.f; m(0, 2) = 3.f;
    m(1, 0) = 4.f; m(1, 1) = 5.f; m(1, 2) = 6.f;
    m(2, 0) = 7.f; m(2, 1) = 8.f; m(2, 2) = 9.f;
    CHECK(m(1, 1) == 5.f);
    CHECK(m(2, 2) == 9.f);
    const Matrix<float>& cm = m;
    CHECK(cm(0, 2) == 3.f);
}

TEST_CASE("Matrix: fill constructor", "[matrix][dense]") {
    Matrix<double> m(2, 2, 7.0);
    CHECK(m(0, 0) == 7.0);
    CHECK(m(1, 1) == 7.0);
}

TEST_CASE("Matrix: trace", "[matrix][dense]") {
    Matrix<float> m(3, 3, 0.f);
    m(0,0)=1.f; m(1,1)=2.f; m(2,2)=3.f;
    CHECK(m.trace() == Catch::Approx(6.f));
}

TEST_CASE("Matrix: transpose", "[matrix][dense]") {
    Matrix<float> m(2, 3);
    m(0,0)=1; m(0,1)=2; m(0,2)=3;
    m(1,0)=4; m(1,1)=5; m(1,2)=6;
    auto mt = m.transpose();
    REQUIRE(mt.rows() == 3);
    REQUIRE(mt.cols() == 2);
    CHECK(mt(0,0) == 1.f);
    CHECK(mt(1,0) == 2.f);
    CHECK(mt(2,0) == 3.f);
    CHECK(mt(0,1) == 4.f);
}

TEST_CASE("Matrix: scalar multiply", "[matrix][dense]") {
    Matrix<float> m(2, 2, 1.f);
    auto m2 = m * 3.f;
    CHECK(m2(0,0) == 3.f);
    CHECK(m2(1,1) == 3.f);
}

TEST_CASE("Vector: construction and indexing", "[matrix][dense][vector]") {
    Vector<float> v(5);
    v[0] = 1.f; v[1] = 2.f; v[2] = 3.f; v[3] = 4.f; v[4] = 5.f;
    REQUIRE(v.size() == 5);
    CHECK(v[0] == 1.f);
    CHECK(v[4] == 5.f);
    CHECK(v(2) == 3.f);
    const Vector<float>& cv = v;
    CHECK(cv(3) == 4.f);
}

TEST_CASE("Vector: fill constructor", "[matrix][dense][vector]") {
    Vector<double> v(4, 2.5);
    for (std::size_t i = 0; i < 4; ++i) CHECK(v[i] == 2.5);
}

TEST_CASE("Vector: axpy via free function", "[matrix][dense][vector]") {
    Vector<float> x(3, 1.f), y(3, 2.f);
    // y += alpha*x
    for (std::size_t i=0;i<3;++i) y[i] += 2.f * x[i];
    for (std::size_t i=0;i<3;++i) CHECK(y[i] == Catch::Approx(4.f));
}

TEST_CASE("StaticMatrix: construction and operations", "[matrix][static]") {
    StaticMatrix<float, 3, 3> A;
    A(0,0)=1; A(1,1)=2; A(2,2)=3;
    CHECK(A(0,0) == 1.f);
    CHECK(A(1,1) == 2.f);
    CHECK(A.rows() == 3);
    CHECK(A.cols() == 3);
}

TEST_CASE("StaticMatrix 2×2: det", "[matrix][static][det]") {
    StaticMatrix<float, 2, 2> A;
    A(0,0)=1; A(0,1)=2;
    A(1,0)=3; A(1,1)=4;
    CHECK(A.det() == Catch::Approx(-2.f).epsilon(1e-6f));
}

TEST_CASE("StaticMatrix 3×3: det", "[matrix][static][det]") {
    StaticMatrix<float, 3, 3> A;
    A(0,0)=1; A(0,1)=2; A(0,2)=3;
    A(1,0)=0; A(1,1)=1; A(1,2)=4;
    A(2,0)=5; A(2,1)=6; A(2,2)=0;
    CHECK(A.det() == Catch::Approx(1.f).epsilon(1e-5f));
}

TEST_CASE("StaticMatrix 2×2: inverse", "[matrix][static][inv]") {
    StaticMatrix<float, 2, 2> A;
    A(0,0)=4; A(0,1)=7;
    A(1,0)=2; A(1,1)=6;
    auto inv = A.inv();
    // A*inv = I
    auto I = A * inv;
    CHECK(I(0,0) == Catch::Approx(1.f).epsilon(1e-5f));
    CHECK(I(0,1) == Catch::Approx(0.f).margin(1e-5f));
    CHECK(I(1,0) == Catch::Approx(0.f).margin(1e-5f));
    CHECK(I(1,1) == Catch::Approx(1.f).epsilon(1e-5f));
}

TEST_CASE("StaticMatrix: transpose", "[matrix][static][transpose]") {
    StaticMatrix<float, 2, 3> A;
    A(0,0)=1; A(0,1)=2; A(0,2)=3;
    A(1,0)=4; A(1,1)=5; A(1,2)=6;
    auto At = A.transpose();
    REQUIRE(At.rows() == 3);
    REQUIRE(At.cols() == 2);
    CHECK(At(0,1) == 4.f);
    CHECK(At(2,0) == 3.f);
}

TEST_CASE("Matrix: rows/cols match data storage", "[matrix][dense][storage]") {
    Matrix<float> m(5, 7);
    for (std::size_t i=0;i<5;++i)
        for (std::size_t j=0;j<7;++j)
            m(i,j) = float(i*7+j);
    for (std::size_t i=0;i<5;++i)
        for (std::size_t j=0;j<7;++j)
            CHECK(m(i,j) == float(i*7+j));
}
