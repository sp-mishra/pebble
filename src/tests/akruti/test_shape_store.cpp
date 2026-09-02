#include "catch_amalgamated.hpp"
#include "akruti/shape_store.hpp"
#include <cmath>

TEST_CASE (
"Akruti: ShapeStore"
,
"[akruti][shape_store]"
)
 {
    akruti::Circle c{.center = {1, 2}, .radius = 3.0f};
    akruti::ShapeStore store(c);

    REQUIRE(store.type == akruti::ShapeType::Circle);
    REQUIRE(std::fabs(store.sdf(akruti::Vec{1, 2}) - (-3.0f)) < 1e-4);

    akruti::Box b{.center = {0, 0}, .half = {1, 1}};
    store.set(b);
    REQUIRE(store.type == akruti::ShapeType::Box);
    REQUIRE(std::fabs(store.sdf(akruti::Vec{0, 0}) - (-1.0f)) < 1e-4);
}
