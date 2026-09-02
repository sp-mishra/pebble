#include <catch_amalgamated.hpp>
#include <containers/tensor/tensor.hpp>
#include <containers/tensor/highway_computation_policy.hpp>
#include <containers/tensor/pravaha_computation_policy.hpp>
#include <cmath>
#include <cstddef>
#include <vector>

// ============================================================================
// test_gemm.cpp — blocked gemm + BLAS primitives correctness
// ============================================================================

using namespace ts;

// Reference naive gemm
template <typename T>
static void naive_gemm(const T* A, const T* B, T* C,
                       std::size_t M, std::size_t N, std::size_t K) {
    for (std::size_t i = 0; i < M; ++i)
        for (std::size_t j = 0; j < N; ++j) {
            T s = T{0};
            for (std::size_t k = 0; k < K; ++k) s += A[i * K + k] * B[k * N + j];
            C[i * N + j] = s;
        }
}

TEST_CASE (
"tensor gemm: DefaultComputationPolicy blocked gemm correctness"
,
"[tensor][gemm][blas]"
)
 {
    SECTION("square 8×8") {
        constexpr std::size_t N = 8;
        DynamicTensor<float> A(TensorShape{N,N}), B(TensorShape{N,N}), C(TensorShape{N,N});
        for (std::size_t i=0;i<N;++i) for (std::size_t j=0;j<N;++j) {
            A.data()[i*N+j] = static_cast<float>(i+j+1);
            B.data()[i*N+j] = static_cast<float>((i*2+j+1));
        }
        DynamicTensor<float> C_ref(TensorShape{N,N});
        std::fill(C.data(), C.data()+N*N, 0.f);
        std::fill(C_ref.data(), C_ref.data()+N*N, 0.f);

        DefaultComputationPolicy::gemm<float>(1.f, A, B, 0.f, C);
        naive_gemm<float>(A.data(), B.data(), C_ref.data(), N, N, N);

        for (std::size_t i=0;i<N*N;++i)
            CHECK(C.data()[i] == Catch::Approx(C_ref.data()[i]).epsilon(1e-4f));
    }

    SECTION("non-square 4×6 × 6×3") {
        constexpr std::size_t M=4, K=6, N=3;
        DynamicTensor<float> A(TensorShape{M,K}), B(TensorShape{K,N}), C(TensorShape{M,N});
        for (std::size_t i=0;i<M*K;++i) A.data()[i] = static_cast<float>(i+1);
        for (std::size_t i=0;i<K*N;++i) B.data()[i] = static_cast<float>(i+2);
        std::fill(C.data(), C.data()+M*N, 0.f);
        DynamicTensor<float> C_ref(TensorShape{M,N});
        std::fill(C_ref.data(), C_ref.data()+M*N, 0.f);
        DefaultComputationPolicy::gemm<float>(1.f, A, B, 0.f, C);
        naive_gemm<float>(A.data(), B.data(), C_ref.data(), M, N, K);
        for (std::size_t i=0;i<M*N;++i)
            CHECK(C.data()[i] == Catch::Approx(C_ref.data()[i]).epsilon(1e-4f));
    }

    SECTION("alpha=2 beta=3 accumulate") {
        constexpr std::size_t N = 4;
        DynamicTensor<float> A(TensorShape{N,N}), B(TensorShape{N,N}), C(TensorShape{N,N});
        for (std::size_t i=0;i<N*N;++i) { A.data()[i]=1.f; B.data()[i]=1.f; C.data()[i]=1.f; }
        DefaultComputationPolicy::gemm<float>(2.f, A, B, 3.f, C);
        // C = 2*A*B + 3*C. A*B = matrix of N each. C was 1s.
        // Each C[i][j] = 2*N + 3 = 2*4+3 = 11
        for (std::size_t i=0;i<N*N;++i)
            CHECK(C.data()[i] == Catch::Approx(11.f).epsilon(1e-5f));
    }
}

TEST_CASE (
"tensor gemm: matmul convenience (alpha=1 beta=0)"
,
"[tensor][gemm][matmul]"
)
 {
    constexpr std::size_t N = 6;
    DynamicTensor<float> A(TensorShape{N,N}), B(TensorShape{N,N});
    for (std::size_t i=0;i<N*N;++i) { A.data()[i]=float(i+1); B.data()[i]=float(N*N-i); }
    auto C = DefaultComputationPolicy::matmul<float>(A, B);
    DynamicTensor<float> C_ref(TensorShape{N,N});
    std::fill(C_ref.data(), C_ref.data()+N*N, 0.f);
    naive_gemm<float>(A.data(), B.data(), C_ref.data(), N, N, N);
    for (std::size_t i=0;i<N*N;++i)
        CHECK(C.data()[i] == Catch::Approx(C_ref.data()[i]).epsilon(1e-4f));
}

