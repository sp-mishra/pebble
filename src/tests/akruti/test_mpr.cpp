#include "catch_amalgamated.hpp"
#include "akruti/mpr.hpp"
#include <cmath>

TEST_CASE("Akruti: MPR Distance Oracle", "[akruti][mpr]") {
    akruti::Circle c1{{0, 0}, 1.0f};
    akruti::Circle c2{{3, 0}, 1.0f};

    akruti::MprDistanceOracle oracle;
    auto res = oracle(c1, c2);

    REQUIRE(!res.overlap);
    REQUIRE(std::fabs(res.distance - 1.0f) < 0.1f);
}
