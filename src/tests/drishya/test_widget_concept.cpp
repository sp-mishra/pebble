// ============================================================================
// test_widget_concept.cpp — Drishya widget/painter concept conformance
// ----------------------------------------------------------------------------
// Compile-time checks that the stock widgets satisfy Widget + PaintableWith, and
// that the default painter satisfies Painter / ColorPainter. These are the
// contracts AnyWidget erases against; STATIC_REQUIRE fails the build if a widget
// ever drifts out of conformance.
// ============================================================================

#include <catch_amalgamated.hpp>

#include "drishya/drishya.hpp"

using namespace pebble::drishya;
namespace w = pebble::drishya::widgets;

using M = MonospaceMetrics;
using P = DefaultPainter;

TEST_CASE("drishya: default painter satisfies Painter concepts", "[drishya][concept]") {
    STATIC_REQUIRE(Painter<P>);
    STATIC_REQUIRE(ColorPainter<P>);
    STATIC_REQUIRE(ITextMetrics<M>);
}

TEST_CASE("drishya: container widgets satisfy Widget + PaintableWith", "[drishya][concept]") {
    STATIC_REQUIRE(Widget<w::Stack, M>);
    STATIC_REQUIRE(PaintableWith<w::Stack, P>);
    STATIC_REQUIRE(Widget<w::Panel, M>);
    STATIC_REQUIRE(PaintableWith<w::Panel, P>);
    STATIC_REQUIRE(Widget<w::ScrollArea, M>);
    STATIC_REQUIRE(Widget<w::Grid, M>);
    STATIC_REQUIRE(Widget<w::Splitter, M>);
    STATIC_REQUIRE(Widget<w::Tabs, M>);
    STATIC_REQUIRE(Widget<w::Spacer, M>);
}

TEST_CASE("drishya: display widgets satisfy Widget + PaintableWith", "[drishya][concept]") {
    STATIC_REQUIRE(Widget<w::Label, M>);
    STATIC_REQUIRE(PaintableWith<w::Label, P>);
    STATIC_REQUIRE(Widget<w::Icon, M>);
    STATIC_REQUIRE(Widget<w::Separator, M>);
    STATIC_REQUIRE(Widget<w::Badge, M>);
    STATIC_REQUIRE(Widget<w::Progress, M>);
    STATIC_REQUIRE(Widget<w::Spinner, M>);
    STATIC_REQUIRE(Widget<w::Tooltip, M>);
}

TEST_CASE("drishya: input widgets satisfy Widget + PaintableWith", "[drishya][concept]") {
    STATIC_REQUIRE(Widget<w::Button, M>);
    STATIC_REQUIRE(PaintableWith<w::Button, P>);
    STATIC_REQUIRE(Widget<w::Toggle, M>);
    STATIC_REQUIRE(Widget<w::Checkbox, M>);
    STATIC_REQUIRE(Widget<w::Slider, M>);
    STATIC_REQUIRE(Widget<w::TextField, M>);
    STATIC_REQUIRE(Widget<w::Select, M>);
}

TEST_CASE("drishya: data + game widgets satisfy Widget + PaintableWith", "[drishya][concept]") {
    STATIC_REQUIRE(Widget<w::Sparkline, M>);
    STATIC_REQUIRE(Widget<w::StatTile, M>);
    STATIC_REQUIRE(Widget<w::ListView, M>);
    STATIC_REQUIRE(Widget<w::Table, M>);
    STATIC_REQUIRE(Widget<w::Chat, M>);
    STATIC_REQUIRE(Widget<w::Gauge, M>);
    STATIC_REQUIRE(PaintableWith<w::Gauge, P>);
    STATIC_REQUIRE(Widget<w::RadialMenu, M>);
    STATIC_REQUIRE(Widget<w::Crosshair, M>);
    STATIC_REQUIRE(Widget<w::DamageNumber, M>);
    STATIC_REQUIRE(Widget<w::NinePatch, M>);
    STATIC_REQUIRE(Widget<w::WorldAnchor, M>);
}

TEST_CASE("drishya: stub widgets are concept-complete", "[drishya][concept][stub]") {
    STATIC_REQUIRE(Widget<w::Markdown, M>);
    STATIC_REQUIRE(PaintableWith<w::Markdown, P>);
    STATIC_REQUIRE(Widget<w::Chart, M>);
    STATIC_REQUIRE(Widget<w::Heatmap, M>);
    STATIC_REQUIRE(Widget<w::TreeView, M>);
    STATIC_REQUIRE(Widget<w::Minimap, M>);
    STATIC_REQUIRE(Widget<w::Hotbar, M>);
    STATIC_REQUIRE(Widget<w::DialogueBox, M>);
}

TEST_CASE("drishya: widgets fit the default AnyWidget inline buffer", "[drishya][concept]") {
    // The 512B default buffer must hold every stock widget with no heap. If a
    // widget grows past it, AnyWidget's static_assert would already fail to
    // compile; this run-time check documents the headroom.
    CHECK(sizeof(w::TextField) <= 512);
    CHECK(sizeof(w::Button) <= 512);
    CHECK(sizeof(w::StatTile) <= 512);
    CHECK(sizeof(w::Slider) <= 512);
}
