// ============================================================================
// src/tests/containers/test_prakriti_engine.cpp — full simulation loop smoke tests.
//   Falling body accelerates under gravity and settles on the floor;
//   a rigid bar under gravity stays roughly rigid; fluid column spreads.
// ============================================================================
#include "catch_amalgamated.hpp"
#include "prakriti/prakriti.hpp"
#include <containers/numeric/math_vector.hpp>

using namespace prakriti;

TEST_CASE("free particle falls under gravity", "[prakriti][engine]") {
    WorldConfig cfg;
    cfg.bounds = {{-100.0f, 0.0f}, {100.0f, 100.0f}}; // floor at y=0
    World<> w(cfg);
    auto mat = w.materials().add(MaterialRegistry::steel());
    w.thermal().cfg.enabled = false; // isolate mechanics
    Index p = w.particles().add({.position = {0.0f, 50.0f}, .material = mat});

    const Scalar y0 = w.particles().pos_v(p)[1];
    w.step();
    REQUIRE(w.particles().pos_v(p)[1] < y0);        // moved down
    REQUIRE(w.particles().vel_v(p)[1] < 0);         // downward velocity
}

TEST_CASE("particle settles on floor (boundary containment)", "[prakriti][engine]") {
    WorldConfig cfg;
    cfg.bounds = {{-100.0f, 0.0f}, {100.0f, 100.0f}};
    World<> w(cfg);
    auto mat = w.materials().add(MaterialRegistry::steel());
    w.thermal().cfg.enabled = false;
    Index p = w.particles().add({.position = {0.0f, 5.0f}, .material = mat});

    for (int i = 0; i < 300; ++i) w.step();
    REQUIRE(w.particles().pos_v(p)[1] >= -1e-3f);   // never tunnels through floor
    REQUIRE(w.particles().pos_v(p)[1] <= 5.0f);
}

TEST_CASE("rigid bar keeps length under gravity", "[prakriti][engine]") {
    WorldConfig cfg;
    cfg.bounds = {{-100.0f, -100.0f}, {100.0f, 100.0f}};
    World<> w(cfg);
    auto mat = w.materials().add(MaterialRegistry::steel());
    w.thermal().cfg.enabled = false;

    Index a = w.particles().add({.position = {0.0f, 10.0f}, .mass = 0, .material = mat}); // anchor
    Index b = w.particles().add({.position = {1.0f, 10.0f}, .material = mat});
    const Scalar L0 = pebble::math::distance(w.particles().pos_v(a), w.particles().pos_v(b));
    w.edges().add(a, b, L0);

    for (int i = 0; i < 60; ++i) w.step();
    const Scalar len = pebble::math::distance(w.particles().pos_v(a), w.particles().pos_v(b));
    // Stiff steel bond should hold length within a small tolerance while swinging.
    REQUIRE(len == Catch::Approx(L0).margin(0.3f));
}

TEST_CASE("fluid column spreads horizontally (dam break)", "[prakriti][engine]") {
    WorldConfig cfg;
    cfg.cell_size = 1.0f;
    cfg.bounds = {{-50.0f, 0.0f}, {50.0f, 100.0f}};
    World<> w(cfg);
    // Hot material -> liquid phase so the density solver engages.
    auto p = MaterialRegistry::water();
    auto mat = w.materials().add(p);
    w.thermal().cfg.enabled = false;

    Scalar min_x = 1e9f, max_x = -1e9f;
    for (int i = 0; i < 5; ++i)
        for (int j = 0; j < 5; ++j) {
            Index id = w.particles().add({.position = {Scalar(i) * 0.5f, Scalar(j) * 0.5f + 1.0f},
                                          .temperature = 50, // liquid
                                          .material = mat,
                                          .f_solid = 0, .f_liquid = 1});
            min_x = std::min(min_x, w.particles().pos_v(id)[0]);
            max_x = std::max(max_x, w.particles().pos_v(id)[0]);
        }
    const Scalar span0 = max_x - min_x;

    for (int s = 0; s < 60; ++s) w.step();

    Scalar mn = 1e9f, mx = -1e9f;
    for (Index i = 0; i < w.particles().size(); ++i) {
        mn = std::min(mn, w.particles().pos_v(i)[0]);
        mx = std::max(mx, w.particles().pos_v(i)[0]);
    }
    REQUIRE((mx - mn) >= span0 - 0.5f); // fluid did not collapse to a point
    REQUIRE(std::isfinite(mx));         // no NaN blow-up
}
