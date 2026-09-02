#include "catch_amalgamated.hpp"
#include "containers/associative/SparseSet.hpp"
#include "mem/smriti.hpp"
#include "mem/arena.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

using namespace sparseset;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
using U32Set = SparseSet<std::uint32_t>;
using U32Map = SparseSet<std::uint32_t, std::string>;

enum class EntityId : std::uint32_t {};

// Convenience: insert and assert success in one call.
template <typename S, typename K, typename... V>
static void ins(S& s, K k, V&&... v) {
    REQUIRE(s.insert(k, std::forward<V>(v)...).has_value());
}

template <typename S, typename K>
static void rem(S& s, K k) {
    REQUIRE(s.remove(k).has_value());
}

// ---------------------------------------------------------------------------
// 1. Construction & basic properties
// ---------------------------------------------------------------------------
TEST_CASE (



"[SparseSet] Default construction"
,
"[SparseSet]"
)
 {
    U32Set s;
    REQUIRE(s.empty());
    REQUIRE(s.empty());
    REQUIRE(s.capacity() == 0);
}

TEST_CASE (



"[SparseSet] Construction with universe capacity"
,
"[SparseSet]"
)
 {
    U32Set s(256);
    REQUIRE(s.empty());
    REQUIRE(s.capacity() == 256);
}

// ---------------------------------------------------------------------------
// 2. Insert & contains
// ---------------------------------------------------------------------------
TEST_CASE (



"[SparseSet] Insert single key"
,
"[SparseSet]"
)
 {
    U32Set s(64);
    auto result = s.insert(7u);
    REQUIRE(result.has_value());
    REQUIRE(s.contains(7u));
    REQUIRE(s.size() == 1);
    REQUIRE(!s.empty());
}

TEST_CASE (



"[SparseSet] Insert returns KeyAlreadyExists for duplicate"
,
"[SparseSet]"
)
 {
    U32Set s(64);
    ins(s, 3u);
    auto dup = s.insert(3u);
    REQUIRE(!dup.has_value());
    REQUIRE(dup.error() == SSError::KeyAlreadyExists);
    REQUIRE(s.size() == 1);
}

TEST_CASE (



"[SparseSet] Insert returns KeyOutOfRange when key >= capacity"
,
"[SparseSet]"
)
 {
    U32Set s(8);
    auto r = s.insert(100u);
    REQUIRE(!r.has_value());
    REQUIRE(r.error() == SSError::KeyOutOfRange);
}

TEST_CASE (



"[SparseSet] insert_or_update auto-reserves and upserts"
,
"[SparseSet]"
)
 {
    U32Map s;
    s.insert_or_update(5u, "hello");
    REQUIRE(s.contains(5u));
    REQUIRE(s.get(5u)->get() == "hello");

    s.insert_or_update(5u, "world");
    REQUIRE(s.get(5u)->get() == "world");
    REQUIRE(s.size() == 1);
}

// ---------------------------------------------------------------------------
// 3. Remove
// ---------------------------------------------------------------------------
TEST_CASE (



"[SparseSet] Remove existing key"
,
"[SparseSet]"
)
 {
    U32Set s(32);
    ins(s, 10u);
    ins(s, 20u);
    ins(s, 30u);

    rem(s, 20u);
    REQUIRE(!s.contains(20u));
    REQUIRE(s.size() == 2);
    REQUIRE(s.contains(10u));
    REQUIRE(s.contains(30u));
}

TEST_CASE (



"[SparseSet] Remove last element"
,
"[SparseSet]"
)
 {
    U32Set s(16);
    ins(s, 5u);
    rem(s, 5u);
    REQUIRE(s.empty());
}

TEST_CASE (



"[SparseSet] Remove absent key returns KeyNotFound"
,
"[SparseSet]"
)
 {
    U32Set s(16);
    auto r = s.remove(7u);
    REQUIRE(!r.has_value());
    REQUIRE(r.error() == SSError::KeyNotFound);
}

