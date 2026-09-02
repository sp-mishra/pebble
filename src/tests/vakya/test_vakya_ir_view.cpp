// =============================================================================
// test_vakya_ir_view.cpp — vakya type IR / generic IR alignment (Stage 9).
//
// Verifies: include/vakya/types.hpp (as_ir_module_view, as_egraph_view, kosha_dedup_adapter)
//           include/languages/generic/ir/ir_module.hpp (handle_store<Tag> policy)
//
// Cases:
//   1. static_assert: type_ref == generational_handle<type_tag, uint32_t>.
//      Documents the HandleStore key alignment between vakya and the generic IR.
//   2. as_ir_module_view() parity: same node count and child adjacency as native
//      type_arena accessors.
//   3. as_egraph_view() round-trip: both views return the same adjacency for a
//      small constructor type tree.
//   4. kosha_dedup_adapter intern parity: interning the same type twice via
//      kosha yields one canonical ref through the generic Dedup adapter.
// =============================================================================

#include "catch_amalgamated.hpp"

#include "vakya/types.hpp"
#include "languages/generic/ir/ir_module.hpp"
#include "containers/handle/generational_handle.hpp"

using namespace vakya::types;

// ============================================================================
// Test 1 — static_assert: type_ref == generational_handle<type_tag, uint32_t>
// ============================================================================

static_assert(
    std::is_same_v<
        type_ref,
        containers::generational_handle<type_tag, std::uint32_t>
    >,
    "type_ref must equal generational_handle<type_tag,uint32_t> "
    "— vakya uses the exact handle type that handle_store<type_tag> names"
);

// Confirm that ir_module<..., handle_store<type_tag>>::node_handle == type_ref.
using vakya_ir_mod = lang::ir_module<
    type_ir_kind,
    type_ref,
    lang::handle_store<type_tag>
>;
static_assert(
    std::is_same_v<vakya_ir_mod::node_handle, type_ref>,
    "handle_store<type_tag> node_handle must be type_ref"
);

TEST_CASE (
"type_ref == generational_handle<type_tag,uint32_t>"
,
"[vakya][ir_view][static]"
)
 {
    // Runtime complement to the static_asserts above.
    type_ref null_ref;
    REQUIRE(null_ref.is_null());
    REQUIRE(null_ref.index == 0);
    REQUIRE(null_ref.generation == 0);
}

// ============================================================================
// Test 2 — as_ir_module_view() parity: node count + child adjacency
// ============================================================================

TEST_CASE (
"as_ir_module_view: node count and child adjacency match native"
,
"[vakya][ir_view]"
)
 {
    type_arena arena;
    type_var_generator gen;

    // Build: List<Integer>
    type_ref int_ref  = arena.intern_primitive<integer_type_tag>();
    type_ref list_int;
    {
        type_ref children[1] = {int_ref};
        list_int = arena.intern_constructor<list_type_tag>(
            std::span<const type_ref>(children, 1));
    }

    // Two distinct type_nodes interned → 2 nodes in the store.
    auto view = arena.as_ir_module_view();
    REQUIRE(view.size() == 2u);
    REQUIRE_FALSE(view.empty());

    // Native: get(int_ref) has no children; get(list_int) has 1 child.
    const type_node* int_node  = arena.get(int_ref);
    const type_node* list_node = arena.get(list_int);
    REQUIRE(int_node  != nullptr);
    REQUIRE(list_node != nullptr);
    REQUIRE(int_node->children.empty());
    REQUIRE(list_node->children.size() == 1u);
    REQUIRE(list_node->children[0] == int_ref);

    // Via view: same adjacency.
    auto adj_int  = view.adj(int_ref);
    auto adj_list = view.adj(list_int);
    REQUIRE(adj_int.empty());
    REQUIRE(adj_list.size() == 1u);
    REQUIRE(adj_list[0] == int_ref);

    // find() parity.
    const type_node* vf = view.find(int_ref);
    REQUIRE(vf != nullptr);
    REQUIRE(vf->kind == type_kind::primitive);
}

// ============================================================================
// Test 3 — as_egraph_view() round-trip: adjacency consistent across both views
// ============================================================================

TEST_CASE (
"as_egraph_view: adjacency consistent with as_ir_module_view"
,
"[vakya][ir_view]"
)
 {
    type_arena arena;

    // Build: Optional<Bool>
    type_ref bool_ref = arena.intern_primitive<bool_type_tag>();
    type_ref opt_bool;
    {
        type_ref children[1] = {bool_ref};
        opt_bool = arena.intern_constructor<optional_type_tag>(
            std::span<const type_ref>(children, 1));
    }

    auto mod_view   = arena.as_ir_module_view();
    auto egraph_view = arena.as_egraph_view();   // same backing, same type

    // Both views report the same node count.
    REQUIRE(mod_view.size() == egraph_view.size());

    // Both views return the same child list for opt_bool.
    auto adj_mod    = mod_view.adj(opt_bool);
    auto adj_egraph = egraph_view.adj(opt_bool);

    REQUIRE(adj_mod.size()    == adj_egraph.size());
    REQUIRE(adj_mod.size()    == 1u);
    REQUIRE(adj_mod[0]        == bool_ref);
    REQUIRE(adj_egraph[0]     == bool_ref);

    // For the leaf bool_ref both views return empty adjacency.
    REQUIRE(mod_view.adj(bool_ref).empty());
    REQUIRE(egraph_view.adj(bool_ref).empty());
}

// ============================================================================
// Test 4 — kosha_dedup_adapter intern parity
// ============================================================================

TEST_CASE (
"kosha_dedup_adapter: interning same type yields same ref via Dedup seam"
,
"[vakya][ir_view][dedup]"
)
{
    type_arena arena;

    type_ref int_ref_a = arena.intern_primitive<integer_type_tag>();
    type_ref int_ref_b = arena.intern_primitive<integer_type_tag>();

    // Native path already deduplicates.
    REQUIRE(int_ref_a == int_ref_b);

    // Demonstrate kosha_dedup_adapter satisfies the Dedup contract:
    // building a fresh adapter over the same cache, inserting int_ref's hash,
    // then deduping an equivalent hash returns the stored ref.
    type_intern_cache_t cache{256};
    kosha_dedup_adapter adapter{&cache};

    const type_node* n = arena.get(int_ref_a);
    REQUIRE(n != nullptr);
    const std::uint64_t h = type_hash(*n);

    // First call: hash not yet in adapter cache — inserts and returns ref.
    type_ref via_adapter_1 = adapter.dedup(h, int_ref_a);
    REQUIRE(via_adapter_1 == int_ref_a);

    // Second call: hash is now cached — returns stored ref (not the argument).
    type_ref int_ref_dummy{};   // fabricate a different (null) handle as argument
    type_ref via_adapter_2 = adapter.dedup(h, int_ref_dummy);
    REQUIRE(via_adapter_2 == int_ref_a);   // returns the first-seen ref

    // Explicit insert then dedup.
    type_ref bool_ref = arena.intern_primitive<bool_type_tag>();
    const type_node* bn = arena.get(bool_ref);
    REQUIRE(bn != nullptr);
    const std::uint64_t bh = type_hash(*bn);

    adapter.insert(bh, bool_ref);
    type_ref via_adapter_3 = adapter.dedup(bh, type_ref{});
    REQUIRE(via_adapter_3 == bool_ref);
}