TEST_CASE (
"tensor BLAS: gemv correctness"
,
"[tensor][blas][gemv]"
)
 {
    constexpr std::size_t M=5, N=4;
    DynamicTensor<float> A(TensorShape{M,N}), x(TensorShape{N}), y(TensorShape{M});
    for (std::size_t i=0;i<M*N;++i) A.data()[i] = float(i+1);
    for (std::size_t i=0;i<N;++i)   x.data()[i] = float(i+1);
    std::fill(y.data(), y.data()+M, 0.f);
    DefaultComputationPolicy::gemv<float>(1.f, A, x, 0.f, y);
    // reference: y[i] = sum_j A[i][j]*x[j]
    for (std::size_t i=0;i<M;++i) {
        float ref = 0.f;
        for (std::size_t j=0;j<N;++j) ref += A.data()[i*N+j]*x.data()[j];
        CHECK(y.data()[i] == Catch::Approx(ref).epsilon(1e-5f));
    }
}

TEST_CASE (
"tensor BLAS: axpy correctness"
,
"[tensor][blas][axpy]"
)
 {
    constexpr std::size_t N = 8;
    DynamicTensor<float> x(TensorShape{N}), y(TensorShape{N});
    for (std::size_t i=0;i<N;++i) { x.data()[i]=float(i+1); y.data()[i]=float(2*i); }
    DefaultComputationPolicy::axpy<float>(3.f, x, y);
    for (std::size_t i=0;i<N;++i)
        CHECK(y.data()[i] == Catch::Approx(float(2*i) + 3.f*float(i+1)).epsilon(1e-5f));
}

TEST_CASE (
"tensor BLAS: dot and nrm2"
,
"[tensor][blas][dot][nrm2]"
)
 {
    constexpr std::size_t N = 5;
    DynamicTensor<float> x(TensorShape{N}), y(TensorShape{N});
    for (std::size_t i=0;i<N;++i) { x.data()[i]=float(i+1); y.data()[i]=float(i+2); }
    float dot_ref = 0.f;
    for (std::size_t i=0;i<N;++i) dot_ref += x.data()[i]*y.data()[i];

    DynamicTensor<float> vx(TensorShape{N}), vy(TensorShape{N});
    for (std::size_t i=0;i<N;++i) { vx.data()[i]=float(i+1); vy.data()[i]=float(i+2); }
    auto dres = dot(vx, vy); // tensor free-function
    REQUIRE(dres.data()[0] == Catch::Approx(dot_ref).epsilon(1e-5f));

    float nrm_ref = 0.f;
    for (std::size_t i=0;i<N;++i) nrm_ref += x.data()[i]*x.data()[i];
    nrm_ref = std::sqrt(nrm_ref);
    float nrm = DefaultComputationPolicy::nrm2<float>(x);
    CHECK(nrm == Catch::Approx(nrm_ref).epsilon(1e-5f));
}

TEST_CASE (
"tensor BLAS: syrk correctness"
,
"[tensor][blas][syrk]"
)
 {
    constexpr std::size_t M=3, N=4;
    DynamicTensor<float> A(TensorShape{M,N}), C(TensorShape{M,M});
    for (std::size_t i=0;i<M*N;++i) A.data()[i]=float(i+1);
    std::fill(C.data(), C.data()+M*M, 0.f);
    DefaultComputationPolicy::syrk<float>(1.f, A, 0.f, C, true);
    // Reference: C = A·Aᵀ
    for (std::size_t i=0;i<M;++i)
        for (std::size_t j=0;j<M;++j) {
            float ref=0.f;
            for (std::size_t k=0;k<N;++k) ref += A.data()[i*N+k]*A.data()[j*N+k];
            CHECK(C.data()[i*M+j] == Catch::Approx(ref).epsilon(1e-4f));
        }
}

#if __has_include(<hwy/highway.h>)
TEST_CASE ("tensor BLAS: HighwayComputationPolicy gemm matches scalar", "[tensor][gemm][highway]") {
    constexpr std::size_t N = 8;
    using HCP = ts::HighwayComputationPolicy;
    DynamicTensor<float, ts::DefaultStoragePolicy, HCP> A(TensorShape{N,N}), B(TensorShape{N,N}), C(TensorShape{N,N});
    for (std::size_t i=0;i<N*N;++i) { A.data()[i]=float(i%7+1); B.data()[i]=float((i*3)%5+1); }
    std::fill(C.data(), C.data()+N*N, 0.f);
    HCP::gemm<float>(1.f, A, B, 0.f, C);

    DynamicTensor<float> As(TensorShape{N,N}), Bs(TensorShape{N,N}), Cs(TensorShape{N,N});
    for (std::size_t i=0;i<N*N;++i) { As.data()[i]=A.data()[i]; Bs.data()[i]=B.data()[i]; }
    std::fill(Cs.data(), Cs.data()+N*N, 0.f);
    DefaultComputationPolicy::gemm<float>(1.f, As, Bs, 0.f, Cs);
    for (std::size_t i=0;i<N*N;++i)
        CHECK(C.data()[i] == Catch::Approx(Cs.data()[i]).epsilon(1e-3f));
}
#endif
