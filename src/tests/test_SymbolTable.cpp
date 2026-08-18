#define CATCH_CONFIG_MAIN
#include <catch_amalgamated.hpp>

#include "containers/symbol/InternPool.hpp"
#include "containers/symbol/SymbolTable.hpp"

#include <atomic>
#include <thread>
#include <vector>

using namespace symtab;

// compile-time: SymbolTable must not be move-constructible (item 4)
static_assert(!std::is_move_constructible_v<SymbolTable<>>);

// ============================================================================
// Helpers
// ============================================================================

static int dummy_a = 1;
static int dummy_b = 2;
static int dummy_c = 3;

// ============================================================================
// InternPool
// ============================================================================

TEST_CASE (



"[InternPool] intern same string twice returns same pointer"
,
"[SymbolTable]"
)
 {
    InternPool pool;
    auto r1 = pool.intern("hello");
    auto r2 = pool.intern("hello");
    REQUIRE(r1.has_value());
    REQUIRE(r2.has_value());
    REQUIRE(r1->data() == r2->data());
}

TEST_CASE (



"[InternPool] intern different strings returns different pointers"
,
"[SymbolTable]"
)
 {
    InternPool pool;
    auto r1 = pool.intern("foo");
    auto r2 = pool.intern("bar");
    REQUIRE(r1.has_value());
    REQUIRE(r2.has_value());
    REQUIRE(r1->data() != r2->data());
}

TEST_CASE (



"[InternPool] intern empty string returns error"
,
"[SymbolTable]"
)
 {
    InternPool pool;
    auto r = pool.intern("");
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error() == InternError::EmptyString);
}

TEST_CASE (



"[InternPool] contains reflects interned strings"
,
"[SymbolTable]"
)
 {
    InternPool pool;
    REQUIRE_FALSE(pool.contains("x"));
    (void)pool.intern("x");
    REQUIRE(pool.contains("x"));
    REQUIRE_FALSE(pool.contains("y"));
}

TEST_CASE (



"[InternPool] size tracks unique strings"
,
"[SymbolTable]"
)
 {
    InternPool pool;
    REQUIRE(pool.size() == 0);
    (void)pool.intern("a");
    (void)pool.intern("b");
    (void)pool.intern("a"); // duplicate
    REQUIRE(pool.size() == 2);
}

TEST_CASE (



"[InternPool] clear wipes all entries"
,
"[SymbolTable]"
)
 {
    InternPool pool;
    (void)pool.intern("hello");
    pool.clear();
    REQUIRE(pool.size() == 0);
    REQUIRE_FALSE(pool.contains("hello"));
}

TEST_CASE (



"[InternPool] all() returns snapshot of interned strings"
,
"[SymbolTable]"
)
 {
    InternPool pool;
    (void)pool.intern("one");
    (void)pool.intern("two");
    (void)pool.intern("three");
    auto all = pool.all();
    REQUIRE(all.size() == 3);
}

TEST_CASE (



"[InternPool] thread-safety: concurrent interns produce stable pointers"
,
"[SymbolTable]"
)
 {
    InternPool pool;
    constexpr int N_THREADS  = 8;
    constexpr int N_PER_THREAD = 50;
    // Each thread interns N_PER_THREAD strings, half overlapping with others.
    std::vector<std::thread> threads;
    std::atomic<int> errors{0};

    for (int t = 0; t < N_THREADS; ++t) {
        threads.emplace_back([&pool, &errors, t]() {
            for (int i = 0; i < N_PER_THREAD; ++i) {
                // Even threads intern "shared_N", odd threads also intern "shared_N"
                // (overlap) plus their own "private_T_N".
                std::string shared = "shared_" + std::to_string(i % 10);
                auto r = pool.intern(shared);
                if (!r.has_value()) { ++errors; return; }
                auto r2 = pool.intern(shared);
                if (!r2.has_value()) { ++errors; return; }
                // Pointers must be identical for the same string.
                if (r->data() != r2->data()) ++errors;

                if (t % 2 == 1) {
                    std::string priv = "private_" + std::to_string(t) + "_" + std::to_string(i);
                    (void)pool.intern(priv);
                }
            }
        });
    }
    for (auto& th : threads) th.join();
    REQUIRE(errors.load() == 0);
}

// item 2: intern_call_count() correctness
TEST_CASE (



"[InternPool] intern_call_count counts every call including hits"
,
"[InternPool]"
)
 {
    InternPool pool;
    constexpr int N = 5;
    for (int i = 0; i < N; ++i)
        (void)pool.intern("same");
    REQUIRE(pool.intern_call_count() == static_cast<std::size_t>(N));
}

