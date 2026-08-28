// src/tests/containers/test_akruti_primitives.cpp — akruti SDF primitives, queries, and convex hull.
#include "Catch2-3.9.0/extras/catch_amalgamated.hpp"
#include "akruti/akruti.hpp"
#include <cmath>

using namespace akruti;
using V = Vec2<Scalar>;

TEST_CASE("akruti: primitive SDFs sign correctly", "[akruti][primitives]") {
    Circle c{{0, 0}, 1};
    REQUIRE(c.sdf({0, 0}) == Catch::Approx(-1.0));
    REQUIRE(c.sdf({1, 0}) == Catch::Approx(0.0));
    REQUIRE(c.sdf({2, 0}) == Catch::Approx(1.0));

    Box b{{0, 0}, {1, 1}};
    REQUIRE(b.sdf({0, 0}) < 0);
    REQUIRE(b.sdf({2, 0}) == Catch::Approx(1.0));

    HalfPlane h{{0, 1}, {0, 0}};
    REQUIRE(h.sdf({0, 1}) == Catch::Approx(1.0));
    REQUIRE(h.sdf({0, -1}) == Catch::Approx(-1.0));
}

TEST_CASE("akruti: point_inside and closest_point", "[akruti][query]") {
    Circle c{{0, 0}, 1};
    REQUIRE(point_inside(c, V{0.5f, 0}));
    REQUIRE_FALSE(point_inside(c, V{2, 0}));
    V cp = closest_point(c, V{3, 0});
    REQUIRE(cp.x == Catch::Approx(1.0).margin(0.01));
    REQUIRE(cp.y == Catch::Approx(0.0).margin(0.01));
}

TEST_CASE("akruti: SDF raycast hits and misses", "[akruti][query]") {
    Circle c{{5, 0}, 1};
    RayHit h = raycast(c, V{0, 0}, V{1, 0});
    REQUIRE(h.hit);
    REQUIRE(h.t == Catch::Approx(4.0).margin(0.02));
    REQUIRE(h.normal.x == Catch::Approx(-1.0).margin(0.02));
    REQUIRE_FALSE(raycast(c, V{0, 10}, V{1, 0}, 20).hit);
}

TEST_CASE("akruti: convex hull drops interior points", "[akruti][hull]") {
    containers::static_vector<V, 8> pts;
    (void)pts.push_back({0, 0}); (void)pts.push_back({1, 0});
    (void)pts.push_back({1, 1}); (void)pts.push_back({0, 1});
    (void)pts.push_back({0.5f, 0.5f});
    auto h = convex_hull<8>(pts);
    REQUIRE(h.verts.size() == 4);
}
