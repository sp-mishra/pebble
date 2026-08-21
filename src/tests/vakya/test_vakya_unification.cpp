// =============================================================================
// test_vakya_unification.cpp — mgu, occurs-check, constructor clash,
//                              generalize/instantiate correctness.
//
// Verifies: include/vakya/unification.hpp
//
// Cases:
//   1. unify same type → trivial success, empty delta.
//   2. unify variable with concrete type → binds var.
//   3. unify Function<T,U> with Function<Int,X> → {T→Int, X→Float}.
//   4. occurs-check: unify(T, List<T>) → infinite_type error.
//   5. constructor clash: unify(Integer, Bool) → constructor_clash.
//   6. callable arity mismatch.
//   7. apply(subst, τ) correctly replaces bound vars.
//   8. generalize: unconstrained var becomes ∀T.T.
//   9. instantiate: ∀T.T→T freshens vars at each call site.
//  10. std::expected errors — no exceptions thrown.
// =============================================================================

#include "catch_amalgamated.hpp"

#include "vakya/unification.hpp"

using namespace vakya::types;

// ============================================================================
// Test 1 — trivial success: unify same type
// ============================================================================

TEST_CASE (

"unify: same interned handle → trivial success"
,
"[vakya][unification]"
)
 {
    type_arena arena;
    substitution subst;

    type_ref int_ref = arena.intern_primitive<integer_type_tag>();
    auto r = unify(int_ref, int_ref, subst, arena);
    REQUIRE(r.has_value());
    REQUIRE(r->empty()); // no new bindings
}

// ============================================================================
// Test 2 — unify variable with concrete type
// ============================================================================

TEST_CASE (

"unify: variable binds to concrete type"
,
"[vakya][unification]"
)
 {
    type_arena arena;
    substitution subst;
    type_var_generator gen;

    type_var_id tid = gen.fresh();
    subst.make_var();
    type_ref t_ref = arena.intern_variable(tid);
    type_ref int_ref = arena.intern_primitive<integer_type_tag>();

    auto r = unify(t_ref, int_ref, subst, arena);
    REQUIRE(r.has_value());
    REQUIRE(r->size() == 1);
    REQUIRE((*r)[0].var == tid);

    // After unify, prune(t_ref) should equal int_ref
    type_ref pruned = prune(t_ref, subst, arena);
    REQUIRE(pruned == int_ref);
}

// ============================================================================
// Test 3 — unify Function<T,U> with Function<Int,Float>
// ============================================================================

TEST_CASE (

"unify: callable types unify pairwise"
,
"[vakya][unification]"
)
 {
    type_arena arena;
    substitution subst;
    type_var_generator gen;

    // Build T and U as fresh vars
    type_var_id tid = gen.fresh();  subst.make_var();
    type_var_id uid = gen.fresh();  subst.make_var();
    type_ref t_ref = arena.intern_variable(tid);
    type_ref u_ref = arena.intern_variable(uid);

    type_ref int_ref  = arena.intern_primitive<integer_type_tag>();
    type_ref float_ref = arena.intern_primitive<float_type_tag>();

    // Function<T, U>: callable([T], U)
    type_ref params1[1] = {t_ref};
    type_ref fn1 = arena.intern_callable(std::span<const type_ref>(params1, 1), u_ref);

    // Function<Int, Float>: callable([Int], Float)
    type_ref params2[1] = {int_ref};
    type_ref fn2 = arena.intern_callable(std::span<const type_ref>(params2, 1), float_ref);

    auto r = unify(fn1, fn2, subst, arena);
    REQUIRE(r.has_value());

    // T should be bound to Int
    type_ref pruned_t = prune(t_ref, subst, arena);
    REQUIRE(pruned_t == int_ref);

    // U should be bound to Float
    type_ref pruned_u = prune(u_ref, subst, arena);
    REQUIRE(pruned_u == float_ref);
}

// ============================================================================
// Test 4 — occurs-check: unify(T, List<T>) → infinite_type
// ============================================================================

TEST_CASE (

"unify: occurs-check → infinite_type error"
,
"[vakya][unification]"
)
 {
    type_arena arena;
    substitution subst;
    type_var_generator gen;

    type_var_id tid = gen.fresh();  subst.make_var();
    type_ref t_ref = arena.intern_variable(tid);

    // List<T>
    type_ref children[1] = {t_ref};
    type_ref list_t = arena.intern_constructor<list_type_tag>(
        std::span<const type_ref>(children, 1));

    auto r = unify(t_ref, list_t, subst, arena);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().kind == unify_error_kind::infinite_type);
}

// ============================================================================
// Test 5 — constructor clash: Integer vs Bool
// ============================================================================

