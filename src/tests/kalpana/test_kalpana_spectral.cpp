#include "catch_amalgamated.hpp"
#include "kalpana/kalpana.hpp"
#include <vector>

TEST_CASE (
"Kalpana: Multi-Stop Kubelka-Munk Spectral Gradient"
,
"[kalpana][color][spectral]"
)
 {
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

TEST_CASE (
"Kalpana: SpectralGradient SmallVector multi-stop"
,
"[kalpana][color][spectral][smallvector]"
)
 {
    kalpana::spectral::SpectralGradient grad;
    (void)grad.stops.push_back({0.0f, kalpana::spectral::SpectralColor::from_color(kalpana::colors::blue())});
    (void)grad.stops.push_back({0.5f, kalpana::spectral::SpectralColor::from_color(kalpana::colors::yellow())});
    (void)grad.stops.push_back({1.0f, kalpana::spectral::SpectralColor::from_color(kalpana::colors::red())});

    REQUIRE(grad.stops.size() == 3);

    // Sample at t=0 -> blue
    auto at_start = grad.sample(0.0f);
    kalpana::Color col_start = at_start.to_color();
    REQUIRE(col_start.b > 0.5f);

    // Sample at t=1 -> red
    auto at_end = grad.sample(1.0f);
    kalpana::Color col_end = at_end.to_color();
    REQUIRE(col_end.r > 0.5f);

    // Sample at t=0.5 -> yellow-ish (r+g high, b low)
    auto at_mid = grad.sample(0.5f);
    kalpana::Color col_mid = at_mid.to_color();
    REQUIRE(col_mid.b < col_mid.r + col_mid.g); // not primarily blue
}
