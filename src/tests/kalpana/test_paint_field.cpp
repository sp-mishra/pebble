#include "catch_amalgamated.hpp"
#include "kalpana/color/paint_field.hpp"
#include "kalpana/color/spectral.hpp"

TEST_CASE (
"PaintField: construction and dimensions"
,
"[kalpana][paint_field]"
)
 {
    using namespace kalpana;
    PaintField<> f(16, 16, 1.0f);
    REQUIRE(f.valid());
    REQUIRE(f.rows() == 16);
    REQUIRE(f.cols() == 16);
}

TEST_CASE (
"PaintField: initial state is zero"
,
"[kalpana][paint_field]"
)
 {
    using namespace kalpana;
    PaintField<> f(8, 8, 1.0f);
    REQUIRE(f.total_mass() == Catch::Approx(0.0f));
    REQUIRE(f.water_mass() == Catch::Approx(0.0f));
}

TEST_CASE (
"PaintField: splat deposits pigment"
,
"[kalpana][paint_field]"
)
 {
    using namespace kalpana;
    PaintField<> f(32, 32, 1.0f);

    spectral::SpectralColor red = spectral::SpectralColor::from_color(Color{1.0f, 0.0f, 0.0f, 1.0f});

    PaintField<>::SplatParams sp{
        .cx = 16.0f, .cy = 16.0f,
        .radius = 5.0f,
        .opacity = 1.0f,
        .loading = 1.0f,
        .water_add = 0.5f,
        .height_add = 0.1f,
        .granulation = 0.0f,
        .angle = 0.0f,
        .roundness = 1.0f,
        .hardness = 0.8f
    };
    f.splat<StampPreset::Round>(sp, red);

    // After splat, some pigment mass must exist
    REQUIRE(f.total_mass() > 0.0f);
    REQUIRE(f.water_mass() > 0.0f);
}

TEST_CASE (
"PaintField: sample returns correct cell"
,
"[kalpana][paint_field]"
)
 {
    using namespace kalpana;
    PaintField<> f(16, 16, 1.0f);

    spectral::SpectralColor blue = spectral::SpectralColor::from_color(Color{0.0f, 0.0f, 1.0f, 1.0f});
    PaintField<>::SplatParams sp{
        .cx = 8.0f, .cy = 8.0f, .radius = 3.0f, .opacity = 1.0f,
        .loading = 1.0f, .water_add = 0.0f, .height_add = 0.0f,
        .granulation = 0.0f, .angle = 0.0f, .roundness = 1.0f, .hardness = 1.0f
    };
    f.splat<StampPreset::Round>(sp, blue);

    auto cell = f.sample(8, 8);
    float km_sum = 0.0f;
    for (std::size_t b = 0; b < 16; ++b) km_sum += cell.km[b];
    REQUIRE(km_sum > 0.0f);
}

TEST_CASE (
"PaintField: resolve_color blends with canvas"
,
"[kalpana][paint_field]"
)
 {
    using namespace kalpana;
    PaintField<> f(8, 8, 1.0f);

    spectral::SpectralColor green = spectral::SpectralColor::from_color(Color{0.0f, 1.0f, 0.0f, 1.0f});
    PaintField<>::SplatParams sp{
        .cx = 4.0f, .cy = 4.0f, .radius = 2.0f, .opacity = 1.0f,
        .loading = 1.0f, .water_add = 0.0f, .height_add = 0.0f,
        .granulation = 0.0f, .angle = 0.0f, .roundness = 1.0f, .hardness = 1.0f
    };
    f.splat<StampPreset::Round>(sp, green);

    Color result = f.resolve_color(4, 4);
    REQUIRE(result.g > result.r);
}
