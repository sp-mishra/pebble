#include "catch_amalgamated.hpp"
#include "kalpana/kalpana.hpp"

TEST_CASE("Kalpana: Effect Node Factory and Chain Composition", "[kalpana][effect]") {
    using namespace kalpana;

    SECTION("Individual EffectNode construction") {
        auto b = blur(10.0f);
        auto s = shadow(6.0f, {3.0f, 3.0f});
        auto t = tint(colors::coral(), 0.8f);
        auto g = grain(0.05f);

        REQUIRE(std::holds_alternative<BlurEffect>(b.data()));
        REQUIRE(std::get<BlurEffect>(b.data()).radius == 10.0f);
        REQUIRE(std::holds_alternative<DropShadowEffect>(s.data()));
        REQUIRE(std::holds_alternative<TintEffect>(t.data()));
        REQUIRE(std::holds_alternative<GrainEffect>(g.data()));
    }

    SECTION("EffectChain pipe operator| composition") {
        EffectChain chain = shadow(8.0f) | blur(4.0f) | glow(3.0f) | grain(0.02f);
        REQUIRE(chain.size() == 4);
        REQUIRE_FALSE(chain.empty());

        auto nodes = chain.nodes();
        REQUIRE(std::holds_alternative<DropShadowEffect>(nodes[0].data()));
        REQUIRE(std::holds_alternative<BlurEffect>(nodes[1].data()));
        REQUIRE(std::holds_alternative<GlowEffect>(nodes[2].data()));
        REQUIRE(std::holds_alternative<GrainEffect>(nodes[3].data()));
    }

    SECTION("Ranges-style operator>> composition") {
        EffectChain chain = backdrop_blur(20.0f) >> inner_glow(2.0f) >> spectral_tint(pigments::cerulean_blue(), 0.5f);
        REQUIRE(chain.size() == 3);

        auto nodes = chain.nodes();
        REQUIRE(std::holds_alternative<BackdropBlurEffect>(nodes[0].data()));
        REQUIRE(std::holds_alternative<InnerGlowEffect>(nodes[1].data()));
        REQUIRE(std::holds_alternative<SpectralTintEffect>(nodes[2].data()));
    }

    SECTION("Configurable SmallVector storage for EffectChain") {
        BasicEffectChain<512> large_chain;
        large_chain.add(blur(2.0f));
        large_chain.add(shadow(4.0f));
        REQUIRE(large_chain.size() == 2);
    }
}
