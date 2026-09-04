// ============================================================================
// test_reactive_signal.cpp — containers::reactive Signal / Computed / Callback
// ----------------------------------------------------------------------------
// Signal<T> is a generic observable value cell (the reactive primitive drishya
// binds widget state to). It notifies zero-arg observers on write; Computed<F>
// memoizes a derived value and recomputes lazily after a dependency changes.
// ============================================================================

#include <catch_amalgamated.hpp>

#include "containers/reactive/signal.hpp"

using namespace containers::reactive;

TEST_CASE (
"reactive::Signal: get / set / operator()"
,
"[containers][reactive]"
)
 {
    Signal<int> s{7};
    CHECK(s.get() == 7);
    CHECK(s() == 7);
    s.set(42);
    CHECK(s.get() == 42);
}

TEST_CASE (
"reactive::Signal: notifies observers on every set()"
,
"[containers][reactive]"
)
 {
    Signal<int> s{0};
    int fired = 0;
    int last = -1;
    const ObserverId id = s.subscribe([&]() noexcept { ++fired; last = s.get(); });

    s.set(1);
    s.set(2);
    CHECK(fired == 2);
    CHECK(last == 2);

    s.unsubscribe(id);
    s.set(3);
    CHECK(fired == 2); // no longer observing
    CHECK(s.observer_count() == 0);
}

TEST_CASE (
"reactive::Signal: set_if_changed skips equal writes"
,
"[containers][reactive]"
)
 {
    Signal<int> s{5};
    int fired = 0;
    s.subscribe([&]() noexcept { ++fired; });

    CHECK_FALSE(s.set_if_changed(5)); // equal -> no notify
    CHECK(fired == 0);
    CHECK(s.set_if_changed(6)); // changed -> notify
    CHECK(fired == 1);
}

TEST_CASE (
"reactive::Signal: mutate edits in place then notifies"
,
"[containers][reactive]"
)
 {
    Signal<int> s{10};
    int fired = 0;
    s.subscribe([&]() noexcept { ++fired; });
    s.mutate([](int& v) { v += 5; });
    CHECK(s.get() == 15);
    CHECK(fired == 1);
}

TEST_CASE (
"reactive::Computed: recomputes lazily after a dependency changes"
,
"[containers][reactive]"
)
 {
    Signal<int> a{2};
    Signal<int> b{3};

    Computed sum{[&]() { return a.get() + b.get(); }};
    sum.depend_on(a);
    sum.depend_on(b);

    CHECK(sum.get() == 5);
    a.set(10);
    CHECK(sum.get() == 13); // recomputed after a changed
    // No dependency change -> cached value returned.
    CHECK(sum.get() == 13);
}

TEST_CASE("Signal: Re-entrant unsubscribe during notify", "[reactive][signal]") {
    containers::reactive::Signal<int> sig(0);
    containers::reactive::ObserverId id1 = containers::reactive::kInvalidObserver;
    bool called2 = false;

    id1 = sig.subscribe([&]() {
        sig.unsubscribe(id1);
    });

    sig.subscribe([&]() {
        called2 = true;
    });

    sig.set(42);
    REQUIRE(called2);
    REQUIRE(sig.observer_count() == 1);
}

