#include "catch_amalgamated.hpp"
#include "kalpana/kalpana.hpp"

TEST_CASE (
"Kalpana: Declarative Scene Authoring EDSL"
,
"[kalpana][edsl]"
)
 {
    using namespace kalpana;
    using namespace kalpana::edsl;

    SECTION("NodeBuilder Fluent Construction") {
        Node card = shape(round_rect(10.0f, 10.0f, 200.0f, 100.0f, 12.0f, 12.0f))
            .fill(colors::coral())
            .stroke(colors::black(), 2.0f)
            .opacity(0.9f)
            .effect(shadow(8.0f) | blur(2.0f));

        REQUIRE(card.opacity == 0.9f);
        REQUIRE(card.effects.size() == 2);
        REQUIRE(std::holds_alternative<ShapeNode>(card.content));
    }

    SECTION("TextBuilder Fluent Construction") {
        Node label = text("Kalpana 2.0")
            .fill(pigments::cerulean_blue())
            .font_size(24.0f)
            .position(50.0f, 100.0f)
            .effect(glow(4.0f));

        REQUIRE(std::holds_alternative<TextNode>(label.content));
        const auto& tn = std::get<TextNode>(label.content);
        REQUIRE(tn.text == "Kalpana 2.0");
        REQUIRE(tn.font_size == 24.0f);
        REQUIRE(label.effects.size() == 1);
    }

    SECTION("Scene Stream operator<< Composition") {
        Scene scene;
        scene.clear_color(colors::white());

        scene
        << shape(rect(0.0f, 0.0f, 100.0f, 100.0f))
               .fill(ProceduralFill::marble())
               .effect(shadow(4.0f))
        << text("Header")
               .fill(colors::black())
               .position(10.0f, 20.0f);

        const auto* g = std::get_if<GroupNode>(&scene.root().content);
        REQUIRE(g != nullptr);
        REQUIRE(g->children.size() == 2);
    }
}
