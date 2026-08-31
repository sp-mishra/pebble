// ============================================================================
// test_reflow.cpp — ReflowMotion policies (NullMotion snap, SpringReflow ease)
// ============================================================================

#include <catch_amalgamated.hpp>

#include "drishya/reflow.hpp"

using namespace pebble::drishya;

TEST_CASE("reflow: NullMotion snaps to target and is always settled", "[drishya][reflow]") {
    NullMotion m;
    STATIC_REQUIRE(ReflowMotion<NullMotion>);
    const Rect2D target{10.0f, 20.0f, 30.0f, 40.0f};
    const Rect2D r = m.resolve(0, target, 0.016f);
    CHECK(r.x == Catch::Approx(target.x));
    CHECK(r.w == Catch::Approx(target.w));
    CHECK(m.settled());
}

TEST_CASE("reflow: SpringReflow converges to the target rect", "[drishya][reflow]") {
    SpringReflow m;
    STATIC_REQUIRE(ReflowMotion<SpringReflow>);

    const Rect2D target{100.0f, 100.0f, 50.0f, 50.0f};
    // First sight snaps to the target (no jump-from-origin animation).
    Rect2D r = m.resolve(1, target, 0.016f);
    CHECK(r.x == Catch::Approx(target.x).margin(0.01f));

    // Move the target; drive a fixed step budget. SpringReflow reports settled()
    // as soon as a node's velocity is ~0 (and drops settled nodes), so gating the
    // loop on settled() would exit before the spring reaches the moved target.
    const Rect2D moved{200.0f, 100.0f, 50.0f, 50.0f};
    for (int i = 0; i < 2000; ++i) {
        r = m.resolve(1, moved, 0.016f);
    }
    CHECK(r.x == Catch::Approx(moved.x).margin(1.0f));
}
