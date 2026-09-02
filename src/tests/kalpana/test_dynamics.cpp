#include "catch_amalgamated.hpp"
#include "kalpana/kalpana.hpp"

TEST_CASE (
"Kalpana: Dynamics Binding Evaluation"
,
"[kalpana][brush][dynamics]"
)
 {
    using namespace kalpana;

    BrushInputState input{
        .pressure = 0.5f,
        .velocity = 0.8f,
        .tilt = 0.2f,
        .direction = 0.4f,
        .distance = 100.0f,
        .time = 2.0f,
        .random = 0.7f
    };

    SECTION("Constant Dynamics") {
        DynamicsBinding b{.source = DynamicsSource::Constant, .base = 0.75f};
        REQUIRE(b.evaluate(input) == 0.75f);
    }

    SECTION("Pressure Dynamics with Scaling Range") {
        DynamicsBinding b{.source = DynamicsSource::Pressure, .lo = 10.0f, .hi = 20.0f, .curve = 1.0f};
        REQUIRE(b.evaluate(input) == 15.0f);
    }

    SECTION("Power Curves (Ease-In / Ease-Out)") {
        DynamicsBinding ease_in{.source = DynamicsSource::Pressure, .lo = 0.0f, .hi = 1.0f, .curve = 2.0f};
        DynamicsBinding ease_out{.source = DynamicsSource::Pressure, .lo = 0.0f, .hi = 1.0f, .curve = 0.5f};

        // At pressure 0.5: linear is 0.5, ease_in is 0.25, ease_out is ~0.707
        REQUIRE(ease_in.evaluate(input) == 0.25f);
        REQUIRE(ease_out.evaluate(input) > 0.5f);
    }

    SECTION("Velocity and Tilt Dynamics") {
        DynamicsBinding vb{.source = DynamicsSource::Velocity, .lo = 0.0f, .hi = 10.0f};
        REQUIRE(std::fabs(vb.evaluate(input) - 8.0f) < 1e-4f);

        DynamicsBinding tb{.source = DynamicsSource::Tilt, .lo = 0.0f, .hi = 10.0f};
        REQUIRE(std::fabs(tb.evaluate(input) - 2.0f) < 1e-4f);
    }
}
