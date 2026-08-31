// ============================================================================
// test_router.cpp — pointer routing, capture, and callback dispatch
// ----------------------------------------------------------------------------
// A button that fills the viewport should receive a press+release and fire its
// on_click. A slider grabs the pointer on press (capture) and follows drag until
// release. Input edges are expressed via InputFrame::{buttons, prev_buttons}.
// ============================================================================

#include <catch_amalgamated.hpp>

#include "drishya/drishya.hpp"

using namespace pebble::drishya;
namespace w = pebble::drishya::widgets;

using M = MonospaceMetrics;
using P = DefaultPainter;

namespace {
InputFrame press_at(float x, float y) {
    InputFrame f;
    f.pointer = Vec2{x, y};
    f.buttons = kPointerLeft;
    f.prev_buttons = kPointerNone; // down edge this frame
    return f;
}
InputFrame release_at(float x, float y) {
    InputFrame f;
    f.pointer = Vec2{x, y};
    f.buttons = kPointerNone;
    f.prev_buttons = kPointerLeft; // up edge this frame
    return f;
}
} // namespace

TEST_CASE("router: button fires on_click on press+release", "[drishya][router]") {
    M m;
    App<M, P> app(m);

    int clicks = 0;
    auto btn = w::button("Go");
    btn.style_.width = akruti::layout::SizeSpec::Percent(100.0f);
    btn.style_.height = akruti::layout::SizeSpec::Percent(100.0f);
    btn.on_click = Callback{[&clicks]() noexcept { ++clicks; }};
    app.set_root(std::move(btn));

    app.set_viewport(Rect2D{0.0f, 0.0f, 120.0f, 40.0f});
    app.solve();

    auto down = app.pump(press_at(60.0f, 20.0f));
    CHECK(down.pointer_handled);
    auto up = app.pump(release_at(60.0f, 20.0f));
    CHECK(up.pointer_handled);
    CHECK(clicks == 1);
}

TEST_CASE("router: press outside the button does not fire", "[drishya][router]") {
    M m;
    App<M, P> app(m);

    int clicks = 0;
    auto root = w::vstack();
    root.style_.width = akruti::layout::SizeSpec::Percent(100.0f);
    root.style_.height = akruti::layout::SizeSpec::Percent(100.0f);
    const NodeId rid = app.set_root(std::move(root));

    auto btn = w::button("Go");
    btn.style_.width = akruti::layout::SizeSpec::Px(40.0f);
    btn.style_.height = akruti::layout::SizeSpec::Px(20.0f);
    btn.on_click = Callback{[&clicks]() noexcept { ++clicks; }};
    app.add_child(rid, std::move(btn));

    app.set_viewport(Rect2D{0.0f, 0.0f, 200.0f, 200.0f});
    app.solve();

    app.pump(press_at(180.0f, 180.0f));   // far corner, off the button
    app.pump(release_at(180.0f, 180.0f));
    CHECK(clicks == 0);
}
