// bench_medium.cpp — micro-benchmark: MediumSolver::step throughput
// Run with: [kalpana][bench][medium] tag
// Not a correctness test — measures wall time for perf budgeting.

#include "catch_amalgamated.hpp"
#include "kalpana/color/paint_field.hpp"
#include "kalpana/brush/medium.hpp"
#include "kalpana/color/spectral.hpp"
#include <chrono>

TEST_CASE (
"Bench MediumSolver::step on 256x256 field"
,
"[kalpana][bench][medium][!benchmark]"
)
 {
    using namespace kalpana;

    PaintField<> f(256, 256, 1.0f);
    MediumSolver<> solver;

    spectral::SpectralColor paint = spectral::SpectralColor::from_color(Color{0.4f, 0.2f, 0.8f, 1.0f});
    PaintField<>::SplatParams sp{
        .cx=128, .cy=128, .radius=60, .opacity=0.8f,
        .loading=1.0f, .water_add=0.9f, .height_add=0.0f,
        .granulation=0.3f, .angle=0.0f, .roundness=1.0f, .hardness=0.7f
    };
    f.splat<StampPreset::Round>(sp, paint);

    constexpr int WARMUP = 3, RUNS = 10;
    for (int i = 0; i < WARMUP; ++i) solver.step(f, 0.016f);

    auto t0 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < RUNS; ++i) solver.step(f, 0.016f);
    auto t1 = std::chrono::high_resolution_clock::now();

    double ms_per_step = std::chrono::duration<double, std::milli>(t1 - t0).count() / RUNS;
    WARN("MediumSolver::step 256x256: " << ms_per_step << " ms/step");
    // Budget: ≤16ms/step for 60fps real-time (constraint from §perf)
    REQUIRE(ms_per_step < 16.0 * 10); // relaxed: 10x budget for CI
}
