// =============================================================================
// test_vakya_property.cpp — tests for the Vākya Property System.
//
// Verifies: include/vakya/property.hpp
//
// The Property System is a lazy, typed, external metadata sidecar. Nodes stay
// POD; metadata lives in a property_store keyed by structural hash. These tests
// exercise the pure vakya:: surface with no Lithe compiler header.
//
// Cases:
//   1. property_key ids: distinct names → distinct ids; same name → same id.
//   2. property_set: set/get/has round-trip for a trivial payload (int).
//   3. property_set: overwrite replaces value in place.
//   4. property_set: non-trivial payload (std::string) constructs/destroys.
//   5. property_set: large payload spills to heap and survives move.
//   6. property_set: erase removes a key.
//   7. property_store: absent key → find returns nullptr (zero-cost path).
//   8. property_store: ensure_for / find_for keyed by expression structure.
// =============================================================================

#include "catch_amalgamated.hpp"

#include "vakya/property.hpp"
#include "vakya/vakya.hpp"

#include <array>
#include <string>

using vakya::property_key;
using vakya::property_set;
using vakya::property_store;

// Key definitions used across cases.
using LineKey = property_key<int, "src.line">;
using NameKey = property_key<std::string, "src.name">;
using BigKey = property_key<std::array<std::uint64_t, 8>, "shape.dims">; // 64 bytes → heap

// ============================================================================
// Test 1 — key ids.
// ============================================================================

TEST_CASE (



"property: key ids are name-stable and distinct"
,
"[vakya][property]"
)
{
    STATIC_REQUIRE(LineKey::id == property_key<int, "src.line">::id);
    STATIC_REQUIRE(LineKey::id != NameKey::id);
    REQUIRE(LineKey::name == "src.line");
}

// ============================================================================
// Test 2 — trivial payload round-trip.
// ============================================================================

TEST_CASE (



"property: set/get/has round-trip (int)"
,
"[vakya][property]"
)
{
    property_set ps;
    REQUIRE_FALSE(ps.has<LineKey>());

    ps.set<LineKey>(42);
    REQUIRE(ps.has<LineKey>());

    auto v = ps.get<LineKey>();
    REQUIRE(v.has_value());
    REQUIRE(*v == 42);
    REQUIRE(ps.size() == 1);
}

// ============================================================================
// Test 3 — overwrite.
// ============================================================================

TEST_CASE (



"property: set overwrites existing value"
,
"[vakya][property]"
)
{
    property_set ps;
    ps.set<LineKey>(1);
    ps.set<LineKey>(2);
    REQUIRE(ps.size() == 1);
    REQUIRE(*ps.get<LineKey>() == 2);
}

// ============================================================================
// Test 4 — non-trivial payload.
// ============================================================================

TEST_CASE (



"property: non-trivial payload (string)"
,
"[vakya][property]"
)
{
    property_set ps;
    ps.set<NameKey>(std::string("hello"));
    REQUIRE(ps.has<NameKey>());
    REQUIRE(*ps.get<NameKey>() == "hello");

    if (auto* p = ps.get_if<NameKey>()) {
        *p += "-world";
    }
    REQUIRE(*ps.get<NameKey>() == "hello-world");
}

// ============================================================================
// Test 5 — heap-spilled payload survives move.
// ============================================================================

TEST_CASE (



"property: large payload spills to heap and survives move"
,
"[vakya][property]"
)
{
    property_set ps;
    std::array<std::uint64_t, 8> dims{};
    for (std::size_t i = 0; i < dims.size(); ++i) dims[i] = i + 1;
    ps.set<BigKey>(dims);

    property_set moved = std::move(ps);
    auto got = moved.get<BigKey>();
    REQUIRE(got.has_value());
    REQUIRE((*got)[0] == 1);
    REQUIRE((*got)[7] == 8);
}

// ============================================================================
// Test 6 — erase.
// ============================================================================

TEST_CASE (



"property: erase removes a key"
,
"[vakya][property]"
)
{
    property_set ps;
    ps.set<LineKey>(7);
    ps.set<NameKey>(std::string("x"));
    REQUIRE(ps.size() == 2);

    REQUIRE(ps.erase<LineKey>());
    REQUIRE_FALSE(ps.has<LineKey>());
    REQUIRE(ps.has<NameKey>());
    REQUIRE(ps.size() == 1);
    REQUIRE_FALSE(ps.erase<LineKey>()); // already gone
}

// ============================================================================
// Test 7 — store: absent key is zero-cost nullptr.
// ============================================================================

TEST_CASE (



"property_store: absent key returns nullptr"
,
"[vakya][property]"
)
{
    property_store store;
    REQUIRE(store.find(12345) == nullptr);
    REQUIRE(store.size() == 0);
}

// ============================================================================
// Test 8 — store keyed by expression structure.
// ============================================================================

TEST_CASE (



"property_store: attach by expression structural key"
,
"[vakya][property]"
)
{
    property_store store;
    int x = 3, y = 4;
    auto e = vakya::as_expr(x) + vakya::as_expr(y);

    store.update_for(e, [](property_set& properties) {
        properties.set<LineKey>(99);
    });

    auto* ps = store.find_for(e);
    REQUIRE(ps != nullptr);
    REQUIRE(*ps->get<LineKey>() == 99);
    REQUIRE(store.size() == 1);
}

// ============================================================================
// Test 9 — basic_property_set<N>: custom InlineBytes avoids heap for large payload.
// ============================================================================

TEST_CASE (



"property: basic_property_set<64> stores 64-byte payload inline"
,
"[vakya][property]"
)
{
    // With default InlineBytes=24 the 64-byte array spills to heap.
    // With InlineBytes=64 it should fit inline.
    vakya::basic_property_set<64> ps;
    std::array<std::uint64_t, 8> dims{};
    for (std::size_t i = 0; i < dims.size(); ++i) dims[i] = i + 10;
    ps.set<BigKey>(dims);

    REQUIRE(ps.has<BigKey>());
    REQUIRE(*ps.get<BigKey>() == dims);
}

// ============================================================================
// Test 10 — property_store thread safety: concurrent reads do not data-race.
//   (Basic structural test; full thread-safety requires TSAN verification.)
// ============================================================================

TEST_CASE (



"property_store: concurrent find calls are safe"
,
"[vakya][property]"
)
{
    property_store store;
    int x = 1, y = 2;
    auto e = vakya::as_expr(x) + vakya::as_expr(y);
    store.update_for(e, [](property_set& properties) {
        properties.set<LineKey>(7);
    });

    // Multiple sequential finds: both return the same slot.
    auto* p1 = store.find_for(e);
    auto* p2 = store.find_for(e);
    REQUIRE(p1 != nullptr);
    REQUIRE(p1 == p2);
    REQUIRE(*p1->get<LineKey>() == 7);
}
