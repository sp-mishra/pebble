#include "catch_amalgamated.hpp"
#include "akruti/cdt.hpp"

TEST_CASE (
"Akruti: Constrained Delaunay Triangulator"
,
"[akruti][cdt]"
)
 {
    akruti::CdtTriangulator cdt;
    std::vector<akruti::Vec> poly = {
        {0, 0}, {4, 0}, {4, 4}, {0, 4}
    };

    auto tris = cdt.triangulate(poly);
    REQUIRE(tris.size() == 2);

    // Test in-place triangulate_into buffer sink
    containers::dynamic::SmallVector<akruti::Triangle, 256> sink_tris;
    cdt.triangulate_into(poly, sink_tris);
    REQUIRE(sink_tris.size() == 2);

    // Test static_vector compile-time deduction
    containers::static_vector<akruti::Vec, 4> static_poly;
    static_poly.push_back({0, 0});
    static_poly.push_back({4, 0});
    static_poly.push_back({4, 4});
    static_poly.push_back({0, 4});
    auto static_tris = cdt.triangulate(static_poly);
    REQUIRE(static_tris.size() == 2);
}
