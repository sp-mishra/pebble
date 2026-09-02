#include "catch_amalgamated.hpp"
#include "akruti/akruti.hpp"
#include "akruti/spline.hpp"
#include <cmath>

TEST_CASE (
"Akruti: Cubic Bézier Curve Evaluation and Arc-Length"
,
"[akruti][spline]"
)
 {
    akruti::CubicBezierCurve bezier{
        .p0 = {0.0f, 0.0f},
        .p1 = {0.0f, 10.0f},
        .p2 = {10.0f, 10.0f},
        .p3 = {10.0f, 0.0f},
        .radius = 1.0f
    };

    auto start = bezier.evaluate(0.0f);
    REQUIRE(start.x == 0.0f);
    REQUIRE(start.y == 0.0f);

    auto end = bezier.evaluate(1.0f);
    REQUIRE(end.x == 10.0f);
    REQUIRE(end.y == 0.0f);

    auto mid = bezier.evaluate(0.5f);
    REQUIRE(mid.x == 5.0f);
    REQUIRE(mid.y == 7.5f);

    float len = bezier.arc_length();
    REQUIRE(len > 15.0f);
    REQUIRE(len < 25.0f);
}

TEST_CASE (
"Akruti: Spline Shape Contract (SDF, AABB, Support)"
,
"[akruti][spline]"
)
 {
    akruti::CubicBezierCurve bezier{
        .p0 = {0.0f, 0.0f},
        .p1 = {0.0f, 10.0f},
        .p2 = {10.0f, 10.0f},
        .p3 = {10.0f, 0.0f},
        .radius = 1.0f
    };

    // Point on curve start should have distance <= 0 (within stroke radius)
    float d_on = bezier.sdf({0.0f, 0.0f});
    REQUIRE(d_on <= 0.0f);

    // Far away point
    float d_far = bezier.sdf({100.0f, 100.0f});
    REQUIRE(d_far > 50.0f);

    auto box = bezier.aabb();
    REQUIRE(box.lo[0] <= -1.0f);
    REQUIRE(box.hi[0] >= 11.0f);
}

TEST_CASE (
"Akruti: Spline CSG Shape Arithmetic Participation"
,
"[akruti][spline][csg]"
)
 {
    akruti::CubicBezierCurve bezier{
        .p0 = {0.0f, 0.0f},
        .p1 = {5.0f, 5.0f},
        .p2 = {5.0f, 5.0f},
        .p3 = {10.0f, 0.0f},
        .radius = 1.0f
    };

    akruti::Circle circle{{0.0f, 0.0f}, 5.0f};

    // Construct CSG dynamic node blending circle and spline
    akruti::CsgNode node;
    node.is_leaf = false;
    node.op = akruti::CsgOp::Union;
    node.a = std::make_unique<akruti::CsgNode>(akruti::CsgNode{.is_leaf = true, .leaf = circle});
    node.b = std::make_unique<akruti::CsgNode>(akruti::CsgNode{.is_leaf = true, .leaf = bezier});

    float d = node.sdf({0.0f, 0.0f});
    REQUIRE(d < 0.0f);
}