// item 3: move semantics
TEST_CASE (



"[InternPool] move ctor transfers strings; old pool becomes empty"
,
"[InternPool]"
)
 {
    InternPool src;
    (void)src.intern("alpha");
    (void)src.intern("beta");
    auto* ptr_alpha = src.intern("alpha")->data();

    InternPool dst(std::move(src));

    REQUIRE(dst.size() == 2);
    REQUIRE(dst.contains("alpha"));
    REQUIRE(dst.contains("beta"));
    // Pointer stability: same underlying pointer after move.
    REQUIRE(dst.intern("alpha")->data() == ptr_alpha);

    REQUIRE(src.size() == 0);
}

// ============================================================================
// SymbolTable — unit
// ============================================================================

TEST_CASE (



"[SymbolTable] register_symbol and resolve round-trip"
,
"[SymbolTable]"
)
 {
    SymbolTable<> tbl;
    auto r = tbl.register_symbol("my::func", &dummy_a);
    REQUIRE(r.has_value());
    REQUIRE(r->is_valid());
    void* p = tbl.resolve("my::func");
    REQUIRE(p == &dummy_a);
}

TEST_CASE (



"[SymbolTable] resolve unregistered symbol returns nullptr"
,
"[SymbolTable]"
)
 {
    SymbolTable<> tbl;
    REQUIRE(tbl.resolve("not::there") == nullptr);
}

TEST_CASE (



"[SymbolTable] duplicate registration at same version returns error"
,
"[SymbolTable]"
)
 {
    SymbolTable<> tbl;
    (void)tbl.register_symbol("foo", &dummy_a, 1);
    auto r = tbl.register_symbol("foo", &dummy_b, 1);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error() == SymError::AlreadyRegistered);
    // Original address unchanged.
    REQUIRE(tbl.resolve("foo") == &dummy_a);
}

TEST_CASE (



"[SymbolTable] downgrade (lower version) is rejected"
,
"[SymbolTable]"
)
 {
    SymbolTable<> tbl;
    (void)tbl.register_symbol("foo", &dummy_a, 2);
    auto r = tbl.register_symbol("foo", &dummy_b, 1);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error() == SymError::AlreadyRegistered);
    REQUIRE(tbl.resolve("foo") == &dummy_a);
}

TEST_CASE (



"[SymbolTable] upgrade (higher version) succeeds and replaces address"
,
"[SymbolTable]"
)
 {
    SymbolTable<> tbl;
    (void)tbl.register_symbol("foo", &dummy_a, 1);
    auto r = tbl.register_symbol("foo", &dummy_b, 2);
    REQUIRE(r.has_value());
    REQUIRE(tbl.resolve("foo") == &dummy_b);
}

TEST_CASE (



"[SymbolTable] resolve_versioned exact version match"
,
"[SymbolTable]"
)
 {
    SymbolTable<> tbl;
    (void)tbl.register_symbol("bar", &dummy_a, 3);
    REQUIRE(tbl.resolve_versioned("bar", 3) == &dummy_a);
    REQUIRE(tbl.resolve_versioned("bar", 2) == nullptr);
    REQUIRE(tbl.resolve_versioned("bar", 4) == nullptr);
}

TEST_CASE (



"[SymbolTable] unregister removes symbol"
,
"[SymbolTable]"
)
 {
    SymbolTable<> tbl;
    (void)tbl.register_symbol("baz", &dummy_a);
    REQUIRE(tbl.resolve("baz") == &dummy_a);
    REQUIRE(tbl.unregister("baz"));
    REQUIRE(tbl.resolve("baz") == nullptr);
    // Second unregister returns false.
    REQUIRE_FALSE(tbl.unregister("baz"));
}

TEST_CASE (



"[SymbolTable] contains reflects registration state"
,
"[SymbolTable]"
)
 {
    SymbolTable<> tbl;
    REQUIRE_FALSE(tbl.contains("x"));
    (void)tbl.register_symbol("x", &dummy_a);
    REQUIRE(tbl.contains("x"));
    tbl.unregister("x");
    REQUIRE_FALSE(tbl.contains("x"));
}

TEST_CASE (



"[SymbolTable] empty name returns InvalidName error"
,
"[SymbolTable]"
)
 {
    SymbolTable<> tbl;
    auto r = tbl.register_symbol("", &dummy_a);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error() == SymError::InvalidName);
}

