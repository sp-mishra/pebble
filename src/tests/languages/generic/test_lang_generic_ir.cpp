// test_lang_generic_ir.cpp — Unit tests for lang::ir_node, ir_module, ir_interner, lower_events.
//
// C++23. Verifies zero-cost ext, module CRUD, structural-hash dedup, and lowering round-trip.

#include "catch_amalgamated.hpp"
#include "languages/generic/ir/node.hpp"
#include "languages/generic/ir/ir_module.hpp"
#include "languages/generic/ir/interning.hpp"
#include "languages/generic/ir/lowering.hpp"
#include "languages/generic/tree/event_log.hpp"
#include <cstdint>
#include <variant>  // monostate

// ---- Minimal test kind enum -----------------------------------------------

enum class TestKind : std::uint8_t { root = 0, child = 1, leaf = 2 };

// ---- ir_node zero-cost ext ------------------------------------------------

static_assert(sizeof(lang::ir_node<TestKind, std::monostate>) ==
              sizeof(lang::ir_node<TestKind>),
              "monostate ExtPayload must not add bytes");

// ---- ir_module: push / child iteration / root / reset --------------------

TEST_CASE (
"ir_module: basic push/children/root/reset"
,
"[lang][ir]"
)
 {
    lang::ir_module<TestKind> mod;

    // Push a root node
    lang::ir_node<TestKind> root_nd{};
    root_nd.kind = TestKind::root;
    root_nd.structural_hash = 0xdeadbeef;
    const auto root_id = mod.push(root_nd);
    mod.set_root(root_id);

    // Push two child nodes
    lang::ir_node<TestKind> c1{}; c1.kind = TestKind::child;
    lang::ir_node<TestKind> c2{}; c2.kind = TestKind::child;
    const auto c1_id = mod.push(c1);
    const auto c2_id = mod.push(c2);

    // Wire children
    const lang::ir_node_id kids[] = {c1_id, c2_id};
    mod.append_children(root_id, kids);

    REQUIRE(mod.root() == root_id);
    REQUIRE(mod.size() == 3);

    // Iterate children of root
    auto ch = mod.children(root_id);
    REQUIRE(ch.size() == 2);
    REQUIRE(ch[0] == c1_id);
    REQUIRE(ch[1] == c2_id);

    // reset keeps capacity (no crash)
    mod.reset();
    REQUIRE(mod.empty());
    REQUIRE(mod.root() == lang::k_null_ir);
}

// ---- ir_interner: structural-hash dedup -----------------------------------

TEST_CASE (
"ir_interner: structural-hash dedup"
,
"[lang][ir]"
)
 {
    lang::ir_interner interner;

    // Two structurally identical hashes → same node id returned
    const std::uint64_t h = 0xABCDEF0123456789ULL;
    const lang::ir_node_id first_id  = interner.dedup(h, 10);
    const lang::ir_node_id second_id = interner.dedup(h, 20); // different proposed id
    REQUIRE(first_id == 10);
    REQUIRE(second_id == 10);  // dedup: first registration wins

    // Different hash → different id
    const lang::ir_node_id third_id = interner.dedup(h ^ 1, 30);
    REQUIRE(third_id == 30);
}

// ---- ir_interner: name interning ------------------------------------------

TEST_CASE (
"ir_interner: name interning dedup"
,
"[lang][ir]"
)
 {
    lang::ir_interner interner;
    const auto id1 = interner.intern_name("foo");
    const auto id2 = interner.intern_name("foo");
    const auto id3 = interner.intern_name("bar");
    REQUIRE(id1 != lang::k_null_symbol);
    REQUIRE(id1 == id2);    // same string → same id
    REQUIRE(id1 != id3);    // different string → different id
}

// ---- lower_events round-trip -----------------------------------------------
// Build a hand-constructed event_log and verify lower_events produces matching
// structure and hashes with green_arena::build.

TEST_CASE (
"lower_events: round-trip matches green_arena"
,
"[lang][ir]"
)
 {
    using KE = TestKind;
    using Log = lang::event_log<KE, std::uint16_t>;

    Log log;
    const auto m = log.begin(KE::root);
    log.token(0);
    log.token(1);
    log.end(m, lang::byte_span{0, 10});

    // Leaf callbacks: token 0 → span{0,5}, token 1 → span{5,5}
    auto span_fn = [](std::uint32_t idx) -> lang::byte_span {
        return idx == 0 ? lang::byte_span{0, 5} : lang::byte_span{5, 5};
    };
    auto hash_fn = [](std::uint32_t idx) -> std::uint64_t {
        return idx == 0 ? 0x111ULL : 0x222ULL;
    };

    // Build via green_arena (reference)
    auto arena = lang::green_arena<KE>::build(log, span_fn, hash_fn);

    // Build via lower_events — same leaf callbacks wrapped in a struct
    struct LeafFns {
        lang::byte_span span(std::uint32_t idx) const noexcept {
            return idx == 0 ? lang::byte_span{0, 5} : lang::byte_span{5, 5};
        }
        std::uint64_t hash(std::uint32_t idx) const noexcept {
            return idx == 0 ? 0x111ULL : 0x222ULL;
        }
    } leaf_fns;

    auto mod = lang::lower_events<KE>(log, leaf_fns);

    REQUIRE(!mod.empty());
    REQUIRE(mod.root() == arena.root());

    // Root node structural hashes must match
    REQUIRE(mod[mod.root()].structural_hash ==
            arena[arena.root()].structural_hash);
}

// ---- as_egraph_view / as_adjacency ----------------------------------------

TEST_CASE (
"ir_module: graph views"
,
"[lang][ir]"
)
 {
    lang::ir_module<TestKind> mod;
    lang::ir_node<TestKind> r{}; r.kind = TestKind::root;
    lang::ir_node<TestKind> c{}; c.kind = TestKind::leaf;
    const auto rid = mod.push(r);
    const auto cid = mod.push(c);
    mod.set_root(rid);
    const lang::ir_node_id kids[] = {cid};
    mod.append_children(rid, kids);

    auto view = mod.as_egraph_view();
    REQUIRE(view.node_count() == 2);
    auto adj = view.adj(rid);
    REQUIRE(adj.size() == 1);
    REQUIRE(adj[0] == cid);

    auto adj_view = mod.as_adjacency();
    REQUIRE(adj_view.adj(rid).size() == 1);
}
