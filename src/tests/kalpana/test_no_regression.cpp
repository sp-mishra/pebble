// test_no_regression.cpp — guard: original kalpana features untouched by Kalpana Next
// These tests mirror smoke checks from the existing test suite.
// Added at end per project rules (do NOT modify existing tests).
#include "catch_amalgamated.hpp"
#include "kalpana/kalpana.hpp"
#include "kalpana/color/paint_field.hpp"
#include "kalpana/brush/brush_creator.hpp"

TEST_CASE (
"Regression: existing BrushPresets produce valid deposition"
,
"[kalpana][regression]"
)
 {
    using namespace kalpana;

    auto presets = {
        BrushPreset::watercolor_wash(),
        BrushPreset::oil_impasto(),
        BrushPreset::ink_pen(),
        BrushPreset::dry_pastel(),
        BrushPreset::soft_airbrush()
    };

    for (const auto& p : presets) {
        REQUIRE(!p.name.empty());
        REQUIRE(p.default_size > 0.0f);
        REQUIRE(p.spacing > 0.0f);
        REQUIRE(p.deposition.flow >= 0.0f);
        REQUIRE(p.deposition.flow <= 1.0f);
        REQUIRE(p.material.roughness >= 0.0f);
        REQUIRE(p.material.roughness <= 1.0f);
    }
}

TEST_CASE (
"Regression: BrushPipeline stroke_segment backward-compatible"
,
"[kalpana][regression]"
)
 {
    using namespace kalpana;
    SpectralBrush brush;
    brush.size(20.0f).spacing(0.25f);
    brush.color(colors::black());

    BrushPoint p0{.pos = {0.0f, 0.0f}, .pressure = 1.0f};
    BrushPoint p1{.pos = {100.0f, 0.0f}, .pressure = 1.0f};

    auto stamps = brush.stroke_segment(p0, p1, 0.0f);
    REQUIRE(!stamps.empty());
    for (const auto& s : stamps) {
        REQUIRE(s.radius > 0.0f);
        REQUIRE(s.opacity >= 0.0f);
        REQUIRE(s.opacity <= 1.0f);
    }
}

TEST_CASE (
"Regression: DepositionParams default compute_opacity in [0,1]"
,
"[kalpana][regression]"
)
 {
    using namespace kalpana::deposit;
    DepositionParams dp{.mode = Mode::Default, .flow = 1.0f};
    for (float alpha : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
        float cov = dp.compute_opacity(0.0f, alpha, 1.0f, 0.5f);
        REQUIRE(cov >= 0.0f);
        REQUIRE(cov <= 1.0f);
    }
}

TEST_CASE (
"Regression: PaintField valid() false on default-constructed"
,
"[kalpana][regression]"
)
 {
    using namespace kalpana;
    PaintField<> empty;
    REQUIRE(!empty.valid());
}

TEST_CASE (
"Regression: BrushProfile to_toml / from_toml round-trips"
,
"[kalpana][regression]"
)
 {
    using namespace kalpana;
    BrushProfile orig;
    orig.size = 24.0f;
    orig.spacing = 0.2f;
    orig.hardness = 0.75f;
    orig.material.metallic = 0.5f;
    orig.material.roughness = 0.4f;
    orig.stabilizer.mode = StabilizerMode::PullLag;
    orig.stabilizer.strength = 0.7f;

    auto toml = to_toml(orig);
    auto loaded = from_toml(toml);

    REQUIRE(loaded.size      == Catch::Approx(orig.size));
    REQUIRE(loaded.spacing   == Catch::Approx(orig.spacing));
    REQUIRE(loaded.hardness  == Catch::Approx(orig.hardness));
    REQUIRE(loaded.material.metallic  == Catch::Approx(orig.material.metallic));
    REQUIRE(loaded.material.roughness == Catch::Approx(orig.material.roughness));
    REQUIRE(loaded.stabilizer.mode     == orig.stabilizer.mode);
    REQUIRE(loaded.stabilizer.strength == Catch::Approx(orig.stabilizer.strength));
}
