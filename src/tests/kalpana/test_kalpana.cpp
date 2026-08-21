#include "catch_amalgamated.hpp"
#include "kalpana/kalpana.hpp"

TEST_CASE("Kalpana: Kubelka-Munk Spectral Pigment Mixing (Blue + Yellow -> Green)", "[kalpana][color][spectral]") {
    kalpana::Color blue = kalpana::colors::blue();
    kalpana::Color yellow = kalpana::colors::yellow();

    // In standard linear RGB: (0,0,1) + (1,1,0) = (0.5, 0.5, 0.5) where red is equal to green (muddy grey)
    kalpana::Color linear_mix = blue.lerp(yellow, 0.5f);
    REQUIRE(linear_mix.r >= 0.5f);
    REQUIRE(linear_mix.r == linear_mix.g);

    // In Kubelka-Munk Spectral Subtractive Mixing: Red is absorbed by blue pigment, Green dominates over Red!
    kalpana::Color spectral_mix = kalpana::spectral::mix(blue, yellow, 0.5f);
    REQUIRE(spectral_mix.g > spectral_mix.r);
}

TEST_CASE("Kalpana: Vector Path Construction & Akruti Spline Bridge", "[kalpana][geom][path]") {
    kalpana::Path path;
    path.round_rect(10.0f, 10.0f, 80.0f, 40.0f, 5.0f, 5.0f);

    REQUIRE_FALSE(path.empty());
    REQUIRE(path.verbs().size() >= 5);

    // Akruti Spline import
    akruti::CubicBezierCurve bezier{
        .p0 = {0.0f, 0.0f},
        .p1 = {10.0f, 20.0f},
        .p2 = {20.0f, 20.0f},
        .p3 = {30.0f, 0.0f}
    };

    auto spline_path = kalpana::Path::from_bezier(bezier);
    REQUIRE(spline_path.verbs().size() == 2); // Move + Cubic
    REQUIRE(spline_path.points().size() == 4);
}

TEST_CASE("Kalpana: Scene Graph Composition & Headless Scanline Canvas", "[kalpana][scene][canvas]") {
    kalpana::Scene scene;
    scene.clear_color(kalpana::colors::white());

    kalpana::Path r;
    r.rect(10.0f, 10.0f, 30.0f, 30.0f);

    scene.add(kalpana::Node::shape(r, kalpana::Paint::fill(kalpana::colors::red())));

    kalpana::DefaultCanvas canvas(64, 64);
    canvas.render(scene);

    auto pixels = canvas.snapshot();
    REQUIRE(pixels.size() == 64 * 64);

    // Check that red pixels were rendered in the interior
    const std::uint32_t red_argb = kalpana::colors::red().to_argb8888();
    const std::uint32_t sample_px = pixels[20 * 64 + 20];
    REQUIRE(sample_px == red_argb);
}

TEST_CASE("Kalpana: Realtime Brush Pressure Dynamics & Stamp Emission", "[kalpana][brush]") {
    kalpana::Brush brush;
    brush.size(20.0f).spacing(0.2f).color(kalpana::colors::coral());

    kalpana::BrushPoint p0{.pos = {0.0f, 0.0f}, .pressure = 0.5f};
    kalpana::BrushPoint p1{.pos = {50.0f, 0.0f}, .pressure = 1.0f};

    auto stamps = brush.stroke_segment(p0, p1);
    REQUIRE_FALSE(stamps.empty());
    REQUIRE(stamps.size() >= 10);

    // Radius should smoothly expand as pressure increases from 0.5 to 1.0
    REQUIRE(stamps.front().radius < stamps.back().radius);
}
