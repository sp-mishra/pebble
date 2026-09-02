#include "catch_amalgamated.hpp"
#include "akruti/akruti.hpp"
#include "akruti/csg.hpp"
#include <cmath>

TEST_CASE (
"Akruti: CSG Expression Tree Auto-Diff Normals"
,
"[akruti][csg]"
)
 {
    using namespace akruti::expr;

    akruti::Circle c{{0.0f, 0.0f}, 2.0f};
    akruti::Box b{{1.0f, 0.0f}, {1.0f, 1.0f}};

    // Carved shape: Circle minus Box
    auto shape = c - b;

    // Normal at (-2, 0) should point outward along -X
    auto norm_left = normal_auto_diff(shape, {-2.0f, 0.0f});
    REQUIRE(norm_left.x == Catch::Approx(-1.0f).margin(1e-3f));
    REQUIRE(std::fabs(norm_left.y) < 1e-3f);

    // Normal at (0, 2) should point outward along +Y
    auto norm_top = normal_auto_diff(shape, {0.0f, 2.0f});
    REQUIRE(std::fabs(norm_top.x) < 1e-3f);
    REQUIRE(norm_top.y == Catch::Approx(1.0f).margin(1e-3f));
}

TEST_CASE (
"akruti: csg_smooth_subtract yields negative sdf inside carved region"
,
"[akruti][csg][smooth]"
)
 {
    using namespace akruti::expr;

    akruti::Circle base{{0.0f, 0.0f}, 3.0f};
    akruti::Circle cutter{{0.0f, 0.0f}, 1.5f};

    auto carved = csg_smooth_subtract(base, cutter, 0.3f);

    // Deep inside cutter region: base SDF is negative (inside circle), cutter SDF is negative
    // smooth_subtract blends: should produce a value that trends toward positive near cutter center
    const float sdf_at_center = carved.sdf({0.0f, 0.0f});
    // At center: base.sdf = -3, cutter.sdf = -1.5 → smooth_subtract should be positive (carved out)
    REQUIRE(sdf_at_center > 0.0f);

    // Far outside base circle: positive sdf, not inside anything
    const float sdf_outside = carved.sdf({5.0f, 0.0f});
    REQUIRE(sdf_outside > 0.0f);
}

TEST_CASE (
"akruti: csg_smooth_intersect boundary continuity"
,
"[akruti][csg][smooth]"
)
 {
    using namespace akruti::expr;

    akruti::Circle a{{-0.5f, 0.0f}, 2.0f};
    akruti::Circle b{{ 0.5f, 0.0f}, 2.0f};

    auto intersected = csg_smooth_intersect(a, b, 0.2f);

    // Point at origin is inside both circles → inside intersection
    REQUIRE(intersected.sdf({0.0f, 0.0f}) < 0.0f);

    // Point far to the left: inside A but outside B → outside intersection
    REQUIRE(intersected.sdf({-3.0f, 0.0f}) > 0.0f);

    // SDF should be C1 continuous: no abrupt jump near boundary
    const float s1 = intersected.sdf({1.4f, 0.0f});
    const float s2 = intersected.sdf({1.5f, 0.0f});
    REQUIRE(std::fabs(s2 - s1) < 0.5f); // smooth, not a step
}

TEST_CASE (
"akruti: Shape centroid contract satisfied by all primitives"
,
"[akruti][concept][centroid]"
)
 {
    static_assert(akruti::Shape<akruti::Circle>);
    static_assert(akruti::Shape<akruti::Box>);
    static_assert(akruti::Shape<akruti::Capsule>);
    static_assert(akruti::Shape<akruti::Triangle>);
    static_assert(akruti::Shape<akruti::ConvexPoly<4>>);

    akruti::Circle c{{1.0f, 2.0f}, 1.0f};
    REQUIRE(c.centroid().x == Catch::Approx(1.0f));
    REQUIRE(c.centroid().y == Catch::Approx(2.0f));

    akruti::Box b{{3.0f, 4.0f}, {1.0f, 1.0f}};
    REQUIRE(b.centroid().x == Catch::Approx(3.0f));
    REQUIRE(b.centroid().y == Catch::Approx(4.0f));

    akruti::Capsule cap{{0.0f, 0.0f}, {0.0f, 2.0f}, 0.5f};
    REQUIRE(cap.centroid().x == Catch::Approx(0.0f));
    REQUIRE(cap.centroid().y == Catch::Approx(1.0f));

    akruti::ConvexPoly<4> poly;
    (void)poly.verts.push_back({0.0f, 0.0f});
    (void)poly.verts.push_back({2.0f, 0.0f});
    (void)poly.verts.push_back({2.0f, 2.0f});
    (void)poly.verts.push_back({0.0f, 2.0f});
    auto cen = poly.centroid();
    REQUIRE(cen.x == Catch::Approx(1.0f).margin(1e-4f));
    REQUIRE(cen.y == Catch::Approx(1.0f).margin(1e-4f));
}
