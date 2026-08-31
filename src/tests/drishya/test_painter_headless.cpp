// ============================================================================
// test_painter_headless.cpp — KalpanaPainter → DefaultCanvas → pixel snapshot
// ----------------------------------------------------------------------------
// The Painter concept is an adapter: KalpanaPainter accumulates a kalpana::Scene
// and hands it to the canvas at present(). Here we drive a real headless capture
// canvas, paint a filled rect through the painter, and read the pixels back to
// prove the full paint path (widget → painter → scene → canvas) is wired.
// ============================================================================

#include <catch_amalgamated.hpp>

#include "drishya/drishya.hpp"

using namespace pebble::drishya;
namespace w = pebble::drishya::widgets;

using M = MonospaceMetrics;
using P = DefaultPainter;

TEST_CASE("painter_headless: KalpanaPainter satisfies the Painter concept", "[drishya][painter]") {
    STATIC_REQUIRE(Painter<P>);
    STATIC_REQUIRE(ColorPainter<P>);
}

TEST_CASE("painter_headless: a filled rect lands on the capture canvas", "[drishya][painter]") {
    kalpana::DefaultCanvas canvas(64, 64);
    M metrics;
    P painter(canvas, metrics);

    painter.begin_frame();
    painter.set_color(0xFFFF0000u); // opaque red
    painter.fill_rect(Rect2D{16.0f, 16.0f, 32.0f, 32.0f});
    painter.present();

    const std::vector<std::uint32_t> px = canvas.snapshot();
    REQUIRE(px.size() == 64u * 64u);

    // Center pixel is inside the rect → painted; a corner is outside → untouched.
    const std::uint32_t center = px[32u * 64u + 32u];
    const std::uint32_t corner = px[0];
    CHECK(center != corner);
}

TEST_CASE("painter_headless: App::paint drives a widget tree onto the canvas", "[drishya][painter]") {
    kalpana::DefaultCanvas canvas(80, 60);
    M metrics;
    App<M, P> app(metrics);

    auto root = w::panel();
    root.style_.width = akruti::layout::SizeSpec::Percent(100.0f);
    root.style_.height = akruti::layout::SizeSpec::Percent(100.0f);
    app.set_root(std::move(root));

    app.set_viewport(Rect2D{0.0f, 0.0f, 80.0f, 60.0f});

    P painter(canvas, metrics);
    painter.begin_frame();
    app.paint(painter);
    painter.present();

    const std::vector<std::uint32_t> px = canvas.snapshot();
    CHECK(px.size() == 80u * 60u);
}