TEST_CASE (

"unify: constructor clash → constructor_clash error"
,
"[vakya][unification]"
)
 {
    type_arena arena;
    substitution subst;

    type_ref int_ref  = arena.intern_primitive<integer_type_tag>();
    type_ref bool_ref = arena.intern_primitive<bool_type_tag>();

    auto r = unify(int_ref, bool_ref, subst, arena);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().kind == unify_error_kind::constructor_clash);
}

// ============================================================================
// Test 6 — callable arity mismatch
// ============================================================================

TEST_CASE (

"unify: callable arity mismatch → arity_mismatch error"
,
"[vakya][unification]"
)
 {
    type_arena arena;
    substitution subst;

    type_ref int_ref = arena.intern_primitive<integer_type_tag>();
    type_ref bool_ref = arena.intern_primitive<bool_type_tag>();

    // (Int -> Bool)  vs  (Int, Bool -> Int)  — different child counts
    type_ref p1[1] = {int_ref};
    type_ref fn1 = arena.intern_callable(std::span<const type_ref>(p1, 1), bool_ref);

    type_ref p2[2] = {int_ref, bool_ref};
    type_ref fn2 = arena.intern_callable(std::span<const type_ref>(p2, 2), int_ref);

    auto r = unify(fn1, fn2, subst, arena);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().kind == unify_error_kind::arity_mismatch);
}

// ============================================================================
// Test 7 — apply replaces bound vars
// ============================================================================

TEST_CASE (

"apply: replaces bound vars in a type"
,
"[vakya][unification]"
)
 {
    type_arena arena;
    substitution subst;
    type_var_generator gen;

    type_var_id tid = gen.fresh();  subst.make_var();
    type_ref t_ref = arena.intern_variable(tid);
    type_ref int_ref = arena.intern_primitive<integer_type_tag>();

    subst.bind(tid, int_ref);

    // Build Vector<T>
    type_ref children[1] = {t_ref};
    type_ref vec_t = arena.intern_constructor<vector_type_tag>(
        std::span<const type_ref>(children, 1));

    type_ref applied = apply(vec_t, subst, arena);
    const type_node* n = arena.get(applied);
    REQUIRE(n != nullptr);
    REQUIRE(n->children.size() == 1);
    REQUIRE(n->children[0] == int_ref);
}

// ============================================================================
// Test 8 — generalize: unconstrained var becomes ∀T.T
// ============================================================================

TEST_CASE (

"generalize: unconstrained variable becomes polymorphic scheme"
,
"[vakya][unification]"
)
 {
    type_arena arena;
    substitution subst;
    type_var_generator gen;

    type_var_id tid = gen.fresh();  subst.make_var();
    type_ref t_ref = arena.intern_variable(tid);

    std::unordered_set<type_var_id> env_free; // empty env: no free vars in scope
    type_ref scheme = generalize(t_ref, env_free, subst, arena);

    const type_node* n = arena.get(scheme);
    REQUIRE(n != nullptr);
    REQUIRE(n->kind == type_kind::quantified);
    REQUIRE(n->quantified_vars.size() == 1);
}

// ============================================================================
// Test 9 — instantiate: ∀T.T→T freshens vars at each use site
// ============================================================================

TEST_CASE (

"instantiate: quantified type gets fresh vars per instantiation"
,
"[vakya][unification]"
)
 {
    type_arena arena;
    substitution subst;
    type_var_generator gen;

    // Build ∀T. T→T
    type_var_id tid = gen.fresh();  subst.make_var();
    type_ref t_ref = arena.intern_variable(tid);
    type_ref p[1] = {t_ref};
    type_ref id_fn = arena.intern_callable(std::span<const type_ref>(p, 1), t_ref);

    type_var_id qv[1] = {tid};
    type_ref scheme = arena.intern_quantified(std::span<const type_var_id>(qv, 1), id_fn);

    // First instantiation
    type_ref inst1 = instantiate(scheme, subst, arena, gen);
    // Second instantiation — should produce different var handles
    type_ref inst2 = instantiate(scheme, subst, arena, gen);

    // The two instances are distinct nodes (different fresh vars)
    REQUIRE(inst1 != inst2);
}

// ============================================================================
// Test 10 — std::expected — no exceptions on error paths
// ============================================================================

TEST_CASE (

"unify: std::expected errors — no exceptions thrown"
,
"[vakya][unification]"
)
 {
    type_arena arena;
    substitution subst;

    // Force a constructor clash: should return std::unexpected, not throw
    type_ref int_ref  = arena.intern_primitive<integer_type_tag>();
    type_ref void_ref = arena.intern_primitive<void_type_tag>();

    std::optional<unify_error> caught_err;
    auto r = unify(int_ref, void_ref, subst, arena);
    if (!r) caught_err = r.error();

    REQUIRE(caught_err.has_value());
    REQUIRE(caught_err->kind == unify_error_kind::constructor_clash);
}
