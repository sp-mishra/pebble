#include "catch_amalgamated.hpp"
#include "kalpana/brush/stabilizer.hpp"
#include "kalpana/brush/brush.hpp"

TEST_CASE (
"OneEuroFilter: smooths noisy input"
,
"[kalpana][stabilizer]"
)
 {
    using namespace kalpana;
    OneEuroFilter f;
    f.reset();

    // Feed a constant position → should converge to that position
    BrushPoint in{.pos = {100.0f, 200.0f}, .pressure = 1.0f};
    BrushPoint out{};
    for (int i = 0; i < 30; ++i) out = f(in);

    REQUIRE(out.pos[0] == Catch::Approx(100.0f).margin(2.0f));
    REQUIRE(out.pos[1] == Catch::Approx(200.0f).margin(2.0f));
}

TEST_CASE (
"OneEuroFilter: reduces jitter (output less noisy than input)"
,
"[kalpana][stabilizer]"
)
 {
    using namespace kalpana;
    OneEuroFilter f;
    f.reset();

    float max_input_jump = 0.0f, max_output_jump = 0.0f;
    BrushPoint prev_out{};
    bool first = true;

    // Alternate between two positions (high-frequency noise)
    for (int i = 0; i < 20; ++i) {
        BrushPoint in{ .pos = {(i % 2 == 0) ? 100.0f : 108.0f, 200.0f}, .pressure = 1.0f };
        if (i > 0) max_input_jump = std::max(max_input_jump, std::abs(in.pos[0] - ((i-1)%2==0 ? 100.0f : 108.0f)));
        BrushPoint out = f(in);
        if (!first) max_output_jump = std::max(max_output_jump, std::abs(out.pos[0] - prev_out.pos[0]));
        prev_out = out;
        first = false;
    }
    REQUIRE(max_output_jump < max_input_jump);
}

TEST_CASE (
"StrokeStabilizer<OneEuro>: apply returns filtered point"
,
"[kalpana][stabilizer]"
)
 {
    using namespace kalpana;
    OneEuroStabilizer s;
    s.strength(0.5f);
    s.reset();

    BrushPoint p{.pos = {50.0f, 50.0f}, .pressure = 0.8f};
    BrushPoint out = s.apply(p);
    // Output must be a valid position (not NaN)
    REQUIRE(!std::isnan(out.pos[0]));
    REQUIRE(!std::isnan(out.pos[1]));
}

TEST_CASE (
"PullLagFilter: converges toward target"
,
"[kalpana][stabilizer]"
)
 {
    using namespace kalpana;
    PullLagFilter f;
    f.reset();

    BrushPoint target{.pos = {300.0f, 400.0f}};
    BrushPoint out{};
    for (int i = 0; i < 40; ++i) out = f(target);

    REQUIRE(std::abs(out.pos[0] - 300.0f) < 5.0f);
    REQUIRE(std::abs(out.pos[1] - 400.0f) < 5.0f);
}

TEST_CASE (
"CatmullRomFilter: output is smooth midpoint interpolation"
,
"[kalpana][stabilizer]"
)
 {
    using namespace kalpana;
    CatmullRomFilter<4> f;
    f.reset();

    // Feed linearly increasing positions
    BrushPoint out{};
    for (int i = 0; i < 10; ++i) {
        BrushPoint p{.pos = {static_cast<float>(i * 10), 0.0f}};
        out = f(p);
    }
    // Midpoint output should be between 0 and 90
    REQUIRE(out.pos[0] >= 0.0f);
    REQUIRE(out.pos[0] <= 90.0f);
}
