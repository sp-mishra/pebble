#include "catch_amalgamated.hpp"
#include "kalpana/color/paint_field.hpp"
#include "kalpana/effect/liquify.hpp"
#include "kalpana/color/spectral.hpp"

TEST_CASE("LiquifyBrush: construction and valid state", "[kalpana][liquify]") {
    using namespace kalpana;
    LiquifyBrush<> brush(16, 16, 1.0f, LiquifyParams{.mode = LiquifyMode::Push});
    REQUIRE(brush.valid());
}

TEST_CASE("LiquifyBrush: Push mode accumulates displacement", "[kalpana][liquify]") {
    using namespace kalpana;
    LiquifyBrush<> brush(16, 16, 1.0f, LiquifyParams{.mode = LiquifyMode::Push, .strength = 1.0f, .radius = 4.0f});

    brush.accumulate(8.0f, 8.0f, 3.0f, 0.0f); // push right

    const auto& disp = brush.displacement_field();
    // Displacement at center should be positive x
    REQUIRE(disp.channel(0).at(8, 8) > 0.0f);
}

TEST_CASE("LiquifyBrush: clear resets displacement to zero", "[kalpana][liquify]") {
    using namespace kalpana;
    LiquifyBrush<> brush(8, 8, 1.0f);
    brush.accumulate(4.0f, 4.0f, 2.0f, 1.0f);
    brush.clear();

    const auto& disp = brush.displacement_field();
    for (std::size_t r = 0; r < 8; ++r)
        for (std::size_t c = 0; c < 8; ++c)
            REQUIRE(disp.channel(0).at(r, c) == Catch::Approx(0.0f));
}

TEST_CASE("LiquifyBrush: apply does not crash on valid field", "[kalpana][liquify]") {
    using namespace kalpana;
    PaintField<> f(16, 16, 1.0f);
    spectral::SpectralColor paint = spectral::SpectralColor::from_color(Color{0.5f, 0.3f, 0.8f, 1.0f});
    PaintField<>::SplatParams sp{
        .cx=8, .cy=8, .radius=4, .opacity=1, .loading=1,
        .water_add=0.2f, .height_add=0, .granulation=0,
        .angle=0, .roundness=1, .hardness=1
    };
    f.splat<StampPreset::Round>(sp, paint);
    float mass_before = f.total_mass();

    LiquifyBrush<> brush(16, 16, 1.0f, LiquifyParams{.mode = LiquifyMode::Push, .strength = 0.5f, .radius = 4.0f});
    brush.accumulate(8.0f, 8.0f, 2.0f, 0.0f);
    brush.apply(f);

    // Mass should be conserved (backward warp cannot amplify)
    REQUIRE(f.total_mass() <= mass_before * (1.0f + 0.002f));
}

TEST_CASE("LiquifyBrush: Twirl mode displacement is perpendicular", "[kalpana][liquify]") {
    using namespace kalpana;
    LiquifyBrush<> brush(16, 16, 1.0f, LiquifyParams{.mode = LiquifyMode::Twirl, .strength = 1.0f, .radius = 6.0f});
    brush.accumulate(8.0f, 8.0f);

    // For a cell to the right of center (same row), Twirl should give dy displacement, not dx
    const auto& disp = brush.displacement_field();
    float dx = disp.channel(0).at(8, 10);
    float dy = disp.channel(1).at(8, 10);
    // Twirl: dx is driven by -v (should be ~0 at same row), dy by u (positive)
    REQUIRE(std::abs(dy) > std::abs(dx) * 0.5f);
}
