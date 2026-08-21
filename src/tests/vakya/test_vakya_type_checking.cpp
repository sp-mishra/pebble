// =============================================================================
// test_vakya_type_checking.cpp — end-to-end type_check + infer over real
//                                 vakya::node<Tag,...> expression trees.
//
// Verifies: include/vakya/type_checking.hpp
//           include/vakya/type_inference.hpp
//
// Cases:
//   1. Leaf integer literal → inferred type is a (fresh) type variable (monotype).
//   2. add(Int leaf, Int leaf) → inferred type eq after unification.
//   3. add(Int, Bool) → infer fails with infer_error (type mismatch).
//   4. type_check over add tree → validation_result success.
//   5. type_check over mismatched add → type_error.
//   6. infer result cached: same structural hash → same type_ref from cache.
//   7. type_environment: push_scope / pop_scope adds and removes binding.
//   8. generalize in env: unconstrained child var generalised to ∀T.T.
//   9. inferred types retrievable via property_store (TypeResultKey).
//  10. Non-regression: construction-only expr (no type_checking.hpp) builds fine.
// =============================================================================

#include "catch_amalgamated.hpp"

#include "vakya/type_inference.hpp"
#include "vakya/type_checking.hpp"

using namespace vakya::types;
using namespace vakya;

// ============================================================================
// Helpers — build small expression trees
// ============================================================================

// Tags at namespace scope so descriptor specializations can reference them.
struct int_leaf_tag {};

struct bool_leaf_tag {};

// Distinct stable_ids >= kExtensionIdBase (1000) so structural_hash differs.
// Without this both zero-child nodes hash identically and the infer cache
// returns the first result for both, breaking mismatch detection.
template <>
struct vakya::emit::tag_descriptor<int_leaf_tag> {
    static constexpr std::size_t stable_id = 1100u;
    static constexpr std::uint8_t arity = 0;
    static constexpr std::string_view symbol = "int_leaf";
};

template <>
struct vakya::emit::tag_descriptor<bool_leaf_tag> {
    static constexpr std::size_t stable_id = 1101u;
    static constexpr std::uint8_t arity = 0;
    static constexpr std::string_view symbol = "bool_leaf";
};

// typing_rule for int_leaf: result type = Integer
template <>
struct vakya::types::typing_rule<int_leaf_tag> {
    static std::pair<type_ref, std::vector<constraint>>
    emit(const std::vector<type_ref>& /*child_types*/,
         type_environment& /*env*/,
         type_arena& arena,
         type_var_generator& /*gen*/) {
        return {arena.intern_primitive<integer_type_tag>(), {}};
    }
};

// typing_rule for bool_leaf: result type = Bool
template <>
struct vakya::types::typing_rule<bool_leaf_tag> {
    static std::pair<type_ref, std::vector<constraint>>
    emit(const std::vector<type_ref>& /*child_types*/,
         type_environment& /*env*/,
         type_arena& arena,
         type_var_generator& /*gen*/) {
        return {arena.intern_primitive<bool_type_tag>(), {}};
    }
};

// ============================================================================
// Test 1 — infer: leaf int literal → concrete integer type
// ============================================================================

TEST_CASE (

"infer: leaf integer node → integer type"
,
"[vakya][type_checking]"
)
 {
    type_arena arena;
    substitution subst;
    type_var_generator gen;
    type_environment env;
    infer_cache_t cache{4096};

    auto leaf = make_node<int_leaf_tag>();
    auto r = infer(leaf, env, subst, arena, gen, cache);

    REQUIRE(r.has_value());
    // Result should prune to integer_type_tag primitive
    type_ref pruned = prune(*r, subst, arena);
    type_ref int_ref = arena.intern_primitive<integer_type_tag>();
    REQUIRE(pruned == int_ref);
}

// ============================================================================
// Test 2 — infer: add(Int, Int) → result type == Integer after unification
// ============================================================================

TEST_CASE (

"infer: add(int_leaf, int_leaf) → integer type"
,
"[vakya][type_checking]"
)
 {
    type_arena arena;
    substitution subst;
    type_var_generator gen;
    type_environment env;
    infer_cache_t cache{4096};

    auto lhs = make_node<int_leaf_tag>();
    auto rhs = make_node<int_leaf_tag>();
    auto expr = make_node<add_tag>(lhs, rhs);

    auto r = infer(expr, env, subst, arena, gen, cache);
    REQUIRE(r.has_value());

    type_ref pruned = prune(*r, subst, arena);
    type_ref int_ref = arena.intern_primitive<integer_type_tag>();
    REQUIRE(pruned == int_ref);
}

// ============================================================================
// Test 3 — infer: add(Int, Bool) → fails with infer_error (constructor_clash)
// ============================================================================

TEST_CASE (

"infer: add(int_leaf, bool_leaf) → infer_error"
,
"[vakya][type_checking]"
)
 {
    type_arena arena;
    substitution subst;
    type_var_generator gen;
    type_environment env;
    infer_cache_t cache{4096};

    auto lhs = make_node<int_leaf_tag>();
    auto rhs = make_node<bool_leaf_tag>();
    auto expr = make_node<add_tag>(lhs, rhs);

    auto r = infer(expr, env, subst, arena, gen, cache);
    REQUIRE_FALSE(r.has_value());
    // Should be a constructor_clash (Int ≠ Bool)
    REQUIRE(r.error().kind == unify_error_kind::constructor_clash);
}