TEST_CASE (



"[SymbolTable] size tracks registered symbol count"
,
"[SymbolTable]"
)
 {
    SymbolTable<> tbl;
    REQUIRE(tbl.size() == 0);
    (void)tbl.register_symbol("a", &dummy_a);
    (void)tbl.register_symbol("b", &dummy_b);
    REQUIRE(tbl.size() == 2);
    tbl.unregister("a");
    REQUIRE(tbl.size() == 1);
}

TEST_CASE (



"[SymbolTable] snapshot and rollback"
,
"[SymbolTable]"
)
 {
    SymbolTable<> tbl;
    (void)tbl.register_symbol("a", &dummy_a);
    (void)tbl.register_symbol("b", &dummy_b);
    auto snap = tbl.snapshot(); // == 2
    REQUIRE(snap == 2);

    (void)tbl.register_symbol("c", &dummy_c);
    REQUIRE(tbl.size() == 3);
    REQUIRE(tbl.resolve("c") == &dummy_c);

    auto rb = tbl.rollback(snap);
    REQUIRE(rb.has_value());
    REQUIRE(tbl.size() == 2);
    REQUIRE(tbl.resolve("c") == nullptr);
    // Symbols before snapshot still present.
    REQUIRE(tbl.resolve("a") == &dummy_a);
    REQUIRE(tbl.resolve("b") == &dummy_b);
}

TEST_CASE (



"[SymbolTable] rollback to invalid size returns error"
,
"[SymbolTable]"
)
 {
    SymbolTable<> tbl;
    (void)tbl.register_symbol("a", &dummy_a);
    auto r = tbl.rollback(999);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error() == SymError::SnapshotUnderflow);
}

// ============================================================================
// SymbolTable — bulk + integration
// ============================================================================

TEST_CASE (



"[SymbolTable] register_range bulk insert"
,
"[SymbolTable]"
)
 {
    SymbolTable<> tbl;
    std::vector<symbol_entry> entries = {
        {"alpha", &dummy_a, 0},
        {"beta",  &dummy_b, 0},
        {"gamma", &dummy_c, 0},
    };
    std::size_t registered = tbl.register_range(entries);
    REQUIRE(registered == 3);
    REQUIRE(tbl.resolve("alpha") == &dummy_a);
    REQUIRE(tbl.resolve("beta")  == &dummy_b);
    REQUIRE(tbl.resolve("gamma") == &dummy_c);
}

// item 5: rollback does not roll back pool
TEST_CASE (



"[SymbolTable] rollback does not roll back intern pool"
,
"[SymbolTable]"
)
 {
    SymbolTable<> tbl;
    (void)tbl.register_symbol("persistent", &dummy_a);
    auto snap = tbl.snapshot();
    (void)tbl.register_symbol("temp_symbol", &dummy_b);
    REQUIRE(tbl.size() == 2);

    auto rb = tbl.rollback(snap);
    REQUIRE(rb.has_value());
    REQUIRE(tbl.size() == 1);
    // Symbol is gone from table.
    REQUIRE(tbl.resolve("temp_symbol") == nullptr);
    // But re-registering "temp_symbol" must succeed (pool still has the string,
    // which is fine — table only checks map_, not pool_).
    auto r = tbl.register_symbol("temp_symbol", &dummy_c);
    REQUIRE(r.has_value());
    REQUIRE(tbl.resolve("temp_symbol") == &dummy_c);
}

// item 6: register_range with pre-registered symbol in range
TEST_CASE (



"[SymbolTable] register_range skips pre-registered symbols"
,
"[SymbolTable]"
)
 {
    SymbolTable<> tbl;
    (void)tbl.register_symbol("alpha", &dummy_a);

    std::vector<symbol_entry> entries = {
        {"alpha",   &dummy_b, 0}, // already registered — should be skipped
        {"new_sym", &dummy_c, 0},
    };
    std::size_t count = tbl.register_range(entries);
    REQUIRE(count == 1);
    // alpha unchanged
    REQUIRE(tbl.resolve("alpha") == &dummy_a);
    // new_sym registered
    REQUIRE(tbl.resolve("new_sym") == &dummy_c);
}

TEST_CASE (



"[SymbolTable] register and resolve 1000 symbols"
,
"[SymbolTable]"
)
 {
    SymbolTable<> tbl;
    constexpr int N = 1000;
    std::vector<std::string> names(N);
    std::vector<int> values(N);
    for (int i = 0; i < N; ++i) {
        names[i] = "sym_" + std::to_string(i);
        values[i] = i;
        auto r = tbl.register_symbol(names[i], &values[i]);
        REQUIRE(r.has_value());
    }
    for (int i = 0; i < N; ++i) {
        void* p = tbl.resolve(names[i]);
        REQUIRE(p != nullptr);
        REQUIRE(*static_cast<int*>(p) == i);
    }
}

