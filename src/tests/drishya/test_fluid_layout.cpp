// ============================================================================
// test_fluid_layout.cpp — additive SizeSpec units through the drishya App
// ----------------------------------------------------------------------------
// Phase-1 added Fr / Content / Aspect / Clamp to akruti::layout::SizeSpec. This
// exercises them end-to-end via the App (tree → bridge → engine solve → rect):
//   * Fr(1)/Fr(1) split a row into equal halves; Fr(1)/Fr(2) into 1:2.
//   * Clamp bounds a Percent preference between a min and max px window.
//   * Aspect derives the cross axis from the resolved main axis by a ratio.
// ============================================================================

#include <catch_amalgamated.hpp>

#include "drishya/drishya.hpp"

using namespace pebble::drishya;
namespace w = pebble::drishya::widgets;
namespace L = akruti::layout;

using M = MonospaceMetrics;
using P = DefaultPainter;

namespace {
// A row container whose children divide the main axis.
auto make_row() {
    auto row = w::hstack();
    row.style_.width = L::SizeSpec::Percent(100.0f);
    row.style_.height = L::SizeSpec::Percent(100.0f);
    return row;
}
} // namespace

TEST_CASE("fluid: Fr(1)/Fr(1) splits a row into equal halves", "[drishya][fluid]") {
    M m;
    App<M, P> app(m);
    const NodeId root = app.set_root(make_row());

    auto a = w::panel();
    a.style_.width = L::SizeSpec::Fr(1.0f);
    auto b = w::panel();
    b.style_.width = L::SizeSpec::Fr(1.0f);
    const NodeId na = app.add_child(root, std::move(a));
    const NodeId nb = app.add_child(root, std::move(b));

    app.set_viewport(Rect2D{0.0f, 0.0f, 300.0f, 100.0f});
    app.solve();

    const Rect2D ra = app.layout().rect(na);
    const Rect2D rb = app.layout().rect(nb);
    CHECK(ra.w == Catch::Approx(150.0f).margin(0.5f));
    CHECK(rb.w == Catch::Approx(150.0f).margin(0.5f));
}

TEST_CASE("fluid: Fr(1)/Fr(2) splits a row 1:2", "[drishya][fluid]") {
    M m;
    App<M, P> app(m);
    const NodeId root = app.set_root(make_row());

    auto a = w::panel();
    a.style_.width = L::SizeSpec::Fr(1.0f);
    auto b = w::panel();
    b.style_.width = L::SizeSpec::Fr(2.0f);
    const NodeId na = app.add_child(root, std::move(a));
    const NodeId nb = app.add_child(root, std::move(b));

    app.set_viewport(Rect2D{0.0f, 0.0f, 300.0f, 100.0f});
    app.solve();

    const Rect2D ra = app.layout().rect(na);
    const Rect2D rb = app.layout().rect(nb);
    CHECK(ra.w == Catch::Approx(100.0f).margin(0.5f));
    CHECK(rb.w == Catch::Approx(200.0f).margin(0.5f));
    CHECK(rb.w == Catch::Approx(ra.w * 2.0f).margin(1.0f));
}

TEST_CASE("fluid: Clamp bounds a preferred size between min and max", "[drishya][fluid]") {
    M m;
    App<M, P> app(m);
    const NodeId root = app.set_root(make_row());

    // Prefer 100% of a wide parent, but clamp the width to [40, 120].
    auto child = w::panel();
    child.style_.width = L::SizeSpec::Percent(100.0f);
    child.style_.width_clamp = L::SizeSpecClamp{
        L::SizeSpec::Px(40.0f), L::SizeSpec::Px(120.0f), L::SizeSpec::Px(120.0f)};
    const NodeId nc = app.add_child(root, std::move(child));

    app.set_viewport(Rect2D{0.0f, 0.0f, 400.0f, 100.0f});
    app.solve();

    const Rect2D rc = app.layout().rect(nc);
    CHECK(rc.w <= 120.0f + 0.5f);
    CHECK(rc.w >= 40.0f - 0.5f);
}

TEST_CASE("fluid: Aspect derives the cross axis from the main axis", "[drishya][fluid]") {
    M m;
    App<M, P> app(m);
    const NodeId root = app.set_root(make_row());

    // Fixed width, height follows a 2:1 (w:h) aspect ratio → half the width.
    auto box = w::panel();
    box.style_.width = L::SizeSpec::Px(120.0f);
    box.style_.height = L::SizeSpec::Aspect(2.0f);
    const NodeId nb = app.add_child(root, std::move(box));

    app.set_viewport(Rect2D{0.0f, 0.0f, 300.0f, 300.0f});
    app.solve();

    const Rect2D rb = app.layout().rect(nb);
    CHECK(rb.w == Catch::Approx(120.0f).margin(0.5f));
    CHECK(rb.h == Catch::Approx(60.0f).margin(1.0f));
}
