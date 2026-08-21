#define CATCH_CONFIG_MAIN
#include <catch_amalgamated.hpp>
#include "containers/graph/DisjointSet.hpp"

#include <string>
#include <vector>
#include <algorithm>
#include <ranges>

using namespace disjointset;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
using IntDS = DisjointSet<int>;
using StrDS = DisjointSet<std::string>;

// A small lattice type for testing LatticeJoinMeta.
struct TypeLattice {
    int level = 0; // 0 = bottom, higher = more specific

    TypeLattice merge(const TypeLattice& other) const {
        return TypeLattice{std::max(level, other.level)};
    }

    bool operator==(const TypeLattice&) const = default;
};

using TypeDS = DisjointSet<std::string, std::monostate, TypeLattice, LatticeJoinMeta>;

// ---------------------------------------------------------------------------
// 1. Basic insert / find / contains
// ---------------------------------------------------------------------------
TEST_CASE (



"[DisjointSet] Insert and find single element"
,
"[DisjointSet]"
)
 {
    IntDS ds;
    auto id = ds.insert(42);
    REQUIRE(id.has_value());
    REQUIRE(id->is_valid());

    REQUIRE(ds.contains(42));
    REQUIRE(ds.element_count() == 1);
    REQUIRE(ds.set_count() == 1);

    auto root = ds.find(42);
    REQUIRE(root.has_value());
    REQUIRE(root->value == id->value);
}

TEST_CASE (



"[DisjointSet] Duplicate insert returns error"
,
"[DisjointSet]"
)
 {
    IntDS ds;
    auto r1 = ds.insert(7);
    auto r2 = ds.insert(7);
    REQUIRE(r1.has_value());
    REQUIRE(!r2.has_value());
    REQUIRE(r2.error() == DSError::ElementAlreadyExists);
    REQUIRE(ds.element_count() == 1);
}

TEST_CASE (



"[DisjointSet] insert_or_get is idempotent"
,
"[DisjointSet]"
)
 {
    IntDS ds;
    const auto id1 = ds.insert_or_get(10);
    const auto id2 = ds.insert_or_get(10);
    REQUIRE(id1 == id2);
    REQUIRE(ds.element_count() == 1);
}

TEST_CASE (



"[DisjointSet] find missing element returns error"
,
"[DisjointSet]"
)
 {
    IntDS ds;
    (void)ds.insert(1);
    auto r = ds.find(99);
    REQUIRE(!r.has_value());
    REQUIRE(r.error() == DSError::ElementNotFound);
}

// ---------------------------------------------------------------------------
// 2. Union and connectivity
// ---------------------------------------------------------------------------
TEST_CASE (



"[DisjointSet] Union two elements"
,
"[DisjointSet]"
)
 {
    IntDS ds;
    (void)ds.insert(1);
    (void)ds.insert(2);
    REQUIRE_FALSE(ds.connected(1, 2));

    auto root = ds.unite(1, 2);
    REQUIRE(root.has_value());
    REQUIRE(ds.connected(1, 2));
    REQUIRE(ds.set_count() == 1);
    REQUIRE(ds.element_count() == 2);
}

TEST_CASE (



"[DisjointSet] Union is reflexive"
,
"[DisjointSet]"
)
 {
    IntDS ds;
    (void)ds.insert(5);
    REQUIRE(ds.connected(5, 5));
    auto r = ds.unite(5, 5);
    REQUIRE(r.has_value());
    REQUIRE(ds.set_count() == 1);
}

TEST_CASE (



"[DisjointSet] Union is transitive"
,
"[DisjointSet]"
)
 {
    IntDS ds;
    for (int i = 1; i <= 5; ++i) (void)ds.insert(i);

    (void)ds.unite(1, 2);
    (void)ds.unite(3, 4);
    (void)ds.unite(2, 3);

    REQUIRE(ds.connected(1, 4));
    REQUIRE_FALSE(ds.connected(1, 5));
    REQUIRE(ds.set_count() == 2);
}

