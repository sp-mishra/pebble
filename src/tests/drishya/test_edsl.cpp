// ============================================================================
// test_edsl.cpp — compile-time tree DSL: modifiers, literals, mount shape
// ============================================================================

#include <catch_amalgamated.hpp>

#include "drishya/drishya.hpp"
#include "drishya/edsl.hpp"

using namespace pebble::drishya;
using namespace pebble::drishya::edsl;

using M = MonospaceMetrics;
using P = DefaultPainter;

TEST_CASE (
"edsl: _px literal yields a Px SizeSpec"
,
"[drishya][edsl]"
)
 {
    const auto s = 240_px;
    CHECK(s.kind == akruti::layout::SizeSpec::Kind::Px);
    CHECK(s.value == Catch::Approx(240.0f));
}

TEST_CASE (
"edsl: modifiers set style fields via operator|"
,
"[drishya][edsl]"
)
 {
    auto w = vstack_() | pad(12) | flex(2.0f) | align(Align::Center);
    CHECK(w.style_.padding.l == Catch::Approx(12.0f));
    CHECK(w.style_.flex_grow == Catch::Approx(2.0f));
    CHECK(w.style_.align_items == Align::Center);
}

TEST_CASE (
"edsl: node tree mounts into the App with the right shape"
,
"[drishya][edsl]"
)
 {
    M m;
    App<M, P> app(m);

    auto view = node(vstack_(16) | pad(8),
                     label_("Title"),
                     node(hstack_(),
                          button_("OK") | width(80_px),
                          button_("Cancel")));
    const NodeId root = view.mount(app);

    CHECK(app.tree().root() == root);
    // root + label + hstack + 2 buttons = 5 nodes.
    CHECK(app.tree().node_count() == 5);
}
