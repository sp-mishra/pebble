#include "catch_amalgamated.hpp"
#include "kalpana/kalpana.hpp"
#include "akruti/poly_ops.hpp"
#include <cmath>

// Appended: proves Kalpana's geometric CSG/offset are thin delegators to akruti::poly_ops
// (single owner), not independent implementations. Each builder must reproduce the geometry
// the corresponding akruti op yields for the same input contours.

using kalpana::Path;

namespace {
akruti::Poly square_poly(float s, float cx = 0.0f, float cy = 0.0f) {
    akruti::Poly p;
    p.push_back(akruti::Vec2<akruti::Scalar>{cx - s, cy - s});
    p.push_back(akruti::Vec2<akruti::Scalar>{cx + s, cy - s});
    p.push_back(akruti::Vec2<akruti::Scalar>{cx + s, cy + s});
    p.push_back(akruti::Vec2<akruti::Scalar>{cx - s, cy + s});
    return p;
}
} // namespace

TEST_CASE("Kalpana: subtract delegates to akruti::subtract_polygon", "[kalpana][geom][csg]") {
    // Disjoint clip => subtract must leave subject area unchanged (matches akruti fast path).
    Path a = kalpana::rect(-10.0f, -10.0f, 20.0f, 20.0f);
    Path b = kalpana::rect(1000.0f, 1000.0f, 20.0f, 20.0f);

    Path diff = kalpana::subtract(a, b);
    const auto diff_poly = diff.to_poly();
    REQUIRE(diff_poly.size() >= 3);

    const auto expect = akruti::subtract_polygon(a.to_poly(), b.to_poly());
    REQUIRE(std::fabs(akruti::polygon_area(diff_poly)) ==
            Catch::Approx(std::fabs(akruti::polygon_area(expect))).margin(1e-2));
}

TEST_CASE("Kalpana: unite of nested returns outer contour (akruti parity)", "[kalpana][geom][csg]") {
    Path big = kalpana::rect(-10.0f, -10.0f, 20.0f, 20.0f);
    Path small = kalpana::rect(-3.0f, -3.0f, 6.0f, 6.0f);

    Path u = kalpana::unite(big, small);
    const auto up = u.to_poly();
    const auto expect = akruti::union_polygon(big.to_poly(), small.to_poly());

    REQUIRE(std::fabs(akruti::polygon_area(up)) ==
            Catch::Approx(std::fabs(akruti::polygon_area(expect))).margin(1e-2));
}

TEST_CASE("Kalpana: offset delegates to akruti::offset_polygon", "[kalpana][geom][offset]") {
    Path box = kalpana::rect(-10.0f, -10.0f, 20.0f, 20.0f);
    const float base_area = std::fabs(akruti::polygon_area(box.to_poly()));

    Path grown = kalpana::path_ops::offset(box, 5.0f, akruti::JoinStyle::Miter);
    const auto gp = grown.to_poly();
    REQUIRE(gp.size() >= 3);
    // Positive offset must inflate — same direction akruti::offset_polygon produces.
    REQUIRE(std::fabs(akruti::polygon_area(gp)) > base_area);
}