TEST_CASE (



"[DisjointSet] Uniting elements with missing key returns error"
,
"[DisjointSet]"
)
 {
    IntDS ds;
    (void)ds.insert(1);
    auto r = ds.unite(1, 99);
    REQUIRE(!r.has_value());
    REQUIRE(r.error() == DSError::ElementNotFound);
}

// ---------------------------------------------------------------------------
// 3. Set size and weight tracking
// ---------------------------------------------------------------------------
TEST_CASE (



"[DisjointSet] Set size grows on union"
,
"[DisjointSet]"
)
 {
    IntDS ds;
    for (int i = 1; i <= 4; ++i) (void)ds.insert(i);

    auto sz1 = ds.set_size(1);
    REQUIRE(sz1.value_or(0) == 1);

    (void)ds.unite(1, 2);
    REQUIRE(ds.set_size(1).value_or(0) == 2);
    REQUIRE(ds.set_size(2).value_or(0) == 2);

    (void)ds.unite(3, 4);
    (void)ds.unite(1, 3);
    REQUIRE(ds.set_size(1).value_or(0) == 4);
}

TEST_CASE (



"[DisjointSet] Weighted union accumulates weight"
,
"[DisjointSet]"
)
 {
    DisjointSet<int> ds;
    (void)ds.insert(1, {}, {}, 3.0);
    (void)ds.insert(2, {}, {}, 5.0);
    (void)ds.unite(1, 2);
    REQUIRE(ds.set_weight(1).value_or(0.0) == Catch::Approx(8.0));
}

// ---------------------------------------------------------------------------
// 4. Path compression — representative stability
// ---------------------------------------------------------------------------
TEST_CASE (



"[DisjointSet] Representative is consistent after deep chain"
,
"[DisjointSet]"
)
 {
    IntDS ds;
    for (int i = 0; i < 16; ++i) (void)ds.insert(i);
    // Build a chain: 0-1, 1-2, …, 14-15
    for (int i = 0; i < 15; ++i) (void)ds.unite(i, i + 1);

    REQUIRE(ds.set_count() == 1);
    auto rep0 = ds.representative(0);
    auto rep15 = ds.representative(15);
    REQUIRE(rep0.has_value());
    REQUIRE(rep15.has_value());
    REQUIRE(*rep0 == *rep15);
}

// ---------------------------------------------------------------------------
// 5. Element metadata
// ---------------------------------------------------------------------------
TEST_CASE (



"[DisjointSet] Element metadata is stored and retrievable"
,
"[DisjointSet]"
)
 {
    DisjointSet<std::string, int> ds;
    ds.insert("x", 42);
    ds.insert("y", 7);

    auto m = ds.elem_meta("x");
    REQUIRE(m.has_value());
    REQUIRE(m->get() == 42);

    m->get() = 100;
    REQUIRE(ds.elem_meta("x")->get() == 100);
}

TEST_CASE (



"[DisjointSet] Element metadata of missing key returns error"
,
"[DisjointSet]"
)
 {
    DisjointSet<std::string, int> ds;
    auto m = ds.elem_meta("missing");
    REQUIRE(!m.has_value());
    REQUIRE(m.error() == DSError::ElementNotFound);
}

// ---------------------------------------------------------------------------
// 6. Set metadata with LatticeJoinMeta merge strategy
// ---------------------------------------------------------------------------
TEST_CASE (



"[DisjointSet] LatticeJoinMeta picks max level on union"
,
"[DisjointSet]"
)
 {
    TypeDS ds;
    ds.insert("Int",   {}, TypeLattice{3});
    ds.insert("Float", {}, TypeLattice{5});
    ds.insert("Num",   {}, TypeLattice{1});

    ds.unite("Int", "Float");
    auto sm = ds.set_meta("Int");
    REQUIRE(sm.has_value());
    REQUIRE(sm->get().level == 5); // max(3,5)

    ds.unite("Int", "Num");
    REQUIRE(ds.set_meta("Num")->get().level == 5); // max(5,1)
}

