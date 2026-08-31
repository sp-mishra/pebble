// ============================================================================
// test_layout_bridge.cpp — WidgetTree → akruti solve → resolved rects
// ----------------------------------------------------------------------------
// Drives layout through App (which owns the tree + bridge), then reads back the
// solved rectangle for each NodeId. Verifies the root fills the viewport and a
// vertical stack distributes its children down the main axis.
// ============================================================================

#include <catch_amalgamated.hpp>

#include "drishya/drishya.hpp"

using namespace pebble::drishya;
namespace w = pebble::drishya::widgets;

using M = MonospaceMetrics;
using P = DefaultPainter;

TEST_CASE("layout_bridge: root fills the viewport", "[drishya][layout]") {
    M m;
    App<M, P> app(m);
    auto root_stack = w::vstack();
    root_stack.style_.width = akruti::layout::SizeSpec::Percent(100.0f);
    root_stack.style_.height = akruti::layout::SizeSpec::Percent(100.0f);
    const NodeId root = app.set_root(std::move(root_stack));

    app.set_viewport(Rect2D{0.0f, 0.0f, 400.0f, 300.0f});
    app.solve();

    const Rect2D r = app.layout().rect(root);
    CHECK(r.w == Catch::Approx(400.0f));
    CHECK(r.h == Catch::Approx(300.0f));
}

TEST_CASE("layout_bridge: vstack children stack vertically", "[drishya][layout]") {
    M m;
    App<M, P> app(m);

    auto root_stack = w::vstack();
    root_stack.style_.width = akruti::layout::SizeSpec::Percent(100.0f);
    root_stack.style_.height = akruti::layout::SizeSpec::Percent(100.0f);
    const NodeId root = app.set_root(std::move(root_stack));

    auto a = w::panel();
    a.style_.height = akruti::layout::SizeSpec::Px(50.0f);
    auto b = w::panel();
    b.style_.height = akruti::layout::SizeSpec::Px(70.0f);
    const NodeId na = app.add_child(root, std::move(a));
    const NodeId nb = app.add_child(root, std::move(b));

    app.set_viewport(Rect2D{0.0f, 0.0f, 200.0f, 400.0f});
    app.solve();

    const Rect2D ra = app.layout().rect(na);
    const Rect2D rb = app.layout().rect(nb);

    CHECK(ra.h == Catch::Approx(50.0f));
    CHECK(rb.h == Catch::Approx(70.0f));
    // b sits below a on the column axis.
    CHECK(rb.y >= ra.y + ra.h - 0.001f);
}

TEST_CASE("layout_bridge: hit test finds the top node at a point", "[drishya][layout]") {
    M m;
    App<M, P> app(m);
    auto root_stack = w::vstack();
    root_stack.style_.width = akruti::layout::SizeSpec::Percent(100.0f);
    root_stack.style_.height = akruti::layout::SizeSpec::Percent(100.0f);
    const NodeId root = app.set_root(std::move(root_stack));

    app.set_viewport(Rect2D{0.0f, 0.0f, 100.0f, 100.0f});
    app.solve();

    const NodeId hit = app.layout().hit(50.0f, 50.0f);
    CHECK(hit == root);
}
