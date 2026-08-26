#include "catch_amalgamated.hpp"
#include "kalpana/kalpana.hpp"
#include <vector>

TEST_CASE("Kalpana: Multi-Stop Kubelka-Munk Spectral Gradient", "[kalpana][color][spectral]") {
    std::vector<std::pair<float, kalpana::Color>> stops = {
        {0.0f, kalpana::colors::blue()},
        {0.5f, kalpana::colors::yellow()},
        {1.0f, kalpana::colors::red()}
    };

    // Sample at 0.25 (between Blue and Yellow) -> should be rich green in Kubelka-Munk
    kalpana::Color c_green = kalpana::spectral::sample_gradient(stops, 0.25f);
    REQUIRE(c_green.g > c_green.r);

    // Sample at 0.0 -> pure blue
    kalpana::Color c_start = kalpana::spectral::sample_gradient(stops, 0.0f);
    REQUIRE(c_start.b > 0.8f);

    // Sample at 1.0 -> pure red
    kalpana::Color c_end = kalpana::spectral::sample_gradient(stops, 1.0f);
    REQUIRE(c_end.r > 0.8f);
}