TEST_CASE (



"[DisjointSet] KeepRootMeta keeps root meta unchanged"
,
"[DisjointSet]"
)
 {
    using DS = DisjointSet<int, std::monostate, int, KeepRootMeta>;
    DS ds;
    ds.insert(1, {}, 10);
    ds.insert(2, {}, 20);
    // union by rank: both rank 0 -> first arg (1) wins root, rank bumped
    ds.unite(1, 2);
    // root has meta 10 (the set_meta of the node that became root)
    auto m = ds.set_meta(1);
    REQUIRE(m.has_value());
    REQUIRE((m->get() == 10)); // union-by-rank: equal rank -> first arg (1) becomes root
}

// ---------------------------------------------------------------------------
// 7. Range views
// ---------------------------------------------------------------------------
TEST_CASE (



"[DisjointSet] all_elements yields every inserted element"
,
"[DisjointSet]"
)
 {
    IntDS ds;
    for (int i = 0; i < 5; ++i) ds.insert(i);

    std::vector<int> elems;
    for (const int &e : ds.all_elements()) elems.push_back(e);
    std::ranges::sort(elems);
    REQUIRE(elems == std::vector<int>{0, 1, 2, 3, 4});
}

TEST_CASE (



"[DisjointSet] all_sets yields one representative per class"
,
"[DisjointSet]"
)
 {
    IntDS ds;
    for (int i = 0; i < 6; ++i) ds.insert(i);
    ds.unite(0, 1);
    ds.unite(2, 3);
    // 3 classes: {0,1}, {2,3}, {4}, {5} → 4 sets

    std::size_t count = 0;
    for ([[maybe_unused]] const int &r : ds.all_sets()) ++count;
    REQUIRE(count == 4);
}

TEST_CASE (



"[DisjointSet] elements_of returns correct class members"
,
"[DisjointSet]"
)
 {
    IntDS ds;
    for (int i = 0; i < 5; ++i) ds.insert(i);
    ds.unite(1, 3);
    ds.unite(3, 0);

    std::vector<int> members;
    for (const int &e : ds.elements_of(1)) members.push_back(e);
    std::ranges::sort(members);
    REQUIRE(members == std::vector<int>{0, 1, 3});
}

TEST_CASE (



"[DisjointSet] root_ids count matches set_count"
,
"[DisjointSet]"
)
 {
    IntDS ds;
    for (int i = 0; i < 8; ++i) ds.insert(i);
    ds.unite(0, 1); ds.unite(2, 3); ds.unite(4, 5);

    std::size_t rc = 0;
    for ([[maybe_unused]] auto id : ds.root_ids()) ++rc;
    REQUIRE(rc == ds.set_count());
}

// ---------------------------------------------------------------------------
// 8. Snapshot / rollback
// ---------------------------------------------------------------------------
TEST_CASE (



"[DisjointSet] Rollback restores pre-union state"
,
"[DisjointSet]"
)
 {
    IntDS ds;
    for (int i = 1; i <= 4; ++i) ds.insert(i);

    ds.push_snapshot();
    ds.unite(1, 2);
    ds.unite(3, 4);
    REQUIRE(ds.set_count() == 2);

    auto r = ds.rollback();
    REQUIRE(r.has_value());
    REQUIRE(ds.set_count() == 4);
    REQUIRE_FALSE(ds.connected(1, 2));
    REQUIRE_FALSE(ds.connected(3, 4));
}

TEST_CASE (



"[DisjointSet] Commit merges snapshot into parent"
,
"[DisjointSet]"
)
 {
    IntDS ds;
    for (int i = 1; i <= 3; ++i) ds.insert(i);

    ds.push_snapshot(); // outer
    ds.unite(1, 2);

    ds.push_snapshot(); // inner
    ds.unite(2, 3);
    ds.commit();        // inner → outer

    REQUIRE(ds.connected(1, 3));
    REQUIRE(ds.snapshot_depth() == 1);

    ds.rollback();      // undo outer (includes inner's union)
    REQUIRE_FALSE(ds.connected(1, 2));
}

