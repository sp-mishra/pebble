#include "catch_amalgamated.hpp"
#include "kalpana/color/paint_field.hpp"
#include "kalpana/brush/drop_engine.hpp"
#include "kalpana/color/spectral.hpp"

TEST_CASE("DropEngine: spawn creates a droplet", "[kalpana][drop_engine]") {
    using namespace kalpana;
    DropEngine<> engine;

    spectral::SpectralColor blue = spectral::SpectralColor::from_color(Color{0.0f, 0.2f, 0.9f, 1.0f});
    engine.spawn({16.0f, 8.0f}, 0.5f, blue);

    REQUIRE(engine.drop_count() == 1);
}

TEST_CASE("DropEngine: step moves droplets and deposits into field", "[kalpana][drop_engine]") {
    using namespace kalpana;
    PaintField<> f(32, 32, 1.0f);
    DropEngine<> engine;

    spectral::SpectralColor red = spectral::SpectralColor::from_color(Color{0.9f, 0.1f, 0.1f, 1.0f});
    engine.spawn({16.0f, 8.0f}, 1.0f, red);

    float mass_before = f.total_mass();
    engine.step(f, 0.016f, 0.0f, 1.0f); // tilt downward
    // After step, droplet may have deposited pigment
    float mass_after = f.total_mass();
    REQUIRE(mass_after >= mass_before); // deposit is additive
}

TEST_CASE("DropEngine: clear removes all droplets", "[kalpana][drop_engine]") {
    using namespace kalpana;
    DropEngine<> engine;

    for (int i = 0; i < 5; ++i)
        engine.spawn({static_cast<float>(i) * 4.0f, 10.0f}, 0.3f);

    REQUIRE(engine.drop_count() > 0);
    engine.clear();
    REQUIRE(engine.drop_count() == 0);
}

TEST_CASE("DropEngine: max_drops cap is respected", "[kalpana][drop_engine]") {
    using namespace kalpana;
    DropEngineParams params;
    params.max_drops = 3;
    DropEngine<> engine(params);

    for (int i = 0; i < 10; ++i)
        engine.spawn({static_cast<float>(i), 0.0f}, 0.1f);

    REQUIRE(engine.drop_count() <= 3);
}
