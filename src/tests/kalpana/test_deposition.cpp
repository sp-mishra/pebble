#include "catch_amalgamated.hpp"
#include "kalpana/kalpana.hpp"

TEST_CASE("Kalpana: Brush Deposition Mechanics", "[kalpana][brush][deposit]") {
    using namespace kalpana;
    using namespace kalpana::deposit;

    SECTION("Default Deposition Opacity Saturation") {
        DepositionParams dp{.mode = Mode::Default, .flow = 1.0f};
        float cov = 0.0f;
        cov = dp.compute_opacity(cov, 0.5f, 1.0f, 0.5f);
        REQUIRE(cov > 0.0f);
        float cov2 = dp.compute_opacity(cov, 0.5f, 1.0f, 0.5f);
        REQUIRE(cov2 > cov);
    }

    SECTION("Watercolor Edge Darkening and Granulation") {
        DepositionParams dp{
            .mode = Mode::Watercolor,
            .flow = 0.8f,
            .buildup_rate = 0.5f,
            .grain_scale = 1.0f,
            .edge_darken = 1.0f
        };

        // Edge region dab alpha ~0.5 has edge darkening fringe boost
        float edge_op = dp.compute_opacity(0.0f, 0.5f, 1.0f, 0.5f);
        REQUIRE(edge_op > 0.0f);
    }

    SECTION("Dry Pastel Grain Interaction") {
        DepositionParams dp{.mode = Mode::Pastel, .flow = 1.0f, .grain_scale = 1.0f};
        // Low grain value (valley) catches no dry pastel
        float valley_op = dp.compute_opacity(0.0f, 1.0f, 0.5f, 0.1f);
        REQUIRE(valley_op == 0.0f);

        // High grain peak catches pigment
        float peak_op = dp.compute_opacity(0.0f, 1.0f, 1.0f, 0.9f);
        REQUIRE(peak_op > 0.0f);
    }
}