TEST_CASE (



"[DisjointSet] Rollback on empty stack returns error"
,
"[DisjointSet]"
)
 {
    IntDS ds;
    auto r = ds.rollback();
    REQUIRE(!r.has_value());
    REQUIRE(r.error() == DSError::SnapshotStackEmpty);
}

TEST_CASE (



"[DisjointSet] Nested snapshots are independent"
,
"[DisjointSet]"
)
 {
    IntDS ds;
    for (int i = 0; i < 6; ++i) ds.insert(i);

    ds.push_snapshot();
    ds.unite(0, 1);

    ds.push_snapshot();
    ds.unite(2, 3);

    ds.rollback(); // undo unite(2,3)
    REQUIRE(ds.connected(0, 1));
    REQUIRE_FALSE(ds.connected(2, 3));

    ds.rollback(); // undo unite(0,1)
    REQUIRE_FALSE(ds.connected(0, 1));
}

TEST_CASE (



"[DisjointSet] Set sizes are correctly restored after rollback"
,
"[DisjointSet]"
)
 {
    IntDS ds;
    for (int i = 0; i < 4; ++i) ds.insert(i);

    ds.push_snapshot();
    ds.unite(0, 1);
    ds.unite(0, 2);
    ds.unite(0, 3);
    REQUIRE(ds.set_size(0).value_or(0) == 4);

    ds.rollback();
    REQUIRE(ds.set_size(0).value_or(0) == 1);
    REQUIRE(ds.set_size(1).value_or(0) == 1);
}

// ---------------------------------------------------------------------------
// 9. Callbacks
// ---------------------------------------------------------------------------
TEST_CASE (



"[DisjointSet] Callback is invoked on union"
,
"[DisjointSet]"
)
 {
    IntDS ds;
    ds.insert(10); ds.insert(20);

    std::vector<int> roots_seen;
    ds.add_callback([&](const IntDS::event_type &ev) {
        roots_seen.push_back(ev.new_root);
    });

    ds.unite(10, 20);
    REQUIRE(roots_seen.size() == 1);
}

TEST_CASE (



"[DisjointSet] Callback not invoked when already connected"
,
"[DisjointSet]"
)
 {
    IntDS ds;
    ds.insert(1); ds.insert(2);
    ds.unite(1, 2);

    int invocations = 0;
    ds.add_callback([&](const auto &) { ++invocations; });
    ds.unite(1, 2); // already connected
    REQUIRE(invocations == 0);
}

TEST_CASE (



"[DisjointSet] Callback can be removed"
,
"[DisjointSet]"
)
 {
    IntDS ds;
    ds.insert(1); ds.insert(2); ds.insert(3);

    int count = 0;
    const std::size_t cb_id = ds.add_callback([&](const auto &) { ++count; });

    ds.unite(1, 2);
    REQUIRE(count == 1);

    ds.remove_callback(cb_id);
    ds.unite(2, 3);
    REQUIRE(count == 1); // not incremented
}

TEST_CASE (



"[DisjointSet] Multiple callbacks all fire"
,
"[DisjointSet]"
)
 {
    IntDS ds;
    ds.insert(5); ds.insert(6);

    int a = 0, b = 0;
    ds.add_callback([&](const auto &) { ++a; });
    ds.add_callback([&](const auto &) { ++b; });

    ds.unite(5, 6);
    REQUIRE(a == 1);
    REQUIRE(b == 1);
}

