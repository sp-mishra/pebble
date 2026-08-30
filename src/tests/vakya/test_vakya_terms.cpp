// =============================================================================
// test_vakya_terms.cpp — term-extension cluster (effect rows / value params / refinements).
//
// Verifies:
//   vakya/types/effect_row.hpp   — effect rows + subsumption + effect_row_solver
//   vakya/types/value_param.hpp  — value params, unify_value, SIMD/tile synthesis
//   vakya/types/refine.hpp       — refinement subtyping + elision bits
//
// Cases:
//   1.  effect_row: closed subsumes closed (subset)
//   2.  effect_row: concrete leak into closed → not_subsumes
//   3.  effect_row: open sup absorbs excess → subsumes
//   4.  effect_row: distinct symbolic tails → deferred
//   5.  effect_row: interning is structural
//   6.  effect_row_solver: concept + solved on subset
//   7.  effect_row_solver: unsatisfiable on leak
//   8.  value_param: literal intern + accessors
//   9.  value_param: unify_value equal / not_equal / deferred
//  10.  value_param: symbolic var is not literal
//  11.  simd_width: divides extent, fits policy
//  12.  simd_width: prime extent → 0 (no lane count divides)
//  13.  simd_width: extent < 2 → 0
//  14.  tile: budget clamped to extent
//  15.  simd_width/tile from arena literal value_param
//  16.  refine: intern + base/predicate accessors
//  17.  refine: syntactic_subtype identical → true; sup ⊤ → true
//  18.  refine: distinct predicates → not decided here (false)
//  19.  refine_subtype_obligation → kRefineSubKind routes to smt in registry
//  20.  elision bits: mark + has
// =============================================================================

#include "catch_amalgamated.hpp"

#include "vakya/types/effect_row.hpp"
#include "vakya/types/value_param.hpp"
#include "vakya/types/refine.hpp"
#include "vakya/types/effect.hpp"
#include "vakya/constraint_registry.hpp"
#include "vakya/analysis_store.hpp"

#include <span>

using namespace vakya::types;

// ============================================================================
// 1-4. effect_row subsumption
// ============================================================================

TEST_CASE("opt effect_row: closed subsumes subset", "[opt][effect_row]") {
    const effect_row_node sub{kEffectMaskIO, kNoEffectTail};
    const effect_row_node sup{kEffectMaskIO | kEffectMaskMemory, kNoEffectTail};
    CHECK(subsumes(sub, sup) == row_subsume_result::subsumes);
}

TEST_CASE("opt effect_row: concrete leak refuted", "[opt][effect_row]") {
    const effect_row_node sub{kEffectMaskNetwork, kNoEffectTail};
    const effect_row_node sup{kEffectMaskIO, kNoEffectTail};
    CHECK(subsumes(sub, sup) == row_subsume_result::not_subsumes);
}

TEST_CASE("opt effect_row: open sup absorbs", "[opt][effect_row]") {
    const effect_row_node sub{kEffectMaskNetwork, kNoEffectTail};
    const effect_row_node sup{0, 7 /*some tail var*/};
    CHECK(subsumes(sub, sup) == row_subsume_result::subsumes);
}

TEST_CASE("opt effect_row: distinct symbolic tails deferred", "[opt][effect_row]") {
    const effect_row_node sub{kEffectMaskIO, 1};
    const effect_row_node sup{kEffectMaskIO, 2};
    CHECK(subsumes(sub, sup) == row_subsume_result::deferred);
}

// ============================================================================
// 5. effect_row interning
// ============================================================================

TEST_CASE("opt effect_row: structural interning", "[opt][effect_row]") {
    effect_row_arena arena;
    const effect_row_ref r1 = arena.intern_effect_row(kEffectMaskIO);
    const effect_row_ref r2 = arena.intern_effect_row(kEffectMaskIO);
    CHECK(r1 == r2);
    const effect_row_var t = arena.fresh_tail();
    const effect_row_ref o = arena.intern_open_row(kEffectMaskIO, t);
    REQUIRE(arena.get(o) != nullptr);
    CHECK(arena.get(o)->is_open());
}

// ============================================================================
// 6-7. effect_row_solver
// ============================================================================

TEST_CASE("opt effect_row: solver solved", "[opt][effect_row]") {
    static_assert(constraint_solver<effect_row_solver>);
    effect_row_arena arena;
    const effect_row_ref sub = arena.intern_effect_row(kEffectMaskIO);
    const effect_row_ref sup = arena.intern_effect_row(kEffectMaskIO | kEffectMaskMemory);

    effect_row_solver solver(arena);
    const constraint c = make_effect_bound_constraint(sub, sup);
    solve_context ctx{nullptr, nullptr};
    const solve_result r = solver.solve(std::span<const constraint>(&c, 1), ctx);
    CHECK(r.status == solve_status::solved);
}

TEST_CASE("opt effect_row: solver unsatisfiable", "[opt][effect_row]") {
    effect_row_arena arena;
    const effect_row_ref sub = arena.intern_effect_row(kEffectMaskNetwork);
    const effect_row_ref sup = arena.intern_effect_row(kEffectMaskIO);

    effect_row_solver solver(arena);
    const constraint c = make_effect_bound_constraint(sub, sup);
    solve_context ctx{nullptr, nullptr};
    const solve_result r = solver.solve(std::span<const constraint>(&c, 1), ctx);
    CHECK(r.status == solve_status::unsatisfiable);
}

// ============================================================================
// 8-10. value_param
// ============================================================================

