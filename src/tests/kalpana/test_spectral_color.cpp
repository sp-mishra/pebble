#include "catch_amalgamated.hpp"
#include "kalpana/kalpana.hpp"

TEST_CASE (
"Kalpana: SpectralColor and Kubelka-Munk Mixing"
,
"[kalpana][color][spectral]"
)
 {
    using namespace kalpana;
    using namespace kalpana::spectral;

    SECTION("Conversion between Color and SpectralColor") {
        Color c(0.8f, 0.2f, 0.1f, 1.0f);
        SpectralColor sc = SpectralColor::from_color(c);
        Color roundtrip = sc.to_color();

        REQUIRE(std::fabs(c.r - roundtrip.r) < 0.05f);
        REQUIRE(std::fabs(c.g - roundtrip.g) < 0.05f);
        REQUIRE(std::fabs(c.b - roundtrip.b) < 0.05f);
        REQUIRE(roundtrip.a == 1.0f);
    }

    SECTION("SpectralColor mix_km produces subtractive green from blue and yellow") {
        SpectralColor blue = SpectralColor::from_color(colors::blue());
        SpectralColor yellow = SpectralColor::from_color(colors::yellow());

        SpectralColor green = blue.mix_km(yellow, 0.5f);
        Color green_c = green.to_color();

        REQUIRE(green_c.g > green_c.r);
    }

    SECTION("SpectralGradient multi-stop sampling") {
        SpectralGradient grad{
            .stops = {
                SpectralGradientStop{.offset = 0.0f, .pigment = pigments::ultramarine_blue()},
                SpectralGradientStop{.offset = 1.0f, .pigment = pigments::cadmium_yellow()}
            }
        };

        SpectralColor mid = grad.sample(0.5f);
        Color mid_c = mid.to_color();
        REQUIRE(mid_c.g > mid_c.r);

        SpectralColor start = grad.sample(0.0f);
        Color start_c = start.to_color();
        REQUIRE(start_c.b > start_c.r);
    }

    SECTION("Spectral Bloom") {
        Color bright(0.95f, 0.95f, 0.95f, 1.0f);
        Color bloomed = spectral_bloom(bright, 0.8f, 1.0f);
        REQUIRE(bloomed.r >= bright.r);
        REQUIRE(bloomed.g >= bright.g);
    }
}

TEST_CASE (
"Kalpana: Pigment Catalog & Extensible Registry"
,
"[kalpana][color][pigments]"
)
 {
    using namespace kalpana;
    using namespace kalpana::pigments;

    SECTION("Named Pigment Presets") {
        auto y = cadmium_yellow();
        auto b = ultramarine_blue();
        auto r = cadmium_red();
        auto w = titanium_white();
        auto k = ivory_black();

        REQUIRE(y.to_color().r > 0.3f);
        REQUIRE(b.to_color().b > 0.3f);
        REQUIRE(r.to_color().r > 0.3f);
        REQUIRE(w.to_color().r > 0.8f);
        REQUIRE(k.to_color().r < 0.2f);
    }

    SECTION("User-Registered Custom Pigments") {
        // Register custom curve: turquoise peak around 500nm
        register_custom_pigment_curve("custom_turquoise", [](float nm) {
            return std::exp(-0.5f * std::pow((nm - 500.0f) / 35.0f, 2.0f));
        });

        REQUIRE(PigmentRegistry::instance().contains("custom_turquoise"));
        spectral::SpectralColor turq = get_pigment("custom_turquoise");
        Color tc = turq.to_color();
        REQUIRE(tc.g > 0.2f);
    }
}