// ---------------------------------------------------------------------------
// 10. Bulk operations
// ---------------------------------------------------------------------------
TEST_CASE (



"[DisjointSet] unite_all merges an entire range"
,
"[DisjointSet]"
)
 {
    IntDS ds;
    std::vector<int> elems{10, 20, 30, 40, 50};
    for (int e : elems) ds.insert(e);

    auto root = ds.unite_all(elems);
    REQUIRE(root.has_value());
    REQUIRE(ds.set_count() == 1);
    REQUIRE(ds.set_size(10).value_or(0) == 5);
}

TEST_CASE (



"[DisjointSet] insert_all inserts without connecting"
,
"[DisjointSet]"
)
 {
    IntDS ds;
    std::vector<int> elems{1, 2, 3, 4, 5};
    ds.insert_all(elems);
    REQUIRE(ds.element_count() == 5);
    REQUIRE(ds.set_count() == 5);
}

TEST_CASE (



"[DisjointSet] make_disjoint_set free function"
,
"[DisjointSet]"
)
 {
    std::vector<int> elems{7, 8, 9};
    auto ds = make_disjoint_set(elems);
    REQUIRE(ds.element_count() == 3);
    REQUIRE(ds.set_count() == 3);
}

// ---------------------------------------------------------------------------
// 11. Partition / members
// ---------------------------------------------------------------------------
TEST_CASE (



"[DisjointSet] partition returns correct class map"
,
"[DisjointSet]"
)
 {
    IntDS ds;
    for (int i = 0; i < 6; ++i) ds.insert(i);
    ds.unite(0, 1);
    ds.unite(2, 3);
    ds.unite(4, 5);

    auto p = ds.partition();
    REQUIRE(p.size() == 3);
    for (auto &[rep, members] : p) {
        std::ranges::sort(members);
        REQUIRE(members.size() == 2);
    }
}

TEST_CASE (



"[DisjointSet] members returns all elements of a class"
,
"[DisjointSet]"
)
 {
    IntDS ds;
    for (int i = 0; i < 5; ++i) ds.insert(i);
    ds.unite(0, 2);
    ds.unite(2, 4);

    auto m = ds.members(0);
    REQUIRE(m.has_value());
    std::ranges::sort(*m);
    REQUIRE(*m == std::vector<int>{0, 2, 4});
}

TEST_CASE (



"[DisjointSet] members of unknown element returns error"
,
"[DisjointSet]"
)
 {
    IntDS ds;
    auto m = ds.members(999);
    REQUIRE(!m.has_value());
    REQUIRE(m.error() == DSError::ElementNotFound);
}

// ---------------------------------------------------------------------------
// 12. same_partition free function
// ---------------------------------------------------------------------------
TEST_CASE (



"[DisjointSet] same_partition detects equivalent partitions"
,
"[DisjointSet]"
)
 {
    IntDS ds1, ds2;
    for (int i = 0; i < 4; ++i) { ds1.insert(i); ds2.insert(i); }
    ds1.unite(0, 1); ds1.unite(2, 3);
    ds2.unite(1, 0); ds2.unite(3, 2); // same sets, different union order

    REQUIRE(same_partition(ds1, ds2));
}

TEST_CASE (



"[DisjointSet] same_partition detects different partitions"
,
"[DisjointSet]"
)
 {
    IntDS ds1, ds2;
    for (int i = 0; i < 4; ++i) { ds1.insert(i); ds2.insert(i); }
    ds1.unite(0, 1);
    ds2.unite(0, 2);

    REQUIRE_FALSE(same_partition(ds1, ds2));
}

// ---------------------------------------------------------------------------
// 13. String element type
// ---------------------------------------------------------------------------
TEST_CASE (



"[DisjointSet] Works with std::string elements"
,
"[DisjointSet]"
)
 {
    StrDS ds;
    ds.insert("alpha"); ds.insert("beta"); ds.insert("gamma");
    ds.unite("alpha", "gamma");

    REQUIRE(ds.connected("alpha", "gamma"));
    REQUIRE_FALSE(ds.connected("alpha", "beta"));
    REQUIRE(ds.set_count() == 2);

    auto rep = ds.representative("gamma");
    REQUIRE(rep.has_value());
    REQUIRE(ds.connected(*rep, "alpha"));
}

