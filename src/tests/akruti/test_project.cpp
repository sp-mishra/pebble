#include "catch_amalgamated.hpp"
#include "akruti/akruti.hpp"
#include <cmath>

// ===========================================================================
// akruti::project — generic 2D feasible projection onto a convex shape.
// Feasible point → unchanged; infeasible point → on boundary & feasible.
// ===========================================================================

TEST_CASE (
"Akruti: project leaves an interior point unchanged"
,
"[akruti][project]"
)
 {
    akruti::Circle c{ .center = {0.0f, 0.0f}, .radius = 2.0f };
    akruti::Vec2<akruti::Scalar> p{0.5f, -0.3f};       // inside (‖p‖≈0.58 < 2)
    auto q = akruti::project(c, p);
    CHECK(q.x == Catch::Approx(p.x));
    CHECK(q.y == Catch::Approx(p.y));
    CHECK(c.sdf(q) <= Catch::Approx(0.0f).margin(1e-4f));   // feasible
}

TEST_CASE (
"Akruti: project maps an outside point onto the boundary"
,
"[akruti][project]"
)
 {
    akruti::Circle c{ .center = {0.0f, 0.0f}, .radius = 2.0f };
    akruti::Vec2<akruti::Scalar> p{5.0f, 0.0f};        // outside (‖p‖=5 > 2)
    auto q = akruti::project(c, p);
    // on the boundary: sdf ≈ 0, and radius away from center along +x
    CHECK(c.sdf(q) == Catch::Approx(0.0f).margin(1e-2f));
    CHECK(std::sqrt(q.x*q.x + q.y*q.y) == Catch::Approx(2.0f).margin(1e-2f));
    CHECK(q.x > 0.0f);                                  // same side as p
}

TEST_CASE (
"Akruti: project onto a box clamps outside points"
,
"[akruti][project]"
)
 {
    akruti::Box b{ .center = {0.0f, 0.0f}, .half = {1.0f, 1.0f} };
    akruti::Vec2<akruti::Scalar> inside{0.2f, 0.9f};
    auto qi = akruti::project(b, inside);
    CHECK(qi.x == Catch::Approx(inside.x));
    CHECK(qi.y == Catch::Approx(inside.y));

    akruti::Vec2<akruti::Scalar> outside{3.0f, 0.0f};
    auto qo = akruti::project(b, outside);
    CHECK(b.sdf(qo) == Catch::Approx(0.0f).margin(1e-2f));   // on boundary
    CHECK(qo.x == Catch::Approx(1.0f).margin(1e-2f));
}
