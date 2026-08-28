#include "catch_amalgamated.hpp"
#include "kalpana/kalpana.hpp"

TEST_CASE("Kalpana: Layer Compositing and Combiner Policies", "[kalpana][layer]") {
    using namespace kalpana;

    SECTION("Layer Properties and Nodes") {
        Layer layer("Background");
        layer.opacity(0.8f)
             .blend(BlendMode::Multiply)
             .visible(true);

        Path r = rect(0.0f, 0.0f, 100.0f, 100.0f);
        layer.add(Node::shape(r, Paint::fill(colors::blue())));

        REQUIRE(layer.name() == "Background");
        REQUIRE(layer.opacity() == 0.8f);
        REQUIRE(layer.blend() == BlendMode::Multiply);
        REQUIRE(layer.nodes().size() == 1);
    }

    SECTION("Layer Combiner Policies") {
        Color dst = colors::yellow();
        Color src = colors::blue();

        // Spectral Subtractive Combiner: blue + yellow -> green
        LayerCombiner km = LayerCombiner::spectral();
        Color mixed_km = km.combine(dst, src, 1.0f);
        REQUIRE(mixed_km.g > mixed_km.r);

        // Photoshop Multiply Combiner: yellow (1,1,0) * blue (0,0,1) -> (0,0,0) black
        LayerCombiner mult = LayerCombiner::blend_mode(BlendMode::Multiply);
        Color mixed_mult = mult.combine(dst, src, 1.0f);
        REQUIRE(mixed_mult.r < 0.1f);
        REQUIRE(mixed_mult.g < 0.1f);

        // Custom User Combiner Lambda
        LayerCombiner custom = LayerCombiner::custom([](const Color& d, const Color& s, float) {
            return Color{d.r * 0.5f + s.r * 0.5f, d.g, d.b, 1.0f};
        });
        Color custom_res = custom.combine(dst, src, 1.0f);
        REQUIRE(custom_res.r > 0.0f);
    }

    SECTION("LayerStack Operations") {
        LayerStack stack;
        auto& paper = stack.add("paper");
        auto& paint = stack.add("paint");
        auto& ink = stack.add("ink");

        REQUIRE(stack.size() == 3);
        REQUIRE(stack[0].name() == "paper");
        REQUIRE(stack[1].name() == "paint");
        REQUIRE(stack[2].name() == "ink");

        // Reorder
        stack.reorder(2, 0); // Move ink to bottom
        REQUIRE(stack[0].name() == "ink");
        REQUIRE(stack[1].name() == "paper");

        // Flatten
        stack[0].add(Node::shape(circle(10.0f, 10.0f, 5.0f), Paint::fill(colors::red())));
        stack[1].add(Node::shape(circle(20.0f, 20.0f, 5.0f), Paint::fill(colors::black())));

        Layer flat = stack.flatten();
        REQUIRE(flat.nodes().size() == 2);
    }
}