TEST_CASE (



"[SparseSet] Dense packing preserved after removal"
,
"[SparseSet]"
)
 {
    U32Set s(16);
    for (std::uint32_t i = 0; i < 10; ++i) ins(s, i);
    rem(s, 4u);

    REQUIRE(s.size() == 9);
    for (std::uint32_t i = 0; i < 10; ++i) {
        if (i == 4)
            REQUIRE(!s.contains(i));
        else
            REQUIRE(s.contains(i));
    }
    REQUIRE(s.dense_entries().size() == 9);
}

// ---------------------------------------------------------------------------
// 4. Clear & reset
// ---------------------------------------------------------------------------
TEST_CASE (



"[SparseSet] clear empties the set"
,
"[SparseSet]"
)
 {
    U32Set s(32);
    for (std::uint32_t i = 0; i < 10; ++i) ins(s, i);
    s.clear();
    REQUIRE(s.empty());
    REQUIRE(s.capacity() == 32);
    REQUIRE(s.insert(5u).has_value());
}

TEST_CASE (



"[SparseSet] reset clears and zeroes sparse array"
,
"[SparseSet]"
)
 {
    U32Set s(32);
    for (std::uint32_t i = 0; i < 5; ++i) ins(s, i);
    s.reset();
    REQUIRE(s.empty());
    REQUIRE(s.capacity() == 32);
    auto sp = s.sparse_array();
    bool all_invalid = std::ranges::all_of(sp, [](auto v) {
        return v == std::numeric_limits<std::uint32_t>::max();
    });
    REQUIRE(all_invalid);
}

// ---------------------------------------------------------------------------
// 5. Value map (SparseSet with satellite data)
// ---------------------------------------------------------------------------
TEST_CASE (



"[SparseSet] Map: get returns correct value"
,
"[SparseSet]"
)
 {
    U32Map m(64);
    ins(m, 1u, std::string{"alpha"});
    ins(m, 2u, std::string{"beta"});

    auto v1 = m.get(1u);
    REQUIRE(v1.has_value());
    REQUIRE(v1->get() == "alpha");

    auto v2 = m.get(2u);
    REQUIRE(v2.has_value());
    REQUIRE(v2->get() == "beta");
}

TEST_CASE (



"[SparseSet] Map: get on absent key returns KeyNotFound"
,
"[SparseSet]"
)
 {
    U32Map m(16);
    auto r = m.get(5u);
    REQUIRE(!r.has_value());
    REQUIRE(r.error() == SSError::KeyNotFound);
}

TEST_CASE (



"[SparseSet] Map: value survives remove-and-swap"
,
"[SparseSet]"
)
 {
    U32Map m(32);
    ins(m, 0u, std::string{"zero"});
    ins(m, 1u, std::string{"one"});
    ins(m, 2u, std::string{"two"});

    rem(m, 0u);
    REQUIRE(!m.contains(0u));
    REQUIRE(m.get(1u)->get() == "one");
    REQUIRE(m.get(2u)->get() == "two");
}

// ---------------------------------------------------------------------------
// 6. dense_index_of
// ---------------------------------------------------------------------------
TEST_CASE (



"[SparseSet] dense_index_of returns valid index"
,
"[SparseSet]"
)
 {
    U32Set s(16);
    ins(s, 3u);
    ins(s, 7u);

    auto di = s.dense_index_of(7u);
    REQUIRE(di.has_value());
    REQUIRE(*di < s.size());
}

TEST_CASE (



"[SparseSet] dense_index_of returns KeyNotFound for absent key"
,
"[SparseSet]"
)
 {
    U32Set s(16);
    auto r = s.dense_index_of(9u);
    REQUIRE(!r.has_value());
    REQUIRE(r.error() == SSError::KeyNotFound);
}

// ---------------------------------------------------------------------------
// 7. Iteration
// ---------------------------------------------------------------------------
TEST_CASE (



"[SparseSet] Range-for iterates all keys"
,
"[SparseSet]"
)
 {
    U32Set s(32);
    std::vector<std::uint32_t> keys{1u, 5u, 9u, 13u};
    for (auto k : keys) ins(s, k);

    std::vector<std::uint32_t> found(s.begin(), s.end());
    std::ranges::sort(found);
    std::ranges::sort(keys);
    REQUIRE(found == keys);
}

