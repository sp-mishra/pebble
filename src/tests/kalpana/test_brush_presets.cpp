#include "catch_amalgamated.hpp"
#include "kalpana/brush/brush_preset.hpp"
#include "kalpana/brush/material.hpp"
#include "kalpana/brush/deposition.hpp"
#include "kalpana/color/paint_field.hpp"
#include "containers/matrix/field.hpp"

TEST_CASE("BrushPreset: original 5 presets unchanged", "[kalpana][brush_preset]") {
    using namespace kalpana;

    SECTION("watercolor_wash") {
        auto p = BrushPreset::watercolor_wash();
        REQUIRE(p.water.wetness == Catch::Approx(0.9f));
        REQUIRE(p.stamp_preset == StampPreset::Round);
    }
    SECTION("oil_impasto") {
        auto p = BrushPreset::oil_impasto();
        REQUIRE(p.impasto.impasto_thickness == Catch::Approx(0.85f));
        REQUIRE(p.stamp_preset == StampPreset::Bristle);
    }
    SECTION("ink_pen") {
        auto p = BrushPreset::ink_pen();
        REQUIRE(p.stamp_preset == StampPreset::Chisel);
    }
    SECTION("dry_pastel") {
        auto p = BrushPreset::dry_pastel();
        REQUIRE(p.impasto.granulation == Catch::Approx(0.9f));
    }
    SECTION("soft_airbrush") {
        auto p = BrushPreset::soft_airbrush();
        REQUIRE(p.stamp_preset == StampPreset::Airbrush);
    }
}

TEST_CASE("BrushPreset: new presets have correct material", "[kalpana][brush_preset]") {
    using namespace kalpana;

    SECTION("graphite_pencil") {
        auto p = BrushPreset::graphite_pencil();
        REQUIRE(p.material.roughness == Catch::Approx(0.95f));
        REQUIRE(p.material.metallic  == Catch::Approx(0.0f));
        REQUIRE(p.deposition.grain_scale >= 1.5f);
    }
    SECTION("metallic_paint") {
        auto p = BrushPreset::metallic_paint();
        REQUIRE(p.material.metallic  == Catch::Approx(1.0f));
        REQUIRE(p.material.roughness == Catch::Approx(0.25f));
        REQUIRE(p.impasto.impasto_thickness >= 0.5f);
    }
    SECTION("rough_feather") {
        auto p = BrushPreset::rough_feather();
        REQUIRE(p.material.roughness >= 0.75f);
    }
    SECTION("express_oils") {
        auto p = BrushPreset::express_oils();
        REQUIRE(p.impasto.smudge_rate >= 0.5f);
        REQUIRE(p.deposition.opacity_body == deposit::WatercolorBody::Opaque);
    }
    SECTION("gouache") {
        auto p = BrushPreset::gouache();
        REQUIRE(p.deposition.opacity_body == deposit::WatercolorBody::Opaque);
        REQUIRE(p.water.drying_rate >= 0.15f);
    }
}

TEST_CASE("DepositionParams: WatercolorBody opaque raises floor", "[kalpana][deposit]") {
    using namespace kalpana::deposit;

    DepositionParams dp_transparent{
        .mode = Mode::Watercolor, .flow = 0.5f, .buildup_rate = 0.5f,
        .opacity_body = WatercolorBody::Transparent
    };
    DepositionParams dp_opaque{
        .mode = Mode::Watercolor, .flow = 0.5f, .buildup_rate = 0.5f,
        .opacity_body = WatercolorBody::Opaque
    };

    // Opaque body should yield equal or higher coverage at center alpha
    float transp = dp_transparent.compute_opacity(0.0f, 0.8f, 1.0f, 0.5f);
    float opaque  = dp_opaque.compute_opacity(0.0f, 0.8f, 1.0f, 0.5f);
    REQUIRE(opaque >= transp);
}

TEST_CASE("DepositionParams: deposit_to_field writes km and height", "[kalpana][deposit]") {
    using namespace kalpana;
    using namespace kalpana::deposit;

    // Use ga::Field directly as the FieldT template arg
    ga::Field<PaintChannels::COUNT, float> field(8, 8, 1.0f);

    DepositionParams dp{.mode = Mode::Default, .flow = 1.0f};
    dp.deposit_to_field(field, 4, 4,
        PaintChannels::KM_START, PaintChannels::HEIGHT,
        0.8f, 1.0f, 0.5f, 1.0f, 0.3f);

    REQUIRE(field.channel(PaintChannels::KM_START).at(4, 4) > 0.0f);
    REQUIRE(field.channel(PaintChannels::HEIGHT).at(4, 4) > 0.0f);
}
