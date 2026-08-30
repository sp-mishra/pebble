#include "catch_amalgamated.hpp"
#include "kalpana/color/paint_field.hpp"
#include "kalpana/color/realshader.hpp"
#include "kalpana/brush/material.hpp"
#include "kalpana/color/spectral.hpp"
#include "containers/numeric/math_vector.hpp"

TEST_CASE("RealShaderPass: shade_cell on empty field returns canvas color", "[kalpana][realshader]") {
    using namespace kalpana;
    PaintField<> f(8, 8, 1.0f);
    RealShaderPass<> shader;

    Color canvas{0.9f, 0.9f, 0.9f, 1.0f};
    PaintMaterial mat = PaintMaterial::preset_matte();
    pebble::math::vec3 view{0.0f, 0.0f, 1.0f};

    auto result = shader.shade_cell(f, 4, 4, canvas, mat, view);
    REQUIRE(result.color.a == Catch::Approx(1.0f).margin(0.01f));
}

TEST_CASE("RealShaderPass: metallic preset boosts specular over matte", "[kalpana][realshader]") {
    using namespace kalpana;
    PaintField<> f(8, 8, 1.0f);

    spectral::SpectralColor silver = spectral::SpectralColor::from_color(Color{0.8f, 0.8f, 0.85f, 1.0f});
    PaintField<>::SplatParams sp{
        .cx = 4.0f, .cy = 4.0f, .radius = 3.0f, .opacity = 1.0f,
        .loading = 1.0f, .water_add = 0.0f, .height_add = 0.6f,
        .granulation = 0.0f, .angle = 0.0f, .roundness = 1.0f, .hardness = 1.0f
    };
    f.splat<StampPreset::Round>(sp, silver);

    RealShaderPass<> shader;
    Color base{0.8f, 0.8f, 0.85f, 1.0f};
    pebble::math::vec3 view{0.0f, 0.0f, 1.0f};

    auto matte_r    = shader.shade_cell(f, 4, 4, base, PaintMaterial::preset_matte(),    view);
    auto metal_r    = shader.shade_cell(f, 4, 4, base, PaintMaterial::preset_metallic(), view);

    bool differs = (matte_r.color.r != metal_r.color.r) ||
                   (matte_r.color.g != metal_r.color.g) ||
                   (matte_r.color.b != metal_r.color.b);
    REQUIRE(differs);
}

TEST_CASE("PaintMaterial: presets have expected metallic/roughness values", "[kalpana][material]") {
    using namespace kalpana;
    REQUIRE(PaintMaterial::preset_matte().metallic    == Catch::Approx(0.0f));
    REQUIRE(PaintMaterial::preset_matte().roughness   == Catch::Approx(0.9f));
    REQUIRE(PaintMaterial::preset_metallic().metallic == Catch::Approx(1.0f));
    REQUIRE(PaintMaterial::preset_pencil().roughness  == Catch::Approx(0.95f));
    REQUIRE(PaintMaterial::preset_glossy_oil().roughness < 0.5f);
}