TEST_CASE (



"[SparseSet] all_keys() view produces correct sequence"
,
"[SparseSet]"
)
 {
    U32Set s(16);
    for (std::uint32_t i = 0; i < 5; ++i) ins(s, i);

    std::vector<std::uint32_t> v;
    for (auto k : s.all_keys()) v.push_back(k);
    REQUIRE(v.size() == 5);
}

TEST_CASE (



"[SparseSet] all_values() view iterates satellite data"
,
"[SparseSet]"
)
 {
    U32Map m(16);
    ins(m, 0u, std::string{"a"});
    ins(m, 1u, std::string{"b"});
    ins(m, 2u, std::string{"c"});

    std::vector<std::string> vals;
    for (auto& v : m.all_values()) vals.push_back(v);
    REQUIRE(vals.size() == 3);
    std::ranges::sort(vals);
    REQUIRE(vals == std::vector<std::string>{"a", "b", "c"});
}

TEST_CASE (



"[SparseSet] all_pairs() view iterates (key, value) pairs"
,
"[SparseSet]"
)
 {
    U32Map m(32);
    ins(m, 10u, std::string{"ten"});
    ins(m, 20u, std::string{"twenty"});

    bool found_ten = false;
    bool found_twenty = false;
    for (auto [k, v] : m.all_pairs()) {
        if (k == 10u && v == "ten") found_ten = true;
        if (k == 20u && v == "twenty") found_twenty = true;
    }
    REQUIRE(found_ten);
    REQUIRE(found_twenty);
}

// ---------------------------------------------------------------------------
// 8. Bulk operations
// ---------------------------------------------------------------------------
TEST_CASE (



"[SparseSet] insert_range inserts all keys"
,
"[SparseSet]"
)
 {
    U32Set s;
    std::vector<std::uint32_t> keys{2u, 5u, 8u, 11u};
    s.insert_range(keys);
    REQUIRE(s.size() == 4);
    for (auto k : keys)
        REQUIRE(s.contains(k));
}

TEST_CASE (



"[SparseSet] insert_range skips duplicates"
,
"[SparseSet]"
)
 {
    U32Set s(16);
    ins(s, 3u);
    std::vector<std::uint32_t> keys{3u, 7u, 3u};
    s.insert_range(keys);
    REQUIRE(s.size() == 2);
}

TEST_CASE (



"[SparseSet] remove_range removes keys silently"
,
"[SparseSet]"
)
 {
    U32Set s(32);
    for (std::uint32_t i = 0; i < 8; ++i) ins(s, i);
    std::vector<std::uint32_t> to_remove{1u, 3u, 5u, 99u};
    s.remove_range(to_remove);
    REQUIRE(s.size() == 5);
    REQUIRE(!s.contains(1u));
    REQUIRE(!s.contains(3u));
    REQUIRE(!s.contains(5u));
}

TEST_CASE (



"[SparseSet] contains_all / contains_any"
,
"[SparseSet]"
)
 {
    U32Set s(32);
    for (std::uint32_t i = 0; i < 10; ++i) ins(s, i);

    std::vector<std::uint32_t> all_present{0u, 5u, 9u};
    std::vector<std::uint32_t> some_present{0u, 50u};
    std::vector<std::uint32_t> none_present{20u, 30u};

    REQUIRE(s.contains_all(all_present));
    REQUIRE(!s.contains_all(some_present));
    REQUIRE(s.contains_any(some_present));
    REQUIRE(!s.contains_any(none_present));
}

// ---------------------------------------------------------------------------
// 9. Set-theoretic operations
// ---------------------------------------------------------------------------
TEST_CASE (



"[SparseSet] intersection"
,
"[SparseSet]"
)
 {
    U32Set a(32), b(32);
    for (std::uint32_t i = 0; i < 6; ++i) ins(a, i);
    for (std::uint32_t i = 3; i < 9; ++i) ins(b, i);

    auto c = a.intersection(b);
    REQUIRE(c.size() == 3);
    REQUIRE(c.contains(3u));
    REQUIRE(c.contains(4u));
    REQUIRE(c.contains(5u));
    REQUIRE(!c.contains(0u));
    REQUIRE(!c.contains(6u));
}

