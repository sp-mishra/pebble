#include "catch_amalgamated.hpp"
#include "kalpana/kalpana.hpp"
#include <vector>

TEST_CASE("Kalpana: Akruti ChainShape and CatmullRomSpline to Vector Path", "[kalpana][geom][path]") {
    // 1. Akruti Catmull-Rom Spline import
    akruti::CatmullRomSpline spline;
    (void)spline.points.push_back({0.0f, 0.0f});
    (void)spline.points.push_back({10.0f, 20.0f});
    (void)spline.points.push_back({20.0f, 0.0f});
    spline.closed = true;

    auto p_spline = kalpana::Path::from_catmull_rom(spline);
    REQUIRE(p_spline.verbs().size() == 4); // Move + 2 Lines + Close
    REQUIRE(p_spline.points().size() == 3);

    // 2. Akruti ChainShape import
    akruti::ChainShape<4> chain;
    (void)chain.verts.push_back({0.0f, 0.0f});
    (void)chain.verts.push_back({50.0f, 0.0f});
    (void)chain.verts.push_back({50.0f, 50.0f});
    chain.is_loop = false;

    auto p_chain = kalpana::Path::from_chain(chain);
    REQUIRE(p_chain.verbs().size() == 3); // Move + 2 Lines
    REQUIRE(p_chain.points().size() == 3);
}