// ---------------------------------------------------------------------------
// 14. Clear and reserve
// ---------------------------------------------------------------------------
TEST_CASE (



"[DisjointSet] clear resets all state"
,
"[DisjointSet]"
)
 {
    IntDS ds;
    for (int i = 0; i < 10; ++i) ds.insert(i);
    ds.unite(0, 1);
    ds.clear();
    REQUIRE(ds.element_count() == 0);
    REQUIRE(ds.set_count() == 0);
    REQUIRE_FALSE(ds.contains(0));
}

TEST_CASE (



"[DisjointSet] reserve does not add elements"
,
"[DisjointSet]"
)
 {
    IntDS ds;
    ds.reserve(1024);
    REQUIRE(ds.element_count() == 0);
    ds.insert(1);
    REQUIRE(ds.element_count() == 1);
}

// ---------------------------------------------------------------------------
// 15. find_by_id and valid_id
// ---------------------------------------------------------------------------
TEST_CASE (



"[DisjointSet] find_by_id returns root for valid id"
,
"[DisjointSet]"
)
 {
    IntDS ds;
    auto id = ds.insert(42);
    REQUIRE(id.has_value());
    REQUIRE(ds.valid_id(*id));

    auto root = ds.find_by_id(*id);
    REQUIRE(root.has_value());
    REQUIRE(root->value == id->value);
}

TEST_CASE (



"[DisjointSet] find_by_id on INVALID_ELEMENT_ID returns error"
,
"[DisjointSet]"
)
 {
    IntDS ds;
    auto r = ds.find_by_id(INVALID_ELEMENT_ID);
    REQUIRE(!r.has_value());
    REQUIRE(r.error() == DSError::InvalidElement);
}

// ---------------------------------------------------------------------------
// 16. Type-system scenario — unification of type variables
// ---------------------------------------------------------------------------
TEST_CASE (



"[DisjointSet] Type variable unification scenario"
,
"[DisjointSet][TypeSystem]"
)
 {
    // Represent type variables as strings, set metadata as the resolved type.
    using TyDS = DisjointSet<std::string, std::monostate, std::string, KeepRootMeta>;
    TyDS ds;

    // Type variables
    ds.insert("?a", {}, "unknown");
    ds.insert("?b", {}, "unknown");
    ds.insert("?c", {}, "unknown");
    ds.insert("Int", {}, "Int");

    // Unify ?a = Int
    ds.unite("?a", "Int");
    REQUIRE(ds.connected("?a", "Int"));

    // Unify ?b = ?a  →  ?b = Int transitively
    ds.unite("?b", "?a");
    REQUIRE(ds.connected("?b", "Int"));

    // ?c is still its own class
    REQUIRE_FALSE(ds.connected("?c", "Int"));
    REQUIRE(ds.set_count() == 2);
}

TEST_CASE (



"[DisjointSet] Backtracking type inference with snapshot"
,
"[DisjointSet][TypeSystem]"
)
 {
    using TyDS = DisjointSet<std::string>;
    TyDS ds;
    ds.insert("?x"); ds.insert("?y"); ds.insert("Bool"); ds.insert("Int");

    // Try unifying ?x = Bool in a speculative branch.
    ds.push_snapshot();
    ds.unite("?x", "Bool");
    REQUIRE(ds.connected("?x", "Bool"));

    // Inference fails — roll back.
    ds.rollback();
    REQUIRE_FALSE(ds.connected("?x", "Bool"));

    // Now try ?x = Int.
    ds.push_snapshot();
    ds.unite("?x", "Int");
    REQUIRE(ds.connected("?x", "Int"));
    ds.commit(); // accept

    REQUIRE(ds.connected("?x", "Int"));
    REQUIRE(ds.snapshot_depth() == 0);
}