TEST_CASE (



"[SparseSet] union_with"
,
"[SparseSet]"
)
 {
    U32Set a(16), b(16);
    ins(a, 1u);
    ins(a, 2u);
    ins(b, 2u);
    ins(b, 3u);

    auto c = a.union_with(b);
    REQUIRE(c.size() == 3);
    REQUIRE(c.contains(1u));
    REQUIRE(c.contains(2u));
    REQUIRE(c.contains(3u));
}

TEST_CASE (



"[SparseSet] difference"
,
"[SparseSet]"
)
 {
    U32Set a(16), b(16);
    for (std::uint32_t i = 0; i < 5; ++i) ins(a, i);
    ins(b, 2u);
    ins(b, 4u);

    auto c = a.difference(b);
    REQUIRE(c.size() == 3);
    REQUIRE(c.contains(0u));
    REQUIRE(c.contains(1u));
    REQUIRE(c.contains(3u));
    REQUIRE(!c.contains(2u));
    REQUIRE(!c.contains(4u));
}

// ---------------------------------------------------------------------------
// 10. Equality operator
// ---------------------------------------------------------------------------
TEST_CASE (



"[SparseSet] Equality: same keys regardless of insertion order"
,
"[SparseSet]"
)
 {
    U32Set a(16), b(16);
    ins(a, 1u);
    ins(a, 2u);
    ins(a, 3u);
    ins(b, 3u);
    ins(b, 1u);
    ins(b, 2u);
    REQUIRE(a == b);
}

TEST_CASE (



"[SparseSet] Equality: different key sets are not equal"
,
"[SparseSet]"
)
 {
    U32Set a(16), b(16);
    ins(a, 1u);
    ins(a, 2u);
    ins(b, 1u);
    ins(b, 3u);
    REQUIRE(!(a == b));
}

// ---------------------------------------------------------------------------
// 11. Enum class key
// ---------------------------------------------------------------------------
TEST_CASE (



"[SparseSet] Enum class key works"
,
"[SparseSet]"
)
 {
    SparseSet<EntityId> s(128);
    auto e1 = static_cast<EntityId>(7u);
    auto e2 = static_cast<EntityId>(42u);

    REQUIRE(s.insert(e1).has_value());
    REQUIRE(s.insert(e2).has_value());
    REQUIRE(s.contains(e1));
    REQUIRE(s.contains(e2));
    REQUIRE(s.size() == 2);

    rem(s, e1);
    REQUIRE(!s.contains(e1));
    REQUIRE(s.contains(e2));
}

// ---------------------------------------------------------------------------
// 12. reserve / capacity growth
// ---------------------------------------------------------------------------
TEST_CASE (



"[SparseSet] reserve grows capacity without invalidating elements"
,
"[SparseSet]"
)
 {
    U32Set s(8);
    ins(s, 3u);
    ins(s, 7u);
    s.reserve(128);
    REQUIRE(s.capacity() == 128);
    REQUIRE(s.contains(3u));
    REQUIRE(s.contains(7u));
    REQUIRE(s.size() == 2);
}

// ---------------------------------------------------------------------------
// 13. make_sparse_set helper
// ---------------------------------------------------------------------------
TEST_CASE (



"[SparseSet] make_sparse_set builds from range"
,
"[SparseSet]"
)
 {
    std::vector<std::uint32_t> src{0u, 2u, 4u, 6u};
    auto s = make_sparse_set<std::uint32_t>(16, src);
    REQUIRE(s.size() == 4);
    for (auto k : src)
        REQUIRE(s.contains(k));
}

// ---------------------------------------------------------------------------
// 14. Stress: insert-remove cycle maintains invariants
// ---------------------------------------------------------------------------
TEST_CASE (



"[SparseSet] Stress: insert/remove cycle"
,
"[SparseSet]"
)
 {
    constexpr std::uint32_t N = 1000;
    U32Set s(N);

    for (std::uint32_t i = 0; i < N; ++i)
        REQUIRE(s.insert(i).has_value());
    REQUIRE(s.size() == N);

    for (std::uint32_t i = 0; i < N; i += 2)
        REQUIRE(s.remove(i).has_value());
    REQUIRE(s.size() == N / 2);

    for (std::uint32_t i = 0; i < N; ++i) {
        if (i % 2 == 0)
            REQUIRE(!s.contains(i));
        else
            REQUIRE(s.contains(i));
    }
    REQUIRE(s.dense_entries().size() == N / 2);
}