TEST_CASE (



"[SymbolTable] thread-safety: concurrent readers and one writer"
,
"[SymbolTable]"
)
 {
    SymbolTable<> tbl;
    // Pre-populate.
    (void)tbl.register_symbol("shared::sym", &dummy_a);

    std::atomic<int> errors{0};
    constexpr int N_READERS = 4;
    constexpr int N_READS   = 500;

    // Writer registers additional symbols concurrently.
    std::thread writer([&]() {
        for (int i = 0; i < 100; ++i) {
            std::string name = "writer::sym_" + std::to_string(i);
            (void)tbl.register_symbol(name, &dummy_b);
        }
    });

    std::vector<std::thread> readers;
    for (int t = 0; t < N_READERS; ++t) {
        readers.emplace_back([&]() {
            for (int i = 0; i < N_READS; ++i) {
                void* p = tbl.resolve("shared::sym");
                // Must always find the pre-populated symbol.
                if (p != &dummy_a) ++errors;
            }
        });
    }

    writer.join();
    for (auto& r : readers) r.join();
    REQUIRE(errors.load() == 0);
}

TEST_CASE (



"[SymbolTable] lookup_entry returns copy of entry"
,
"[SymbolTable]"
)
 {
    SymbolTable<> tbl;
    (void)tbl.register_symbol("ns::fn", &dummy_c, 7);
    auto r = tbl.lookup_entry("ns::fn");
    REQUIRE(r.has_value());
    REQUIRE(r->address == &dummy_c);
    REQUIRE(r->version == 7);
    REQUIRE(r->name == "ns::fn");
}

TEST_CASE (



"[SymbolTable] lookup_entry on missing symbol returns error"
,
"[SymbolTable]"
)
 {
    SymbolTable<> tbl;
    auto r = tbl.lookup_entry("nope");
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error() == SymError::NotFound);
}

// ============================================================================
// NamespaceIndex
// ============================================================================

TEST_CASE (



"[NamespaceIndex] enumerate returns symbol under prefix"
,
"[SymbolTable]"
)
 {
    SymbolTable<> tbl;
    (void)tbl.register_symbol("lithe::runtime::linker::resolve", &dummy_a);

    // Build NamespaceIndex from the registered entry.
    NamespaceIndex idx;
    // We need a stable symbol_entry*; use lookup_entry to get the address
    // from within the table. For this test, construct directly.
    symbol_entry entry{"lithe::runtime::linker::resolve", &dummy_a, 0};
    idx.insert(&entry);

    auto results = idx.enumerate("lithe");
    REQUIRE(results.size() == 1);
    REQUIRE(results[0]->address == &dummy_a);
}

TEST_CASE (



"[NamespaceIndex] enumerate with empty prefix returns all symbols"
,
"[SymbolTable]"
)
 {
    NamespaceIndex idx;
    symbol_entry e1{"a::b::fn1", &dummy_a, 0};
    symbol_entry e2{"a::c::fn2", &dummy_b, 0};
    idx.insert(&e1);
    idx.insert(&e2);

    auto results = idx.enumerate("");
    REQUIRE(results.size() == 2);
}

TEST_CASE (



"[NamespaceIndex] enumerate unknown prefix returns empty"
,
"[SymbolTable]"
)
 {
    NamespaceIndex idx;
    symbol_entry e{"foo::bar", &dummy_a, 0};
    idx.insert(&e);

    auto results = idx.enumerate("baz");
    REQUIRE(results.empty());
}

TEST_CASE (



"[NamespaceIndex] depth of namespace prefix"
,
"[SymbolTable]"
)
 {
    NamespaceIndex idx;
    symbol_entry e{"lithe::runtime::linker::foo", &dummy_a, 0};
    idx.insert(&e);

    // "lithe" is 1 level deep, "lithe::runtime" is 2, "lithe::runtime::linker" is 3.
    REQUIRE(idx.depth("lithe") == 1);
    REQUIRE(idx.depth("lithe::runtime") == 2);
    REQUIRE(idx.depth("lithe::runtime::linker") == 3);
    // item 10: depth of root
    REQUIRE(idx.depth("") == 0);
}