TEST_CASE("opt value_param: literal intern", "[opt][value_param]") {
    type_arena arena;
    const type_ref v = intern_value_param(arena, 42u);
    CHECK(is_value_param(arena, v));
    CHECK(value_param_is_literal(arena, v));
    CHECK(value_param_literal(arena, v) == 42u);
}

TEST_CASE("opt value_param: unify_value", "[opt][value_param]") {
    type_arena arena;
    const type_ref a = intern_value_param(arena, 4u);
    const type_ref b = intern_value_param(arena, 4u);
    const type_ref c = intern_value_param(arena, 8u);
    const type_ref sym = intern_value_var(arena, 0u);
    CHECK(unify_value(arena, a, b) == value_unify_result::equal);
    CHECK(unify_value(arena, a, c) == value_unify_result::not_equal);
    CHECK(unify_value(arena, a, sym) == value_unify_result::deferred);
}

TEST_CASE("opt value_param: symbolic not literal", "[opt][value_param]") {
    type_arena arena;
    const type_ref sym = intern_value_var(arena, 3u);
    CHECK(is_value_param(arena, sym));
    CHECK_FALSE(value_param_is_literal(arena, sym));
    CHECK(value_param_literal(arena, sym) == 0u);
}

// ============================================================================
// 11-15. SIMD width + tile synthesis
// ============================================================================

TEST_CASE("opt value_param: simd_width divides extent", "[opt][simd]") {
    // Default policy: 128-bit lane / 32-bit elem = 4 lanes. Extent 16 → 4.
    CHECK(synthesize_simd_width(16u) == 4u);
    CHECK(synthesize_simd_width(8u) == 4u);
}

TEST_CASE("opt value_param: simd_width prime extent", "[opt][simd]") {
    CHECK(synthesize_simd_width(7u) == 0u); // no lane count > 1 divides 7
}

TEST_CASE("opt value_param: simd_width tiny extent", "[opt][simd]") {
    CHECK(synthesize_simd_width(1u) == 0u);
    CHECK(synthesize_simd_width(0u) == 0u);
}

TEST_CASE("opt value_param: tile clamps to extent", "[opt][simd]") {
    width_policy pol;
    // budget = 4096 / 4 = 1024; extent 100 clamps to 100.
    CHECK(synthesize_tile(100u, pol) == 100u);
    CHECK(synthesize_tile(4096u, pol) == 1024u);
}

TEST_CASE("opt value_param: synth from arena literal", "[opt][simd]") {
    type_arena arena;
    const type_ref extent = intern_value_param(arena, 16u);
    CHECK(synthesize_simd_width(arena, extent) == 4u);
    const type_ref sym = intern_value_var(arena, 0u);
    CHECK(synthesize_simd_width(arena, sym) == 0u); // symbolic → no synthesis
    CHECK(synthesize_tile(arena, sym) == 0u);
}

// ============================================================================
// 16-18. refinement subtyping
// ============================================================================

TEST_CASE("opt refine: intern + accessors", "[opt][refine]") {
    type_arena arena;
    const type_ref base = arena.intern_primitive<integer_type_tag>();
    const type_ref r = intern_refinement(arena, base, 0xDEAD);
    CHECK(is_refinement(arena, r));
    CHECK(refinement_base(arena, r) == base);
    CHECK(refinement_predicate(arena, r) == 0xDEAD);
}

TEST_CASE("opt refine: syntactic_subtype", "[opt][refine]") {
    type_arena arena;
    const type_ref base = arena.intern_primitive<integer_type_tag>();
    const type_ref p = intern_refinement(arena, base, 0x11);
    const type_ref top = intern_refinement(arena, base, 0); // trivially-true

    CHECK(syntactic_subtype(arena, p, p));   // identical
    CHECK(syntactic_subtype(arena, p, top)); // anything ⇒ ⊤
}

TEST_CASE("opt refine: distinct predicates not decided", "[opt][refine]") {
    type_arena arena;
    const type_ref base = arena.intern_primitive<integer_type_tag>();
    const type_ref p = intern_refinement(arena, base, 0x11);
    const type_ref q = intern_refinement(arena, base, 0x22);
    CHECK_FALSE(syntactic_subtype(arena, p, q)); // needs SMT
}

// ============================================================================
// 19. refine_sub routes to smt in registry
// ============================================================================

TEST_CASE("opt refine: kRefineSubKind routes to smt", "[opt][refine]") {
    auto reg = make_builtin_constraint_registry();
    const constraint_descriptor* d =
        reg.find(static_cast<std::uint32_t>(kRefineSubKind));
    REQUIRE(d != nullptr);
    CHECK(d->target == solver_class::smt);

    type_arena arena;
    const type_ref base = arena.intern_primitive<integer_type_tag>();
    const type_ref p = intern_refinement(arena, base, 0x11);
    const type_ref q = intern_refinement(arena, base, 0x22);
    const constraint c = refine_subtype_obligation(p, q);
    CHECK(c.kind == kRefineSubKind);
    REQUIRE(c.operands.size() == 2);
}

// ============================================================================
// 20. elision bits
// ============================================================================

TEST_CASE("opt refine: elision bits", "[opt][refine]") {
    analysis_record rec;
    CHECK_FALSE(has_elision(rec, kElisionBoundsCheck));
    mark_elision(rec, kElisionBoundsCheck);
    CHECK(has_elision(rec, kElisionBoundsCheck));
    CHECK_FALSE(has_elision(rec, kElisionNullCheck));
}