// ---------------------------------------------------------------------------
// New tests
// ---------------------------------------------------------------------------

// set-theoretic ops are restricted to Value=monostate (pure set) to avoid
// silently dropping satellite data — intersection/union/difference carry no
// values by design; map users must implement their own set ops.
TEST_CASE (



"[SparseSet] intersection restricted to pure-set (documents behavior)"
,
"[SparseSet]"
)
 {
    U32Set a(16), b(16);
    ins(a, 1u);
    ins(a, 2u);
    ins(a, 3u);
    ins(b, 2u);
    ins(b, 3u);
    ins(b, 4u);
    auto c = a.intersection(b);
    REQUIRE(c.size() == 2);
    REQUIRE(c.contains(2u));
    REQUIRE(c.contains(3u));
    REQUIRE(!c.contains(1u));
    REQUIRE(!c.contains(4u));
}

// Documents the dense_.size() >= kInvalid guard that prevents IndexT overflow.
// With IndexT=uint8_t, kInvalid=255; inserting 255 elements fills the dense
// array to the guard boundary; the 256th key (255) resolves to sparse_[255]
// which holds kInvalid=255 and is therefore treated as absent, then the size
// guard fires and returns KeyOutOfRange.
TEST_CASE (



"[SparseSet] IndexT overflow guard fires at capacity boundary"
,
"[SparseSet]"
)
 {
    // Universe of 256 slots, IndexT=uint8_t → kInvalid=255
    SparseSet<std::uint8_t, std::monostate, std::uint8_t> s(256);
    // Fill 254 keys (0..253) — all succeed
    for (std::uint8_t i = 0; i < 254; ++i)
        REQUIRE(s.insert(i).has_value());
    REQUIRE(s.size() == 254);
    // Key 254: dense_.size()==254 < 255=kInvalid, succeeds
    REQUIRE(s.insert(std::uint8_t{254}).has_value());
    REQUIRE(s.size() == 255);
    // Key 255 == kInvalid: sparse_[255] initialized to kInvalid=255,
    // the "not present" sentinel, so the guard dense_.size()>=255 fires first
    auto r = s.insert(std::uint8_t{255});
    REQUIRE(!r.has_value());
    REQUIRE(r.error() == SSError::KeyOutOfRange);
}

TEST_CASE (



"[SparseSet] all_values() mutation through view"
,
"[SparseSet]"
)
 {
    U32Map m(16);
    ins(m, 1u, std::string{"before"});
    for (auto& v : m.all_values()) v = "after";
    REQUIRE(m.get(1u)->get() == "after");
}

TEST_CASE (
"SparseSet: smriti arena allocator integration"
,
"[SparseSet][mem][arena]"
)
 {
    using Resource = smriti::ManagedResource<smriti::domains::SystemRAMDomain,
                                            smriti::pools::BumpPool<smriti::domains::SystemRAMDomain>>;
    Resource res{smriti::domains::SystemRAMDomain{},
                 smriti::pools::BumpPool<smriti::domains::SystemRAMDomain>{64 * 1024}};

    using DenseAlloc = smriti::SmritiAllocator<std::pair<std::uint32_t, std::monostate>, Resource>;
    using SparseAlloc = smriti::SmritiAllocator<std::uint32_t, Resource>;

    DenseAlloc da{res};
    SparseAlloc sa{res};

    SparseSet<std::uint32_t, std::monostate, std::uint32_t, DenseAlloc, SparseAlloc> set(64, da, sa);

    REQUIRE(set.empty());
    REQUIRE(set.capacity() == 64);

    REQUIRE(set.insert(5u).has_value());
    REQUIRE(set.insert(12u).has_value());
    REQUIRE(set.insert(42u).has_value());

    REQUIRE(set.size() == 3);
    REQUIRE(set.contains(5u));
    REQUIRE(set.contains(12u));
    REQUIRE(set.contains(42u));
    REQUIRE_FALSE(set.contains(99u));

    REQUIRE(set.remove(12u).has_value());
    REQUIRE(set.size() == 2);
    REQUIRE_FALSE(set.contains(12u));
}
