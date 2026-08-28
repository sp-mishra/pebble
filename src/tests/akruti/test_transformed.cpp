#include "catch_amalgamated.hpp"
#include "akruti/transformed.hpp"
#include "akruti/primitives.hpp"
#include <cmath>

TEST_CASE("Akruti: TransformedShape", "[akruti][transformed]") {
    akruti::Box box{.center = {0, 0}, .half = {2, 1}};
    akruti::TransformedShape ts{box, akruti::Vec{10, 5}, 0.0f};

    REQUIRE(std::fabs(ts.sdf(akruti::Vec{10, 5}) - (-1.0f)) < 1e-4);
    REQUIRE(std::fabs(ts.sdf(akruti::Vec{13, 5}) - 1.0f) < 1e-4);

    auto aabb = ts.aabb();
    REQUIRE(std::fabs(aabb.lo[0] - 8.0f) < 1e-4);
    REQUIRE(std::fabs(aabb.hi[0] - 12.0f) < 1e-4);
}
