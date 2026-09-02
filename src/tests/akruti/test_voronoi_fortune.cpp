#include "catch_amalgamated.hpp"
#include "akruti/voronoi.hpp"

TEST_CASE (
"Akruti: Fortune Voronoi Builder"
,
"[akruti][voronoi]"
)
 {
    akruti::Poly boundary = akruti::rect_poly({-10, -10}, {10, 10});
    std::vector<akruti::Vec> seeds = {
        {-2, -2}, {2, -2}, {0, 2}
    };

    akruti::FortuneVoronoiBuilder builder;
    auto cells = builder(boundary, seeds);

    REQUIRE(cells.size() == 3);
    for (const auto& c : cells) {
        REQUIRE(!c.empty());
    }
}
