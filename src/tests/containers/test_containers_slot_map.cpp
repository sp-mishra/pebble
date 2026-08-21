// =============================================================================
// test_containers_slot_map.cpp — tests for containers::slot_map
// =============================================================================

#include "catch_amalgamated.hpp"

#include <string>
#include <type_traits>
#include <unordered_set>

#include "containers/associative/slot_map.hpp"

namespace {
    struct item_tag {};

    using ItemHandle = containers::generational_handle<item_tag>;
    using IntMap = containers::slot_map<int, ItemHandle>;
    using StrMap = containers::slot_map<std::string, ItemHandle>;
}

// ---------------------------------------------------------------------------
// Insert returns a valid handle
// ---------------------------------------------------------------------------
TEST_CASE (



"slot_map insert returns valid handle"
,
"[containers][slot_map]"
)
 {
    IntMap m;
    ItemHandle h = m.insert(42);
    REQUIRE_FALSE(h.is_null());
    REQUIRE(m.contains(h));
    REQUIRE(*m.find(h) == 42);
}

// ---------------------------------------------------------------------------
// Erase bumps generation → stale handle find returns nullptr
// ---------------------------------------------------------------------------
TEST_CASE (



"slot_map erase invalidates handle"
,
"[containers][slot_map]"
)
 {
    IntMap m;
    const ItemHandle h = m.insert(100);
    REQUIRE(m.find(h) != nullptr);

    m.erase(h);
    REQUIRE(m.find(h) == nullptr);
    REQUIRE_FALSE(m.contains(h));
}

// ---------------------------------------------------------------------------
// Slot address stability across inserts and erases
// ---------------------------------------------------------------------------
TEST_CASE (



"slot_map: slot address stable across mutations"
,
"[containers][slot_map]"
)
 {
    IntMap m;

    // Insert three items and capture the address of item 1.
    const ItemHandle h0 = m.insert(0);
    const ItemHandle h1 = m.insert(1);
    const ItemHandle h2 = m.insert(2);

    int* addr_h1 = m.find(h1);
    REQUIRE(addr_h1 != nullptr);

    // More inserts
    const ItemHandle h3 = m.insert(3);
    const ItemHandle h4 = m.insert(4);
    // Erase some slots (triggers free-list reuse on next insert)
    m.erase(h0);
    m.erase(h2);
    const ItemHandle h5 = m.insert(5);
    const ItemHandle h6 = m.insert(6);

    // h1's address must still point to the same live value.
    int* addr_h1_after = m.find(h1);
    REQUIRE(addr_h1_after == addr_h1); // pointer stability
    REQUIRE(*addr_h1_after == 1);

    (void)h3; (void)h4; (void)h5; (void)h6;
}

// ---------------------------------------------------------------------------
// Dense iteration visits only live slots
// ---------------------------------------------------------------------------
TEST_CASE (



"slot_map dense iteration visits only live slots"
,
"[containers][slot_map]"
)
 {
    IntMap m;

    const ItemHandle h0 = m.insert(10);
    const ItemHandle h1 = m.insert(20);
    const ItemHandle h2 = m.insert(30);
    const ItemHandle h3 = m.insert(40);
    m.erase(h1);
    m.erase(h3);

    std::unordered_set<int> seen;
    for (auto ref : m) seen.insert(ref.value);

    REQUIRE(seen.size() == 2);
    REQUIRE(seen.count(10) == 1);
    REQUIRE(seen.count(30) == 1);
    REQUIRE(seen.count(20) == 0);
    REQUIRE(seen.count(40) == 0);

    (void)h0; (void)h2;
}

// ---------------------------------------------------------------------------
// Free-list slot reuse: new handle has a different generation
// ---------------------------------------------------------------------------
TEST_CASE (



"slot_map reused slot has bumped generation"
,
"[containers][slot_map]"
)
 {
    IntMap m;
    const ItemHandle h = m.insert(1);
    const auto old_gen = h.generation;
    m.erase(h);

    const ItemHandle h2 = m.insert(2); // reuses the freed slot
    REQUIRE(h2.index == h.index);      // same physical slot
    REQUIRE(h2.generation != old_gen); // new generation

    REQUIRE(m.find(h)  == nullptr);    // stale
    REQUIRE(m.find(h2) != nullptr);    // live
}

// ---------------------------------------------------------------------------
// size() / empty()
// ---------------------------------------------------------------------------
TEST_CASE (



"slot_map size and empty"
,
"[containers][slot_map]"
)
 {
    IntMap m;
    REQUIRE(m.empty());
    REQUIRE(m.size() == 0);

    const ItemHandle h0 = m.insert(1);
    const ItemHandle h1 = m.insert(2);
    REQUIRE(m.size() == 2);
    REQUIRE_FALSE(m.empty());

    m.erase(h0);
    REQUIRE(m.size() == 1);

    m.erase(h1);
    REQUIRE(m.empty());
}

// ---------------------------------------------------------------------------
// Erase of null / stale handle is no-op
// ---------------------------------------------------------------------------
TEST_CASE (



"slot_map erase of null or stale handle is no-op"
,
"[containers][slot_map]"
)
 {
    IntMap m;
    m.erase(ItemHandle{}); // null — no crash
    m.erase(ItemHandle{99, 99}); // out-of-range — no crash

    const ItemHandle h = m.insert(5);
    m.erase(h);
    m.erase(h); // double-erase — no crash, stale handle
    REQUIRE(m.size() == 0);
}
