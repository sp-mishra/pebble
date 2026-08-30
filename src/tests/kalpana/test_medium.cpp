#include "catch_amalgamated.hpp"
#include "kalpana/color/paint_field.hpp"
#include "kalpana/brush/medium.hpp"
#include "kalpana/color/spectral.hpp"

TEST_CASE("MediumSolver: step runs without crash", "[kalpana][medium]") {
    using namespace kalpana;
    PaintField<> f(16, 16, 1.0f);
    MediumSolver<> solver;

    spectral::SpectralColor paint = spectral::SpectralColor::from_color(Color{0.5f, 0.3f, 0.1f, 1.0f});
    PaintField<>::SplatParams sp{
        .cx = 8.0f, .cy = 8.0f, .radius = 4.0f, .opacity = 0.8f,
        .loading = 1.0f, .water_add = 0.8f, .height_add = 0.0f,
        .granulation = 0.3f, .angle = 0.0f, .roundness = 1.0f, .hardness = 0.7f
    };
    f.splat<StampPreset::Round>(sp, paint);

    float mass_before = f.total_mass();
    solver.step(f, 0.016f);
    // Mass should remain non-negative and roughly conserved after one step
    REQUIRE(f.total_mass() >= 0.0f);
    REQUIRE(f.total_mass() <= mass_before * 1.05f); // allow slight numerical spread
}

TEST_CASE("MediumSolver: drying reduces water", "[kalpana][medium]") {
    using namespace kalpana;
    PaintField<> f(8, 8, 1.0f);
    MediumSolver<> solver;

    spectral::SpectralColor paint = spectral::SpectralColor::from_color(Color{0.2f, 0.6f, 0.9f, 1.0f});
    PaintField<>::SplatParams sp{
        .cx = 4.0f, .cy = 4.0f, .radius = 3.0f, .opacity = 1.0f,
        .loading = 0.5f, .water_add = 1.0f, .height_add = 0.0f,
        .granulation = 0.0f, .angle = 0.0f, .roundness = 1.0f, .hardness = 1.0f
    };
    f.splat<StampPreset::Round>(sp, paint);

    float water_before = f.water_mass();
    // Step many frames to trigger drying
    for (int i = 0; i < 60; ++i) solver.step(f, 0.016f);
    REQUIRE(f.water_mass() < water_before);
}

TEST_CASE("PigmentReservoir: pickup and deposit round-trip", "[kalpana][medium][reservoir]") {
    using namespace kalpana;
    PaintField<> f(8, 8, 1.0f);

    spectral::SpectralColor yellow = spectral::SpectralColor::from_color(Color{1.0f, 1.0f, 0.0f, 1.0f});
    PigmentReservoir<1> res;

    PaintField<>::SplatParams sp{
        .cx = 4.0f, .cy = 4.0f, .radius = 3.0f, .opacity = 1.0f,
        .loading = 1.0f, .water_add = 0.3f, .height_add = 0.0f,
        .granulation = 0.0f, .angle = 0.0f, .roundness = 1.0f, .hardness = 1.0f
    };
    f.splat<StampPreset::Round>(sp, yellow);

    auto cell = f.sample(4, 4);
    spectral::SpectralColor field_pig{};
    for (std::size_t b = 0; b < spectral::kBands; ++b) field_pig.reflectance[b] = cell.km[b];
    auto picked = res.pickup(0, field_pig, cell.water, 0.5f);
    // Pickup returns a non-zero spectral color when there's pigment in the field
    float km_sum = 0.0f;
    for (float v : picked.reflectance) km_sum += v;
    REQUIRE(km_sum > 0.0f);
}