TEST_CASE (



"[NamespaceIndex] path between sibling namespaces via LCA"
,
"[SymbolTable]"
)
 {
    NamespaceIndex idx;
    symbol_entry e1{"lithe::runtime::foo",  &dummy_a, 0};
    symbol_entry e2{"lithe::codegen::bar",  &dummy_b, 0};
    idx.insert(&e1);
    idx.insert(&e2);

    auto p = idx.path("lithe::runtime", "lithe::codegen");
    REQUIRE(p.has_value());
    REQUIRE_FALSE(p->empty());
}

TEST_CASE (



"[NamespaceIndex] path with unknown prefix returns nullopt"
,
"[SymbolTable]"
)
 {
    NamespaceIndex idx;
    symbol_entry e{"a::b", &dummy_a, 0};
    idx.insert(&e);

    auto p = idx.path("a", "unknown::ns");
    REQUIRE_FALSE(p.has_value());
}

TEST_CASE (



"[NamespaceIndex] multiple symbols under same namespace"
,
"[SymbolTable]"
)
 {
    NamespaceIndex idx;
    symbol_entry e1{"ns::foo", &dummy_a, 0};
    symbol_entry e2{"ns::bar", &dummy_b, 0};
    symbol_entry e3{"ns::baz", &dummy_c, 0};
    idx.insert(&e1);
    idx.insert(&e2);
    idx.insert(&e3);

    auto results = idx.enumerate("ns");
    REQUIRE(results.size() == 3);
}

TEST_CASE (



"[NamespaceIndex] enumerate sub-namespace excludes siblings"
,
"[SymbolTable]"
)
 {
    NamespaceIndex idx;
    symbol_entry e1{"lithe::runtime::foo", &dummy_a, 0};
    symbol_entry e2{"lithe::codegen::bar", &dummy_b, 0};
    idx.insert(&e1);
    idx.insert(&e2);

    auto under_runtime = idx.enumerate("lithe::runtime");
    REQUIRE(under_runtime.size() == 1);
    REQUIRE(under_runtime[0]->address == &dummy_a);

    auto under_codegen = idx.enumerate("lithe::codegen");
    REQUIRE(under_codegen.size() == 1);
    REQUIRE(under_codegen[0]->address == &dummy_b);
}

// item 9: path(x, x) — same node to itself
TEST_CASE (



"[NamespaceIndex] path from node to itself returns defined result"
,
"[NamespaceIndex]"
)
 {
    NamespaceIndex idx;
    symbol_entry e{"a::b::fn", &dummy_a, 0};
    idx.insert(&e);

    auto p = idx.path("a::b", "a::b");
    // Must return a value (not nullopt) — empty or single-element is acceptable.
    REQUIRE(p.has_value());
}

// item 7: duplicate symbol insert — enumerate must not return duplicates
TEST_CASE (



"[NamespaceIndex] duplicate insert returns exactly one result from enumerate"
,
"[NamespaceIndex]"
)
 {
    NamespaceIndex idx;
    symbol_entry e{"ns::dup::fn", &dummy_a, 0};
    idx.insert(&e);
    idx.insert(&e); // second insert of same entry pointer

    auto results = idx.enumerate("ns::dup");
    REQUIRE(results.size() == 1);
}

// item 8: NamespaceIndex concurrent reads + writes
TEST_CASE (



"[NamespaceIndex] concurrent inserts and enumerates do not crash or race"
,
"[NamespaceIndex]"
)
 {
    NamespaceIndex idx;

    constexpr int N_WRITERS = 4;
    constexpr int N_READERS = 4;
    constexpr int N_OPS     = 50;

    // Pre-allocate entries to avoid lifetime issues.
    std::vector<symbol_entry> entries;
    entries.reserve(N_WRITERS * N_OPS);
    for (int t = 0; t < N_WRITERS; ++t)
        for (int i = 0; i < N_OPS; ++i)
            entries.push_back({"concurrent::ns::sym_" + std::to_string(t * N_OPS + i),
                                &dummy_a, 0});

    std::atomic<int> errors{0};
    std::vector<std::thread> writers, readers;

    for (int t = 0; t < N_WRITERS; ++t) {
        writers.emplace_back([&, t]() {
            for (int i = 0; i < N_OPS; ++i)
                idx.insert(&entries[static_cast<std::size_t>(t * N_OPS + i)]);
        });
    }

    for (int t = 0; t < N_READERS; ++t) {
        readers.emplace_back([&]() {
            for (int i = 0; i < N_OPS; ++i) {
                auto r = idx.enumerate("concurrent::ns");
                (void)r; // just must not crash
            }
        });
    }

    for (auto& w : writers) w.join();
    for (auto& r : readers) r.join();
    REQUIRE(errors.load() == 0);
}
