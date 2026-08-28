#include "catch_amalgamated.hpp"
#include "akruti/akruti.hpp"
#include <cmath>

TEST_CASE("Akruti: ChainShape Open Polyline Contract", "[akruti][chain]") {
    akruti::ChainShape<4> chain;
    (void)chain.verts.push_back({0.0f, 0.0f});
    (void)chain.verts.push_back({10.0f, 0.0f});
    (void)chain.verts.push_back({10.0f, 10.0f});
    chain.is_loop = false;
    chain.radius = 0.5f;

    // Test AABB
    auto box = chain.aabb();
    REQUIRE(box.lo[0] <= -0.5f);
    REQUIRE(box.hi[0] >= 10.5f);
    REQUIRE(box.lo[1] <= -0.5f);
    REQUIRE(box.hi[1] >= 10.5f);

    // Test SDF on vertices and segments
    float d_on_seg1 = chain.sdf({5.0f, 0.0f});
    REQUIRE(d_on_seg1 == Catch::Approx(-0.5f).margin(1e-4f));

    float d_on_seg2 = chain.sdf({10.0f, 5.0f});
    REQUIRE(d_on_seg2 == Catch::Approx(-0.5f).margin(1e-4f));

    float d_outside = chain.sdf({5.0f, 2.0f});
    REQUIRE(d_outside == Catch::Approx(1.5f).margin(1e-4f));

    // Test Support function
    auto sup_x = chain.support({1.0f, 0.0f});
    REQUIRE(sup_x.x >= 10.5f);

    auto sup_y = chain.support({0.0f, 1.0f});
    REQUIRE(sup_y.y >= 10.5f);
}

TEST_CASE("Akruti: ChainShape Closed Loop and Ghost Vertices", "[akruti][chain]") {
    akruti::ChainShape<4> loop;
    (void)loop.verts.push_back({0.0f, 0.0f});
    (void)loop.verts.push_back({10.0f, 0.0f});
    (void)loop.verts.push_back({10.0f, 10.0f});
    (void)loop.verts.push_back({0.0f, 10.0f});
    loop.is_loop = true;
    loop.radius = 0.0f;

    // Closing edge from (0, 10) to (0, 0)
    float d_closing = loop.sdf({0.0f, 5.0f});
    REQUIRE(d_closing == Catch::Approx(0.0f).margin(1e-4f));

    // Edge normal check
    auto n0 = loop.edge_normal(0); // (0,0) -> (10,0), normal should be (0, -1) or (0, 1) depending on perp
    REQUIRE(std::fabs(n0.y) == Catch::Approx(1.0f).margin(1e-4f));
}
