#include "catch_amalgamated.hpp"
#include "akruti/akruti.hpp"
#include <cmath>

TEST_CASE (
"Akruti: GridSDF 2D Bilinear Interpolation & Shape Contract"
,
"[akruti][gridsdf]"
)
 {
    // 3x3 grid spanning [-1, -1] to [1, 1]
    akruti::GridSDF<3, 3> grid;
    grid.bounds = {{-1.0f, -1.0f}, {1.0f, 1.0f}};

    // Distance field representing a circle of radius 0.5 at origin: d = len(p) - 0.5
    for (std::size_t y = 0; y < 3; ++y) {
        float py = -1.0f + static_cast<float>(y) * 1.0f; // -1, 0, 1
        for (std::size_t x = 0; x < 3; ++x) {
            float px = -1.0f + static_cast<float>(x) * 1.0f; // -1, 0, 1
            float d = std::sqrt(px * px + py * py) - 0.5f;
            grid.grid[y * 3 + x] = d;
        }
    }

    // Origin (0, 0) should interpolate exactly to center cell (d = -0.5)
    float d_origin = grid.sdf({0.0f, 0.0f});
    REQUIRE(d_origin == Catch::Approx(-0.5f).margin(1e-3f));

    // Midpoint between center (0,0) and (1,0) -> (0.5, 0)
    // Grid (0,0) = -0.5, Grid(1,0) = 0.5 -> linear interp = 0.0
    float d_mid = grid.sdf({0.5f, 0.0f});
    REQUIRE(d_mid == Catch::Approx(0.0f).margin(1e-3f));

    // AABB check
    auto box = grid.aabb();
    REQUIRE(box.lo[0] == -1.0f);
    REQUIRE(box.hi[0] == 1.0f);

    // Support check
    auto sup = grid.support({1.0f, 0.0f});
    REQUIRE(sup.x() >= 0.99f);
}
