#include "catch_amalgamated.hpp"
#include "kalpana/color/paint_field.hpp"
#include "kalpana/brush/medium.hpp"
#include "kalpana/color/spectral.hpp"

namespace {
// Helper: read cell (r,c) from PaintField as SpectralColor
kalpana::spectral::SpectralColor cell_pigment(const kalpana::PaintField<>& f,
                                               std::size_t r, std::size_t c) {
    kalpana::spectral::Spectrum sp{};
    for (std::size_t b = 0; b < kalpana::spectral::kBands; ++b)
        sp[b] = f.field().channel(kalpana::PaintChannels::KM_START + b).at(r, c);
    return kalpana::spectral::SpectralColor{sp};
}
float cell_water(const kalpana::PaintField<>& f, std::size_t r, std::size_t c) {
    return f.field().channel(kalpana::PaintChannels::WATER).at(r, c);
}
}

TEST_CASE("PigmentReservoir<1>: single slot round-trip", "[kalpana][reservoir]") {
    using namespace kalpana;
    PaintField<> f(8, 8, 1.0f);

    spectral::SpectralColor cyan = spectral::SpectralColor::from_color(Color{0.0f, 1.0f, 1.0f, 1.0f});
    PaintField<>::SplatParams sp{
        .cx=4, .cy=4, .radius=3, .opacity=1,
        .loading=1, .water_add=0.4f, .height_add=0, .granulation=0,
        .angle=0, .roundness=1, .hardness=1
    };
    f.splat<StampPreset::Round>(sp, cyan);

    PigmentReservoir<1> res;
    auto pigment = cell_pigment(f, 4, 4);
    auto water   = cell_water(f, 4, 4);
    auto color = res.pickup(0, pigment, water, 0.5f);
    float sum = 0.0f;
    for (float v : color.reflectance) sum += v;
    REQUIRE(sum > 0.0f);
}

TEST_CASE("PigmentReservoir<1>: rinse clears slot", "[kalpana][reservoir]") {
    using namespace kalpana;
    PigmentReservoir<1> res;

    PaintField<> f(8, 8, 1.0f);
    spectral::SpectralColor red = spectral::SpectralColor::from_color(Color{1.0f, 0.0f, 0.0f, 1.0f});
    PaintField<>::SplatParams sp{
        .cx=4, .cy=4, .radius=3, .opacity=1,
        .loading=1, .water_add=0.3f, .height_add=0, .granulation=0,
        .angle=0, .roundness=1, .hardness=1
    };
    f.splat<StampPreset::Round>(sp, red);
    (void)res.pickup(0, cell_pigment(f, 4, 4), cell_water(f, 4, 4), 0.8f);

    res.rinse();
    // After rinse, slot volume should be ~0
    auto& slot = res.slots[0];
    REQUIRE(slot.volume < 0.01f);
}

TEST_CASE("PigmentReservoir<3>: multicolor slot mapping", "[kalpana][reservoir]") {
    using namespace kalpana;
    PigmentReservoir<3> res;

    REQUIRE(res.slot_for_u(0.1f)  == 0);
    REQUIRE(res.slot_for_u(0.5f)  == 1);
    REQUIRE(res.slot_for_u(0.9f)  == 2);
    REQUIRE(res.slot_for_u(0.0f)  == 0);
    REQUIRE(res.slot_for_u(1.0f)  == 2);
}

TEST_CASE("PigmentReservoir: deposit blends pigment into cell pigment", "[kalpana][reservoir]") {
    using namespace kalpana;
    PigmentReservoir<1> res;

    spectral::SpectralColor blue = spectral::SpectralColor::from_color(Color{0.0f, 0.0f, 1.0f, 1.0f});
    PaintField<> f(8, 8, 1.0f);
    PaintField<>::SplatParams sp{
        .cx=4, .cy=4, .radius=3, .opacity=1,
        .loading=1, .water_add=0.2f, .height_add=0, .granulation=0,
        .angle=0, .roundness=1, .hardness=1
    };
    f.splat<StampPreset::Round>(sp, blue);
    (void)res.pickup(0, cell_pigment(f, 4, 4), cell_water(f, 4, 4), 0.9f);

    spectral::SpectralColor empty{};
    auto deposited = res.deposit(0, empty, 0.4f);
    float sum = 0.0f;
    for (float v : deposited.reflectance) sum += v;
    REQUIRE(sum > 0.0f);
}
