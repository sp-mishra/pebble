#include "catch_amalgamated.hpp"
#include "akruti/akruti.hpp"
#include "akruti/simd.hpp"
#include <vector>

TEST_CASE("Akruti: SIMD Vectorized Polygon Support Point Sweep", "[akruti][simd]") {
    akruti::ConvexPoly<8> poly;
    // Regular octagon
    for (int i = 0; i < 8; ++i) {
        float angle = static_cast<float>(i) * 3.14159265f / 4.0f;
        (void)poly.verts.push_back({std::cos(angle) * 10.0f, std::sin(angle) * 10.0f});
    }

    // Direction along +X
    auto sup_x = akruti::simd::vectorized_support_poly(poly, {1.0f, 0.0f});
    REQUIRE(sup_x.x == Catch::Approx(10.0f).margin(1e-3f));

    // Direction along +Y
    auto sup_y = akruti::simd::vectorized_support_poly(poly, {0.0f, 1.0f});
    REQUIRE(sup_y.y == Catch::Approx(10.0f).margin(1e-3f));

    // Diagonal direction
    auto sup_diag = akruti::simd::vectorized_support_poly(poly, {1.0f, 1.0f});
    REQUIRE(sup_diag.x > 5.0f);
    REQUIRE(sup_diag.y > 5.0f);
}

TEST_CASE("Akruti: SIMD 4-Ray Packet AABB Intersection", "[akruti][simd]") {
    akruti::simd::Ray4 rays;
    rays.o[0] = {-10.0f, 0.0f};  rays.d[0] = {1.0f, 0.0f}; rays.tmax[0] = 100.0f; // Direct hit
    rays.o[1] = {-10.0f, 10.0f}; rays.d[1] = {1.0f, 0.0f}; rays.tmax[1] = 100.0f; // Miss (above)
    rays.o[2] = {0.0f, -10.0f};  rays.d[2] = {0.0f, 1.0f}; rays.tmax[2] = 100.0f; // Hit from bottom
    rays.o[3] = {-10.0f, 0.0f};  rays.d[3] = {-1.0f, 0.0f}; rays.tmax[3] = 100.0f; // Facing away

    akruti::AABB<float> box{{-2.0f, -2.0f}, {2.0f, 2.0f}};
    auto hits = akruti::simd::packet_raycast_aabb(rays, box);

    REQUIRE(hits.hit[0] == true);
    REQUIRE(hits.t[0] == Catch::Approx(8.0f).margin(1e-3f));

    REQUIRE(hits.hit[1] == false);

    REQUIRE(hits.hit[2] == true);
    REQUIRE(hits.t[2] == Catch::Approx(8.0f).margin(1e-3f));

    REQUIRE(hits.hit[3] == false);
}
