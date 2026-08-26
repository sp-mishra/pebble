#include "catch_amalgamated.hpp"
#include "akruti/akruti.hpp"
#include "akruti/csg.hpp"
#include <cmath>

TEST_CASE("Akruti: CSG Expression Tree Auto-Diff Normals", "[akruti][csg]") {
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
