#include "catch_amalgamated.hpp"
#include "akruti/akruti.hpp"
#include "akruti/poly_ops.hpp"
#include <cmath>

// Appended: coverage for the new akruti polygon-domain boolean & offset ops
// (offset_polygon / union_polygon / subtract_polygon). These are the single-owner
// implementations that Kalpana's path offset + boolean builders delegate to.

using akruti::Poly;
using akruti::Vec2;
using akruti::Scalar;

namespace {
    Poly make_square(Scalar s, Scalar cx = 0, Scalar cy = 0) {
        Poly p;
        p.push_back(Vec2<Scalar>{cx - s, cy - s});
        p.push_back(Vec2<Scalar>{cx + s, cy - s});
        p.push_back(Vec2<Scalar>{cx + s, cy + s});
        p.push_back(Vec2<Scalar>{cx - s, cy + s});
        return p;
    }
} // namespace

TEST_CASE (
"Akruti poly_ops: offset inflates area for positive delta (miter)"
,
"[akruti][poly_ops]"
)
 {
    const Poly sq = make_square(10.0f);               // 20x20 => area 400
    const Scalar base_area = std::fabs(akruti::polygon_area(sq));

    const Poly grown = akruti::offset_polygon(sq, 5.0f, akruti::JoinStyle::Miter);
    REQUIRE(grown.size() >= 3);
    const Scalar grown_area = std::fabs(akruti::polygon_area(grown));
    REQUIRE(grown_area > base_area);                  // inflation grows area

    const Poly shrunk = akruti::offset_polygon(sq, -3.0f, akruti::JoinStyle::Miter);
    REQUIRE(shrunk.size() >= 3);
    REQUIRE(std::fabs(akruti::polygon_area(shrunk)) < base_area); // deflation shrinks
}

TEST_CASE (
"Akruti poly_ops: offset join styles all produce valid contours"
,
"[akruti][poly_ops]"
)
 {
    const Poly sq = make_square(10.0f);
    for (auto js : {akruti::JoinStyle::Miter, akruti::JoinStyle::Round, akruti::JoinStyle::Bevel}) {
        const Poly r = akruti::offset_polygon(sq, 4.0f, js);
        REQUIRE(r.size() >= 3);
        REQUIRE(std::fabs(akruti::polygon_area(r)) > 0.0f);
    }
}

TEST_CASE (
"Akruti poly_ops: union of nested returns outer; area >= max(a,b)"
,
"[akruti][poly_ops]"
)
 {
    const Poly big = make_square(10.0f);
    const Poly small = make_square(3.0f);
    const Poly u = akruti::union_polygon(big, small);
    // small ⊂ big => union is the outer (big) contour.
    const Scalar ua = std::fabs(akruti::polygon_area(u));
    REQUIRE(ua == Catch::Approx(std::fabs(akruti::polygon_area(big))).margin(1e-3));
    REQUIRE(ua >= std::fabs(akruti::polygon_area(small)));
}

TEST_CASE (
"Akruti poly_ops: subtract fully-contained clip empties subject"
,
"[akruti][poly_ops]"
)
 {
    const Poly subject = make_square(3.0f);
    const Poly clip = make_square(10.0f);            // clip ⊃ subject
    const Poly d = akruti::subtract_polygon(subject, clip);
    REQUIRE(d.empty());                              // nothing of subject survives
}

TEST_CASE (
"Akruti poly_ops: subtract disjoint clip leaves subject intact"
,
"[akruti][poly_ops]"
)
 {
    const Poly subject = make_square(3.0f, 0.0f, 0.0f);
    const Poly clip = make_square(3.0f, 100.0f, 100.0f); // far away
    const Poly d = akruti::subtract_polygon(subject, clip);
    REQUIRE(d.size() == subject.size());
    REQUIRE(std::fabs(akruti::polygon_area(d)) ==
            Catch::Approx(std::fabs(akruti::polygon_area(subject))).margin(1e-3));
}

TEST_CASE (
"Akruti poly_ops: intersect parity via clip_polygon on overlap"
,
"[akruti][poly_ops]"
)
 {
    // clip_polygon (intersect) is the pre-existing op Kalpana::intersect uses; sanity-check
    // it against overlapping squares — intersection area must be positive and <= each input.
    const Poly a = make_square(10.0f, 0.0f, 0.0f);
    const Poly b = make_square(10.0f, 8.0f, 0.0f);   // overlap x in [-2,10]
    const Poly x = akruti::clip_polygon(a, b);
    REQUIRE(x.size() >= 3);
    const Scalar xa = std::fabs(akruti::polygon_area(x));
    REQUIRE(xa > 0.0f);
    REQUIRE(xa <= std::fabs(akruti::polygon_area(a)) + 1e-3f);
}

TEST_CASE("Akruti poly_ops: voronoi shatter and khanda fracture integration", "[akruti][poly_ops][khanda]") {
    const Poly poly = make_square(10.0f);
    std::vector<Vec2<Scalar>> sites = {{ -5.0f, -5.0f }, { 5.0f, 5.0f }};
    const auto shards = akruti::voronoi_shatter(poly, sites);
    REQUIRE(!shards.empty());
    Scalar sum_area = 0;
    for (const auto& s : shards) {
        sum_area += std::fabs(akruti::polygon_area(s));
    }
    REQUIRE(sum_area == Catch::Approx(400.0f).margin(1e-2f));
}

TEST_CASE("Akruti auto_policies: intelligent container deduction and sink dispatch", "[akruti][auto_policies]") {
    akruti::AutoTriangulator auto_tri;
    akruti::Poly poly = make_square(10.0f);

    // 1. SmallVector output (default)
    auto small_tris = auto_tri(poly);
    REQUIRE(small_tris.size() == 2);

    // 2. static_vector output (100% stack)
    containers::static_vector<Vec2<Scalar>, 4> static_poly;
    for (const auto& pt : poly) (void)static_poly.push_back(pt);
    auto static_tris = auto_tri(static_poly);
    REQUIRE(static_tris.size() == 2);

    // 3. std::vector output
    std::vector<akruti::Triangle> std_tris = auto_tri.triangulate_vector(std::span<const Vec2<Scalar>>(poly.data(), poly.size()));
    REQUIRE(std_tris.size() == 2);

    // 4. In-place buffer sink
    std::vector<akruti::Triangle> sink_tris;
    auto_tri.triangulate_into(poly, sink_tris);
    REQUIRE(sink_tris.size() == 2);
}
