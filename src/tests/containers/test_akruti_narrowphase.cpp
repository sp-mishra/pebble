// src/tests/containers/test_akruti_narrowphase.cpp — SAT 2-point manifold generation, OBB collision, analytic fast paths.
#include "Catch2-3.9.0/extras/catch_amalgamated.hpp"
#include "akruti/akruti.hpp"
#include <cmath>

using namespace akruti;

TEST_CASE("akruti: analytic circle-circle fast path", "[akruti][narrowphase]") {
    Circle c1{{0, 0}, 1.0f};
    Circle c2{{1.5f, 0}, 1.0f};
    Circle c3{{3.0f, 0}, 1.0f};

    Manifold m1 = collide_circle_circle(c1, c2);
    REQUIRE(m1.hit);
    REQUIRE(m1.depth == Catch::Approx(0.5f).margin(1e-3));
    REQUIRE(m1.normal.x == Catch::Approx(1.0f).margin(1e-3));
    REQUIRE(m1.points.size() == 1);

    Manifold m2 = collide_circle_circle(c1, c3);
    REQUIRE_FALSE(m2.hit);
}

TEST_CASE("akruti: analytic circle-box collision", "[akruti][narrowphase]") {
    Circle c{{0, 1.2f}, 0.5f};
    Box b{{0, 0}, {1.0f, 1.0f}};

    Manifold m = collide_circle_box(c, b);
    REQUIRE(m.hit);
    REQUIRE(m.depth == Catch::Approx(0.3f).margin(1e-3));
}

TEST_CASE("akruti: SAT OBB-OBB 2-point contact manifold", "[akruti][narrowphase][sat]") {
    // Two stacked boxes
    OrientedBox b1{{0, 0}, {1.0f, 1.0f}, akruti::make_rotation2d(0.0f)};
    OrientedBox b2{{0, 1.8f}, {1.0f, 1.0f}, akruti::make_rotation2d(0.0f)};

    Manifold m = collide_obb_obb(b1, b2);
    REQUIRE(m.hit);
    REQUIRE(m.depth == Catch::Approx(0.2f).margin(1e-2));
    REQUIRE(m.normal.y == Catch::Approx(1.0f).margin(1e-2));
    // 2-point manifold generated for face-face contact
    REQUIRE(m.points.size() == 2);
}

TEST_CASE("akruti: SAT rotated OBB collision", "[akruti][narrowphase][sat]") {
    OrientedBox b1{{0, 0}, {1.0f, 1.0f}, akruti::make_rotation2d(0.785398f)}; // 45 deg
    OrientedBox b2{{1.2f, 0}, {0.5f, 0.5f}, akruti::make_rotation2d(0.0f)};

    Manifold m = collide_obb_obb(b1, b2);
    REQUIRE(m.hit);
    REQUIRE(m.depth > 0.0f);
}

TEST_CASE("akruti: warm-started GJK temporal caching", "[akruti][narrowphase][gjk]") {
    Triangle t{{0, 0}, {1, 0}, {0, 1}};
    Circle c{{0.5f, 0.5f}, 0.5f};

    SimplexCache cache;
    REQUIRE_FALSE(cache.valid);

    Manifold m1 = collide_gjk_warm_started(t, c, &cache);
    REQUIRE(m1.hit);
    REQUIRE(cache.valid);

    // Second query with warm cache
    Manifold m2 = collide_gjk_warm_started(t, c, &cache);
    REQUIRE(m2.hit);
    REQUIRE(m2.depth == Catch::Approx(m1.depth).margin(1e-3));
}
