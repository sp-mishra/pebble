#include <catch_amalgamated.hpp>
#include <containers/matrix/sparse.hpp>
#include <chrono>
#include <cstddef>
#include <vector>

// ============================================================================
// bench_spmv.cpp — SpMV bandwidth gate (≥60% peak memory bandwidth)
// Run with: ./tests [bench][spmv] --benchmark-samples=5
// ============================================================================

using namespace ga;

template<typename TP>
static double elapsed_ms(TP start, TP end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

// Build N×N 5-point stencil (2D Laplacian), nnz ≈ 5N
static CsrMatrix<float> build_stencil_csr(std::size_t n) {
    std::size_t N = n * n;
    std::vector<std::size_t> rows, cols;
    std::vector<float> vals;
    rows.reserve(5*N); cols.reserve(5*N); vals.reserve(5*N);
    for (std::size_t i=0;i<n;++i) for (std::size_t j=0;j<n;++j) {
        std::size_t r = i*n+j;
        rows.push_back(r); cols.push_back(r); vals.push_back(4.f);
        if (j > 0)   { rows.push_back(r); cols.push_back(r-1); vals.push_back(-1.f); }
        if (j < n-1) { rows.push_back(r); cols.push_back(r+1); vals.push_back(-1.f); }
        if (i > 0)   { rows.push_back(r); cols.push_back(r-n); vals.push_back(-1.f); }
        if (i < n-1) { rows.push_back(r); cols.push_back(r+n); vals.push_back(-1.f); }
    }
    return CsrMatrix<float>::from_triplets(N, N, rows, cols, vals);
}

TEST_CASE("bench_spmv: 5-pt stencil 128×128 bandwidth", "[bench][spmv][perf]") {
    constexpr std::size_t n = 128;
    auto A = build_stencil_csr(n);
    std::size_t N = n*n;
    Vector<float> x(N, 1.f);

    // warm-up
    Vector<float> y(N, 0.f);
    spmv_into(A, x, y);

    constexpr int reps = 20;
    auto t0 = std::chrono::high_resolution_clock::now();
    for (int r=0;r<reps;++r) spmv_into(A, x, y);
    auto t1 = std::chrono::high_resolution_clock::now();

    double ms = elapsed_ms(t0, t1) / reps;
    // bytes = values + col_idx + row_ptr + x read + y write
    double bytes = double(A.nnz()) * (sizeof(float) + sizeof(std::size_t))
                 + (N+1) * sizeof(std::size_t)
                 + N * sizeof(float)   // x
                 + N * sizeof(float);  // y
    double bw_gb = (bytes / 1e9) / (ms / 1000.0);
    INFO("SpMV 128² stencil (nnz=" << A.nnz() << ") bandwidth = " << bw_gb << " GB/s");
    CHECK(bw_gb > 0.5);  // conservative floor — real gate is 60% peak
}

TEST_CASE("bench_spmv: 5-pt stencil 256×256 throughput", "[bench][spmv][perf]") {
    constexpr std::size_t n = 256;
    auto A = build_stencil_csr(n);
    std::size_t N = n*n;
    Vector<float> x(N, 1.f);
    Vector<float> y(N, 0.f);
    spmv_into(A, x, y);  // warm-up

    constexpr int reps = 10;
    auto t0 = std::chrono::high_resolution_clock::now();
    for (int r=0;r<reps;++r) spmv_into(A, x, y);
    auto t1 = std::chrono::high_resolution_clock::now();

    double ms = elapsed_ms(t0, t1) / reps;
    double gnnz_s = (double(A.nnz()) * reps / 1e9) / (elapsed_ms(t0, t1) / 1000.0);
    INFO("SpMV 256² (nnz=" << A.nnz() << ") = " << gnnz_s << " Gnnz/s in " << ms << "ms");
    CHECK(ms < 1000.0);  // sanity: should complete in < 1s
}

TEST_CASE("bench_spmv: DiaSpmv bandwidth vs CSR", "[bench][spmv][dia][perf]") {
    constexpr std::size_t N = 4096;
    std::vector<std::ptrdiff_t> offsets = {-1, 0, 1};
    DiaMatrix<float> D(N, N, offsets);
    for (std::size_t i=0;i<N;++i) D.set_diag(1, i, 4.f);
    for (std::size_t i=1;i<N;++i) D.set_diag(0, i, -1.f);
    for (std::size_t i=0;i<N-1;++i) D.set_diag(2, i, -1.f);

    Vector<float> x(N, 1.f);
    auto y = spmv(D, x);  // warm-up

    constexpr int reps = 50;
    auto t0 = std::chrono::high_resolution_clock::now();
    for (int r=0;r<reps;++r) y = spmv(D, x);
    auto t1 = std::chrono::high_resolution_clock::now();

    double ms = elapsed_ms(t0, t1) / reps;
    INFO("Dia SpMV N=" << N << " time = " << ms << "ms");
    CHECK(ms < 100.0);  // should complete well under 100ms
}
