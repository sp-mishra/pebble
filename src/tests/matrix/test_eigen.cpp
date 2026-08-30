#include <catch_amalgamated.hpp>
#include <containers/matrix/ganita.hpp>
#include <cmath>
#include <algorithm>

using namespace ga;

// ============================================================================
// test_eigen.cpp — eigenvalue / SVD / rSVD / Lanczos
// ============================================================================

static Matrix<double> make_sym_spd(std::size_t n) {
    Matrix<double> A(n, n, 0.0);
    for (std::size_t i=0;i<n;++i) {
        A(i,i) = double(2*n);
        if (i > 0)   A(i,i-1) = A(i-1,i) = -1.0;
    }
    return A;
}

TEST_CASE("eig_sym: eigenvalues of 1D Laplacian are positive", "[eigen][eig_sym]") {
    constexpr std::size_t N = 4;
    auto A = make_sym_spd(N);
    auto res = eig_sym(A, 1e-12, 200);
    REQUIRE(res.eigenvalues.size() == N);
    for (auto lam : res.eigenvalues) CHECK(lam > 0.0);
}

TEST_CASE("eig_sym: 2×2 known eigenvalues", "[eigen][eig_sym]") {
    // A = [[3,1],[1,3]] → eigenvalues 2, 4
    Matrix<double> A(2, 2);
    A(0,0)=3; A(0,1)=1; A(1,0)=1; A(1,1)=3;
    auto res = eig_sym(A, 1e-12, 100);
    REQUIRE(res.eigenvalues.size() == 2);
    auto evs = res.eigenvalues;
    std::sort(evs.begin(), evs.end());
    CHECK(evs[0] == Catch::Approx(2.0).epsilon(1e-8));
    CHECK(evs[1] == Catch::Approx(4.0).epsilon(1e-8));
}

TEST_CASE("eig_sym: eigenvectors are orthonormal", "[eigen][eig_sym][orthonormal]") {
    Matrix<double> A(3, 3);
    A(0,0)=4; A(0,1)=2; A(0,2)=0;
    A(1,0)=2; A(1,1)=5; A(1,2)=2;
    A(2,0)=0; A(2,1)=2; A(2,2)=6;
    auto res = eig_sym(A, 1e-12, 200);
    // V^T V = I
    const std::size_t N = 3;
    for (std::size_t i=0;i<N;++i)
        for (std::size_t j=0;j<N;++j) {
            double dot = 0;
            for (std::size_t k=0;k<N;++k) dot += res.eigenvectors(k,i)*res.eigenvectors(k,j);
            if (i==j) CHECK(dot == Catch::Approx(1.0).epsilon(1e-6));
            else       CHECK(dot == Catch::Approx(0.0).margin(1e-6));
        }
}

TEST_CASE("svd: singular values of 3×2 matrix", "[eigen][svd]") {
    Matrix<double> A(3, 2);
    A(0,0)=1; A(0,1)=2;
    A(1,0)=3; A(1,1)=4;
    A(2,0)=5; A(2,1)=6;
    auto res = svd(A, 1e-12);
    REQUIRE(res.sigma.size() == 2);
    // sigma sorted descending, all positive
    CHECK(res.sigma[0] > 0.0);
    CHECK(res.sigma[1] > 0.0);
    CHECK(res.sigma[0] >= res.sigma[1]);
    // known singular values of this matrix: sqrt(eigenvalues of AᵀA)
    // AᵀA = [[35,44],[44,56]], eigenvalues ≈ 90.74 and 0.264
    CHECK(res.sigma[0] == Catch::Approx(9.52552).epsilon(0.01));
    CHECK(res.sigma[1] == Catch::Approx(0.51450).epsilon(0.02));
}

TEST_CASE("svd: U and Vt are orthogonal", "[eigen][svd][orthogonal]") {
    Matrix<double> A(4, 3);
    for (std::size_t i=0;i<4;++i) for (std::size_t j=0;j<3;++j) A(i,j)=double(i*3+j+1);
    auto res = svd(A, 1e-12);
    // U^T U = I (4×3 → U is 4×3, k=min(4,3)=3)
    std::size_t k = res.sigma.size();
    for (std::size_t i=0;i<k;++i)
        for (std::size_t j=0;j<k;++j) {
            double dot=0;
            for (std::size_t r=0;r<4;++r) dot += res.U(r,i)*res.U(r,j);
            if (i==j) CHECK(dot == Catch::Approx(1.0).epsilon(0.01));
            else       CHECK(dot == Catch::Approx(0.0).margin(0.01));
        }
}

TEST_CASE("rsvd: truncated k=2 on 5×5 rank-2 matrix", "[eigen][rsvd]") {
    // Build A = u1*s1*v1^T + u2*s2*v2^T (rank 2)
    constexpr std::size_t N = 5;
    Matrix<double> A(N, N, 0.0);
    for (std::size_t i=0;i<N;++i) A(i,i) = (i<2) ? double(i+2)*2.0 : 0.0;
    // A = diag(4,6,0,0,0) — rank 2
    auto res = rsvd(A, 2, 5, 2, 1e-10, 42);
    REQUIRE(res.truncated);
    REQUIRE(res.sigma.size() == 2);
    CHECK(res.sigma[0] > 0.0);
    CHECK(res.sigma[1] > 0.0);
}

TEST_CASE("power_iteration: dominant eigenvalue of diag matrix", "[eigen][power_iteration]") {
    constexpr std::size_t N = 4;
    // Apply = diag(5, 3, 1, 0.5)·x
    auto apply = [](const Vector<double>& x) -> Vector<double> {
        Vector<double> y(4);
        y[0]=5.0*x[0]; y[1]=3.0*x[1]; y[2]=1.0*x[2]; y[3]=0.5*x[3];
        return y;
    };
    auto [lam, v] = power_iteration(apply, N, 1e-10, 1000);
    CHECK(lam == Catch::Approx(5.0).epsilon(1e-6));
}

TEST_CASE("lanczos_eig: largest eigenvalue of Laplacian", "[eigen][lanczos]") {
    constexpr std::size_t N = 8;
    auto A = make_sym_spd(N);
    auto apply = [&](const Vector<double>& x) -> Vector<double> {
        Vector<double> y(N, 0.0);
        for (std::size_t i=0;i<N;++i) {
            y[i] += A(i,i)*x[i];
            if (i > 0) y[i] += A(i,i-1)*x[i-1];
            if (i < N-1) y[i] += A(i,i+1)*x[i+1];
        }
        return y;
    };
    auto res = lanczos_eig(apply, N, 3, 1e-8, 200);
    REQUIRE(!res.eigenvalues.empty());
    // All eigenvalues should be positive
    for (auto lam : res.eigenvalues) CHECK(lam > 0.0);
}
