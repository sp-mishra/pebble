// ============================================================================
// test_reactive.cpp — containers::reactive Signal / Computed / Callback
// ----------------------------------------------------------------------------
// The reactive layer Drishya binds widget state to. A Signal notifies observers
// on set(); a Computed memoizes and re-evaluates after a dependency changes.
// ============================================================================

#include <catch_amalgamated.hpp>

#include "drishya/reactive.hpp"

using namespace pebble::drishya;

TEST_CASE("reactive: signal get/set", "[drishya][reactive]") {
    Signal<int> s{7};
    CHECK(s.get() == 7);
    CHECK(s() == 7);
    s.set(42);
    CHECK(s.get() == 42);
}

TEST_CASE("reactive: signal notifies observers on set", "[drishya][reactive]") {
    Signal<int> s{0};
    int fired = 0;
    int last = -1;
    const auto id = s.subscribe([&]() noexcept { ++fired; last = s.get(); });

    s.set(1);
    s.set(2);
    CHECK(fired == 2);
    CHECK(last == 2);

    s.unsubscribe(id);
    s.set(3);
    CHECK(fired == 2); // no longer observing
}

TEST_CASE("reactive: setting the same value still tracks correctly", "[drishya][reactive]") {
    Signal<int> s{5};
    int fired = 0;
    s.subscribe([&]() noexcept { ++fired; });
    s.set(5);
    // notify semantics: observers are told on every set() call.
    CHECK(fired >= 1);
}

TEST_CASE("reactive: bind mirrors a signal into a setter", "[drishya][reactive]") {
    Signal<int> src{1};
    int mirror = 0;
    bind(src, [&](const int& v) { mirror = v; });
    src.set(99);
    CHECK(mirror == 99);
}
