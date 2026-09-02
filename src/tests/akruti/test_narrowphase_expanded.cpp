#include "catch_amalgamated.hpp"
#include "akruti/narrowphase.hpp"

TEST_CASE (
"Akruti: Narrowphase Expanded Pairs"
,
"[akruti][narrowphase]"
)
 {
    // 1. Circle - OBB
    akruti::Circle c{{0, 0}, 1.0f};
    akruti::OrientedBox obb = akruti::OrientedBox::from_angle({1.5f, 0}, {1, 1}, 0.0f);
    auto m1 = akruti::collide_circle_obb(c, obb);
    REQUIRE(m1.hit);
    REQUIRE(m1.depth > 0.0f);

    // 2. Circle - Triangle
    akruti::Triangle tri{{0.8f, -1}, {2, 0}, {0.8f, 1}};
    auto m2 = akruti::collide_circle_triangle(c, tri);
    REQUIRE(m2.hit);

    // 3. Segment - Circle
    akruti::Segment seg{{-2, 0}, {2, 0}};
    akruti::Circle c2{{0, 0.5f}, 1.0f};
    auto m3 = akruti::collide_segment_circle(seg, c2);
    REQUIRE(m3.hit);

    // 4. Triangle - Triangle
    akruti::Triangle t1{{0, 0}, {2, 0}, {1, 2}};
    akruti::Triangle t2{{0.5f, 0.5f}, {2.5f, 0.5f}, {1.5f, 2.5f}};
    auto m4 = akruti::collide_triangle_triangle(t1, t2);
    REQUIRE(m4.hit);
}
