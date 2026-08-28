#include "catch_amalgamated.hpp"
#include "kalpana/kalpana.hpp"

TEST_CASE("Kalpana: Brush Pipeline & Rebelle Presets", "[kalpana][brush]") {
    using namespace kalpana;

    SECTION("SpectralBrush Configuration and Emission") {
        SpectralBrush brush;
        brush.size(16.0f)
             .spacing(0.25f)
             .pigment(pigments::cadmium_red());

        brush.size_dyn() = DynamicsBinding{
            .source = DynamicsSource::Pressure,
            .lo = 0.5f,
            .hi = 1.0f,
            .curve = 1.0f
        };

        BrushPoint p0{.pos = {0.0f, 0.0f}, .pressure = 0.5f};
        BrushPoint p1{.pos = {100.0f, 0.0f}, .pressure = 1.0f};

        auto stamps = brush.stroke_segment(p0, p1);
        REQUIRE_FALSE(stamps.empty());
        REQUIRE(stamps.front().radius < stamps.back().radius);
        REQUIRE(stamps.front().pigment == pigments::cadmium_red());
    }

    SECTION("Rebelle Physical Brush Presets") {
        SpectralBrush watercolor;
        watercolor.apply_preset(BrushPreset::watercolor_wash());
        REQUIRE(watercolor.size() == 32.0f);

        SpectralBrush oil;
        oil.apply_preset(BrushPreset::oil_impasto());
        REQUIRE(oil.spacing() == 0.10f);

        SpectralBrush pen;
        pen.apply_preset(BrushPreset::ink_pen());
        REQUIRE(pen.size() == 8.0f);
    }

    SECTION("Smudge Sampling") {
        SpectralBrush brush;
        brush.pigment(pigments::titanium_white());
        brush.impasto_params(PigmentImpastoParams{.smudge_rate = 0.5f});

        spectral::SpectralColor surface = pigments::ultramarine_blue();
        spectral::SpectralColor smudged = brush.smudge_sample(surface, pigments::titanium_white(), 1.0f);

        Color sc = smudged.to_color();
        REQUIRE(sc.b > 0.1f); // Picked up blue from surface
    }
}
