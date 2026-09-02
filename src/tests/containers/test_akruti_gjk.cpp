// src/tests/containers/test_akruti_gjk.cpp — GJK overlap, EPA penetration, GJK distance.
#include "Catch2-3.9.0/extras/catch_amalgamated.hpp"
#include "akruti/akruti.hpp"
#include <cmath>

using namespace akruti;

TEST_CASE (
"akruti: GJK boolean overlap"
,
"[akruti][gjk]"
)
 {
    Circle a{{0, 0}, 1}, b{{1.5f, 0}, 1}, c{{5, 0}, 1};
    REQUIRE(gjk_overlap(a, b));
    REQUIRE_FALSE(gjk_overlap(a, c));

    Box ba{{0, 0}, {1, 1}}, bb{{1.5f, 0}, {1, 1}}, bc{{3.1f, 0}, {1, 1}};
    REQUIRE(gjk_overlap(ba, bb));
    REQUIRE_FALSE(gjk_overlap(ba, bc));
}

TEST_CASE (
"akruti: EPA penetration depth and normal"
,
"[akruti][epa]"
)
 {
    Circle a{{0, 0}, 1}, b{{1.5f, 0}, 1};
    Contact ct = epa(a, b);
    REQUIRE(ct.hit);
    REQUIRE(ct.depth == Catch::Approx(0.5).margin(0.02)); // (r+r) - dist
    REQUIRE(std::fabs(ct.normal.x) == Catch::Approx(1.0).margin(0.02));

    Box ba{{0, 0}, {1, 1}}, bb{{1.5f, 0}, {1, 1}};
    Contact cb = epa(ba, bb);
    REQUIRE(cb.hit);
    REQUIRE(cb.depth == Catch::Approx(0.5).margin(0.02));
}

TEST_CASE (
"akruti: GJK distance between disjoint shapes"
,
"[akruti][gjk][distance]"
)
 {
    Circle a{{0, 0}, 1}, b{{5, 0}, 1};
    Separation s = gjk_distance(a, b);
    REQUIRE(s.distance == Catch::Approx(3.0).margin(0.02)); // surface gap
    REQUIRE(std::fabs(s.dir.x) == Catch::Approx(1.0).margin(0.02));
}
