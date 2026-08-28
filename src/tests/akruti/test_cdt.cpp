#include "catch_amalgamated.hpp"
#include "akruti/cdt.hpp"

TEST_CASE("Akruti: Constrained Delaunay Triangulator", "[akruti][cdt]") {
    akruti::CdtTriangulator cdt;
    std::vector<akruti::Vec> poly = {
        {0, 0}, {4, 0}, {4, 4}, {0, 4}
    };

    auto tris = cdt.triangulate(poly);
    REQUIRE(tris.size() == 2);
}
