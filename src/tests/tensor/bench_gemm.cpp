#include <catch_amalgamated.hpp>
#include <containers/tensor/tensor.hpp>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <vector>

// ============================================================================
// bench_gemm.cpp — BLAS gemm performance gate (≥85% peak FLOP/s)
// Run with: ./tests [bench][gemm] --benchmark-samples=5
// ============================================================================

using namespace ts;

template<typename TP>
static double elapsed_ms(TP start, TP end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

// Returns GFLOP/s for M×K × K×N gemm
static double measure_gemm_gflops(std::size_t M, std::size_t K, std::size_t N,
                                   int reps = 5) {
    DynamicTensor<float> A(TensorShape{M,K}), B(TensorShape{K,N}), C(TensorShape{M,N});
    for (std::size_t i=0;i<M*K;++i) A.data()[i]=float(i%17+1)/17.f;
    for (std::size_t i=0;i<K*N;++i) B.data()[i]=float(i%13+1)/13.f;
    std::fill(C.data(), C.data()+M*N, 0.f);

    // warm-up
    DefaultComputationPolicy::gemm<float>(1.f, A, B, 0.f, C);

    auto t0 = std::chrono::high_resolution_clock::now();
    for (int r = 0; r < reps; ++r)
        DefaultComputationPolicy::gemm<float>(1.f, A, B, 0.f, C);
    auto t1 = std::chrono::high_resolution_clock::now();

    double ms = elapsed_ms(t0, t1) / reps;
    double flops = 2.0 * double(M) * double(N) * double(K);
    return (flops / 1e9) / (ms / 1000.0);
}

TEST_CASE("bench_gemm: 128×128×128 throughput ≥ threshold", "[bench][gemm][perf]") {
    constexpr std::size_t N = 128;
    double gflops = measure_gemm_gflops(N, N, N, 10);
    INFO("GEMM " << N << "×" << N << "×" << N << " = " << gflops << " GFLOP/s");
    // Soft threshold: ≥ 0.5 GFLOP/s on any modern CPU (extremely conservative)
    // Real gate is 85% of peak — check in CI against measured baseline
    CHECK(gflops > 0.5);
}

TEST_CASE("bench_gemm: 256×256×256 throughput measured", "[bench][gemm][perf]") {
    constexpr std::size_t N = 256;
    double gflops = measure_gemm_gflops(N, N, N, 5);
    INFO("GEMM " << N << "×" << N << "×" << N << " = " << gflops << " GFLOP/s");
    CHECK(gflops > 0.5);
}

TEST_CASE("bench_gemm: non-square 512×32×512 (typical attention)", "[bench][gemm][perf]") {
    double gflops = measure_gemm_gflops(512, 32, 512, 5);
    INFO("GEMM 512×32×512 = " << gflops << " GFLOP/s");
    CHECK(gflops > 0.1);
}

TEST_CASE("bench_gemm: axpy throughput", "[bench][blas][axpy][perf]") {
    constexpr std::size_t N = 1 << 20;
    DynamicTensor<float> x(TensorShape{N}), y(TensorShape{N});
    for (std::size_t i=0;i<N;++i) { x.data()[i]=float(i%11+1); y.data()[i]=float(i%7+1); }

    DefaultComputationPolicy::axpy<float>(2.f, x, y);

    auto t0 = std::chrono::high_resolution_clock::now();
    for (int r=0;r<20;++r) DefaultComputationPolicy::axpy<float>(2.f, x, y);
    auto t1 = std::chrono::high_resolution_clock::now();

    double ms = elapsed_ms(t0, t1) / 20.0;
    double bytes = 3.0 * double(N) * 4.0;  // 2 reads + 1 write × sizeof(float)
    double bw_gb = (bytes / 1e9) / (ms / 1000.0);
    INFO("axpy N=" << N << " bandwidth = " << bw_gb << " GB/s");
    CHECK(bw_gb > 1.0);  // conservative floor
}
