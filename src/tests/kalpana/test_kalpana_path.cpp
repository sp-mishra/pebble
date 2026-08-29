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

TEST_CASE("Kalpana: BasicPath from_svg parses M L C Q Z commands", "[kalpana][geom][path][svg]") {
    // Simple triangle: MoveTo, LineTo x2, Close
    auto p1 = kalpana::Path::from_svg("M 10 20 L 50 80 L 90 20 Z");
    REQUIRE_FALSE(p1.empty());
    // 4 verbs: Move, Line, Line, Close
    REQUIRE(p1.verbs().size() == 4);
    REQUIRE(p1.points().size() == 3);

    // Cubic bezier
    auto p2 = kalpana::Path::from_svg("M 0 0 C 10 30 40 30 50 0");
    REQUIRE(p2.verbs().size() == 2); // Move + Cubic
    REQUIRE(p2.points().size() == 4); // start + 3 cubic control/end points

    // Relative commands
    auto p3 = kalpana::Path::from_svg("m 0 0 l 10 10 l 10 0 z");
    REQUIRE(p3.verbs().size() == 4); // Move, Line, Line, Close

    // Empty string
    auto p4 = kalpana::Path::from_svg("");
    REQUIRE(p4.empty());
}
