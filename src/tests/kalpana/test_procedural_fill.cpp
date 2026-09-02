#include "catch_amalgamated.hpp"
#include "kalpana/kalpana.hpp"

TEST_CASE (
"Kalpana: Procedural Noise and Fills"
,
"[kalpana][fill][noise]"
)
 {
    using namespace kalpana;
    using namespace kalpana::noise;

    SECTION("Noise Generators Value Ranges") {
        float s = simplex(12.34f, 56.78f);
        REQUIRE(s >= -1.05f);
        REQUIRE(s <= 1.05f);

        float f = fbm(5.0f, 10.0f, 4);
        REQUIRE(f >= -1.05f);
        REQUIRE(f <= 1.05f);

        float w = worley(3.5f, 7.2f);
        REQUIRE(w >= 0.0f);
        REQUIRE(w <= 1.0f);

        float turb = turbulence(2.0f, 4.0f, 4);
        REQUIRE(turb >= 0.0f);
    }

    SECTION("Built-in Procedural Fill Descriptors") {
        auto paper = ProceduralFill::paper_texture();
        float p_val = paper.evaluate(100.0f, 200.0f);
        REQUIRE(p_val >= 0.0f);
        REQUIRE(p_val <= 1.0f);

        auto marble = ProceduralFill::marble();
        float m_val = marble.evaluate(50.0f, 50.0f);
        REQUIRE(m_val >= 0.0f);
        REQUIRE(m_val <= 1.0f);

        auto wood = ProceduralFill::wood();
        float w_val = wood.evaluate(10.0f, 20.0f);
        REQUIRE(w_val >= 0.0f);
        REQUIRE(w_val <= 1.0f);
    }

    SECTION("Plug-and-Play Custom Noise Generators") {
        // Custom user noise struct
        struct CheckerNoise {
            float evaluate(float x, float y) const noexcept {
                int ix = static_cast<int>(std::floor(x));
                int iy = static_cast<int>(std::floor(y));
                return ((ix + iy) % 2 == 0) ? 1.0f : 0.0f;
            }
        };

        auto custom_fill = ProceduralFill::custom_noise(CheckerNoise{});
        float v0 = custom_fill.evaluate(0.5f, 0.5f);
        float v1 = custom_fill.evaluate(1.5f, 0.5f);
        REQUIRE(v0 != v1);
    }

    SECTION("Color Modulation by Procedural Fill") {
        auto grain_fill = ProceduralFill::grain(0.1f);
        Color base = colors::coral();
        Color modulated = grain_fill.modulate(base, 10.0f, 20.0f);
        REQUIRE(modulated.a == base.a);
        REQUIRE(modulated.r > 0.0f);
    }
}
