// src/tests/containers/test_akruti_ccd.cpp — continuous collision (TOI), speculative bound, clip, Voronoi.
#include "Catch2-3.9.0/extras/catch_amalgamated.hpp"
#include "akruti/akruti.hpp"
#include <cmath>
#include <random>

using namespace akruti;
using V = Vec2<Scalar>;

TEST_CASE (
"akruti: conservative-advancement time of impact"
,
"[akruti][ccd]"
)
 {
    Circle a{{0, 0}, 1}, b{{5, 0}, 1};
    TOIResult toi = time_of_impact(a, b, V{-10, 0}); // gap 3, closes at t=0.3
    REQUIRE(toi.hit);
    REQUIRE(toi.t == Catch::Approx(0.3).margin(0.02));
    REQUIRE_FALSE(time_of_impact(a, b, V{10, 0}).hit); // moving away

    Circle c{{1.5f, 0}, 1};
    TOIResult ov = time_of_impact(a, c, V{-1, 0});
    REQUIRE(ov.hit);
    REQUIRE(ov.t == 0);
}

TEST_CASE (
"akruti: speculative fraction bounds motion"
,
"[akruti][ccd]"
)
 {
    REQUIRE(speculative_fraction(3, 10, 1) == Catch::Approx(0.3).margin(0.02));
    REQUIRE(speculative_fraction(3, 0, 1) == Catch::Approx(1.0));
}

TEST_CASE (
"akruti: Sutherland-Hodgman clipping"
,
"[akruti][fracture][clip]"
)
 {
    Poly sq = rect_poly({0, 0}, {1, 1});
    Poly half = clip_halfplane(sq, V{1, 0}, V{0.5f, 0});
    REQUIRE(std::fabs(polygon_area(half)) == Catch::Approx(0.5).margin(1e-3));

    Poly clip = rect_poly({0.25f, 0.25f}, {0.75f, 0.75f});
    Poly inter = clip_polygon(sq, clip);
    REQUIRE(std::fabs(polygon_area(inter)) == Catch::Approx(0.25).margin(1e-3));
}

TEST_CASE (
"akruti: Voronoi shatter conserves area"
,
"[akruti][fracture][voronoi]"
)
 {
    Poly boundary = rect_poly({0, 0}, {1, 1});
    std::mt19937 rng(3);
    std::uniform_real_distribution<float> U(0.05f, 0.95f);
    std::vector<V> seeds;
    for (int i = 0; i < 12; ++i) seeds.push_back({U(rng), U(rng)});
    auto cells = voronoi_shatter(boundary, seeds);
    double total = 0; int nonempty = 0;
    for (auto& c : cells) { total += std::fabs(polygon_area(c)); if (c.size() >= 3) ++nonempty; }
    REQUIRE(total == Catch::Approx(1.0).margin(1e-3));
    REQUIRE(nonempty == 12);
}
