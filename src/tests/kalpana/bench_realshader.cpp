// bench_realshader.cpp — micro-benchmark: RealShaderPass::shade_cell throughput
#include "catch_amalgamated.hpp"
#include "kalpana/color/paint_field.hpp"
#include "kalpana/color/realshader.hpp"
#include "kalpana/brush/material.hpp"
#include "kalpana/color/spectral.hpp"
#include "containers/numeric/math_vector.hpp"
#include <chrono>

TEST_CASE("Bench RealShaderPass shade_cell on 256x256", "[kalpana][bench][realshader][!benchmark]") {
    using namespace kalpana;

    PaintField<> f(256, 256, 1.0f);
    spectral::SpectralColor paint = spectral::SpectralColor::from_color(Color{0.6f, 0.4f, 0.2f, 1.0f});
    PaintField<>::SplatParams sp{
        .cx=128, .cy=128, .radius=80, .opacity=0.9f,
        .loading=1.0f, .water_add=0.0f, .height_add=0.5f,
        .granulation=0.0f, .angle=0.0f, .roundness=1.0f, .hardness=1.0f
    };
    f.splat<StampPreset::Round>(sp, paint);

    RealShaderPass<> shader;
    PaintMaterial mat = PaintMaterial::preset_glossy_oil();
    Color base{0.6f, 0.4f, 0.2f, 1.0f};
    pebble::math::vec3 view{0.0f, 0.0f, 1.0f};

    constexpr int WARMUP = 2, RUNS = 3;
    for (int w = 0; w < WARMUP; ++w)
        for (std::size_t r = 0; r < 256; ++r)
            for (std::size_t c = 0; c < 256; ++c)
                (void)shader.shade_cell(f, r, c, base, mat, view);

    auto t0 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < RUNS; ++i)
        for (std::size_t r = 0; r < 256; ++r)
            for (std::size_t c = 0; c < 256; ++c)
                (void)shader.shade_cell(f, r, c, base, mat, view);
    auto t1 = std::chrono::high_resolution_clock::now();

    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count() / RUNS;
    WARN("RealShaderPass 256x256 full shade: " << ms << " ms/frame");
    // Budget: ≤100ms for offline render (256² = 65536 cells)
    REQUIRE(ms < 1000.0); // very relaxed for CI
}
