#include "catch_amalgamated.hpp"
#include "kalpana/kalpana.hpp"

TEST_CASE (
"Kalpana: Color Space Conversions and OkLab Lerp"
,
"[kalpana][color][space]"
)
 {
    using namespace kalpana;
    using namespace kalpana::color_space;

    SECTION("HSL Round-Trip Accuracy") {
        Color c(0.8f, 0.4f, 0.2f, 1.0f);
        HSL hsl = to_hsl(c);
        Color roundtrip = from_hsl(hsl);

        REQUIRE(std::fabs(c.r - roundtrip.r) < 0.01f);
        REQUIRE(std::fabs(c.g - roundtrip.g) < 0.01f);
        REQUIRE(std::fabs(c.b - roundtrip.b) < 0.01f);
    }

    SECTION("OkLab Round-Trip Accuracy") {
        Color c(0.2f, 0.7f, 0.9f, 1.0f);
        OkLab lab = to_oklab(c);
        Color roundtrip = from_oklab(lab);

        REQUIRE(std::fabs(c.r - roundtrip.r) < 0.01f);
        REQUIRE(std::fabs(c.g - roundtrip.g) < 0.01f);
        REQUIRE(std::fabs(c.b - roundtrip.b) < 0.01f);
    }

    SECTION("OkLab Perceptual Interpolation") {
        Color blue = colors::blue();
        Color yellow = colors::yellow();

        Color mid_oklab = lerp_oklab(blue, yellow, 0.5f);
        REQUIRE(mid_oklab.r > 0.0f);
        REQUIRE(mid_oklab.g > 0.0f);
        REQUIRE(mid_oklab.b > 0.0f);
    }

    SECTION("Custom Color Space Extension via color_space_type Concept") {
        // User custom HSV struct
        struct CustomHSV {
            float h, s, v;
        };

        // User conversion specialization
        struct HSVConverter {
            static Color to_color(const CustomHSV& hsv) noexcept {
                return from_hsl(HSL{.h = hsv.h, .s = hsv.s, .l = hsv.v * 0.5f, .a = 1.0f});
            }
            static CustomHSV from_color(const Color& c) noexcept {
                HSL h = to_hsl(c);
                return CustomHSV{.h = h.h, .s = h.s, .v = h.l * 2.0f};
            }
        };

        CustomHSV hsv{.h = 120.0f, .s = 1.0f, .v = 1.0f};
        Color from_hsv = HSVConverter::to_color(hsv);
        REQUIRE(from_hsv.g > 0.0f);
    }
}