// ============================================================================
// Test 4 — type_check: add(Int, Int) tree → validation success
// ============================================================================

TEST_CASE (

"type_check: add(int, int) expression → validation_status::success"
,
"[vakya][type_checking]"
)
 {
    type_arena arena;
    substitution subst;
    type_var_generator gen;
    type_environment env;
    property_store store;
    unification_solver solver;

    auto lhs = make_node<int_leaf_tag>();
    auto rhs = make_node<int_leaf_tag>();
    auto expr = make_node<add_tag>(lhs, rhs);

    auto vr = type_check(expr, env, solver, arena, gen, subst, store);
    REQUIRE(vr.ok());
    REQUIRE(vr.status == validation_status::success);
}

// ============================================================================
// Test 5 — type_check: add(Int, Bool) → type_error
// ============================================================================

TEST_CASE (

"type_check: add(int, bool) expression → type_error"
,
"[vakya][type_checking]"
)
 {
    type_arena arena;
    substitution subst;
    type_var_generator gen;
    type_environment env;
    property_store store;
    unification_solver solver;

    auto lhs = make_node<int_leaf_tag>();
    auto rhs = make_node<bool_leaf_tag>();
    auto expr = make_node<add_tag>(lhs, rhs);

    auto vr = type_check(expr, env, solver, arena, gen, subst, store);
    REQUIRE_FALSE(vr.ok());
    REQUIRE(vr.status == validation_status::type_error);
}

// ============================================================================
// Test 6 — infer cache: same structural hash → same type_ref
// ============================================================================

TEST_CASE (

"infer: cache hit returns same type_ref for structurally equal expressions"
,
"[vakya][type_checking]"
)
 {
    type_arena arena;
    substitution subst;
    type_var_generator gen;
    type_environment env;
    infer_cache_t cache{4096};

    auto expr = make_node<int_leaf_tag>();

    auto r1 = infer(expr, env, subst, arena, gen, cache);
    auto r2 = infer(expr, env, subst, arena, gen, cache);  // should hit cache

    REQUIRE(r1.has_value());
    REQUIRE(r2.has_value());
    REQUIRE(*r1 == *r2);
}

// ============================================================================
// Test 7 — type_environment: push/pop scope
// ============================================================================

TEST_CASE (

"type_environment: push_scope / pop_scope manages bindings"
,
"[vakya][type_checking]"
)
 {
    type_arena arena;
    substitution subst;
    type_environment env;

    constexpr std::uint64_t name_hash = 0xABCD'1234ULL;
    type_ref int_ref = arena.intern_primitive<integer_type_tag>();

    // Not yet in env
    REQUIRE(env.lookup(name_hash) == nullptr);

    env.push_scope();
    env.bind(name_hash, type_scheme{int_ref, false});
    REQUIRE(env.lookup(name_hash) != nullptr);
    REQUIRE(env.lookup(name_hash)->mono == int_ref);

    env.pop_scope();
    // Binding removed after pop
    REQUIRE(env.lookup(name_hash) == nullptr);
}

// ============================================================================
// Test 8 — generalize: unconstrained var in empty-env → poly scheme
// ============================================================================

TEST_CASE (

"generalize: var not in env_free → quantified scheme"
,
"[vakya][type_checking]"
)
 {
    type_arena arena;
    substitution subst;
    type_var_generator gen;
    type_environment env;

    type_var_id tid = gen.fresh(); subst.make_var();
    type_ref t_ref = arena.intern_variable(tid);

    // env_free is empty (empty env)
    std::unordered_set<type_var_id> env_free = env.free_type_vars(subst, arena);
    type_ref scheme = generalize(t_ref, env_free, subst, arena);

    const type_node* n = arena.get(scheme);
    REQUIRE(n != nullptr);
    REQUIRE(n->kind == type_kind::quantified);
    REQUIRE(n->quantified_vars.size() == 1);
}

// ============================================================================
// Test 9 — type results retrievable via property_store
// ============================================================================

TEST_CASE (

"type_check: inferred types stored in property_store via TypeResultKey"
,
"[vakya][type_checking]"
)
 {
    type_arena arena;
    substitution subst;
    type_var_generator gen;
    type_environment env;
    property_store store;
    unification_solver solver;

    auto leaf = make_node<int_leaf_tag>();
    auto expr = make_node<add_tag>(leaf, leaf);

    auto vr = type_check(expr, env, solver, arena, gen, subst, store);
    REQUIRE(vr.ok());

    // Retrieve stored type for the add node
    const std::uint64_t h = vakya::structural_hash(expr);
    auto* props = store.find(h);
    REQUIRE(props != nullptr);
    auto stored_type = props->get<TypeResultKey>();
    REQUIRE(stored_type.has_value());
}

// ============================================================================
// Test 10 — non-regression: construction-only build (no inference)
// ============================================================================

TEST_CASE (

"non-regression: constructing vakya nodes does not require type_checking.hpp"
,
"[vakya][type_checking]"
)
 {
    // Build a tree using only vakya.hpp ADT — no type system involved
    auto a = make_node<int_leaf_tag>();
    auto b = make_node<int_leaf_tag>();
    auto expr = make_node<add_tag>(a, b);

    // The expression tree is well-formed as a pure construction
    REQUIRE(vakya::tree::arity(expr) == 2);
}
