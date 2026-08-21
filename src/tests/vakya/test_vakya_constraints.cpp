// =============================================================================
// test_vakya_constraints.cpp — constraint_solver concept, composite_solver
//                              routing, per-kind handling, cycle detection,
//                              egraph guard.
//
// Verifies: include/vakya/constraints.hpp
//           include/vakya/constraint_solvers.hpp
//
// Cases:
//   1. STATIC: unification_solver satisfies constraint_solver<S>.
//   2. STATIC: rule_constraint_solver satisfies constraint_solver<S>.
//   3. STATIC: graph_constraint_solver satisfies constraint_solver<S>.
//   4. unification_solver: same_type constraint → solved.
//   5. unification_solver: incompatible types → unsatisfiable + diagnostic.
//   6. unification_solver: handles() returns true for same_type/convertible/subtype.
//   7. unification_solver: handles() returns false for implements/same_rank.
//   8. composite_solver: routes same_type to unification_solver, not rule solver.
//   9. graph_constraint_solver: acyclic graph → solved.
//  10. graph_constraint_solver: circular dependency → unsatisfiable + diagnostic.
//  11. rule_constraint_solver: trait implication closure + query.
//  12. composite_solver<unification, rule, graph>: batch with mixed kinds.
//  13. any_solver: type-erased wrapper wraps unification_solver correctly.
// =============================================================================

#include "catch_amalgamated.hpp"

#include "vakya/constraint_solvers.hpp"

using namespace vakya::types;

// ============================================================================
// Test 1-3 — static concept checks
// ============================================================================

TEST_CASE (

"constraint_solver concept: unification_solver satisfies concept"
,
"[vakya][constraints][static]"
)
 {
    STATIC_REQUIRE(constraint_solver<unification_solver>);
}

TEST_CASE (

"constraint_solver concept: rule_constraint_solver satisfies concept"
,
"[vakya][constraints][static]"
)
 {
    STATIC_REQUIRE(constraint_solver<rule_constraint_solver>);
}

TEST_CASE (

"constraint_solver concept: graph_constraint_solver satisfies concept"
,
"[vakya][constraints][static]"
)
 {
    STATIC_REQUIRE(constraint_solver<graph_constraint_solver>);
}

// ============================================================================
// Test 4 — unification_solver: same_type constraint solved
// ============================================================================

TEST_CASE (

"unification_solver: same_type constraint on equal types → solved"
,
"[vakya][constraints]"
)
 {
    type_arena arena;
    substitution subst;
    unification_solver solver;

    type_ref int_ref = arena.intern_primitive<integer_type_tag>();

    constraint c;
    c.kind = constraint_kind::same_type;
    c.operands.push_back(int_ref);
    c.operands.push_back(int_ref);

    const constraint batch[1] = {c};
    solve_context ctx{&arena, &subst};
    auto r = solver.solve(std::span<const constraint>(batch, 1), ctx);

    REQUIRE(r.status == solve_status::solved);
    REQUIRE(r.diagnostics.empty());
}

// ============================================================================
// Test 5 — unification_solver: incompatible types → unsatisfiable
// ============================================================================

TEST_CASE (

"unification_solver: same_type on incompatible types → unsatisfiable"
,
"[vakya][constraints]"
)
 {
    type_arena arena;
    substitution subst;
    unification_solver solver;

    type_ref int_ref  = arena.intern_primitive<integer_type_tag>();
    type_ref bool_ref = arena.intern_primitive<bool_type_tag>();

    constraint c;
    c.kind = constraint_kind::same_type;
    c.operands.push_back(int_ref);
    c.operands.push_back(bool_ref);

    const constraint batch[1] = {c};
    solve_context ctx{&arena, &subst};
    auto r = solver.solve(std::span<const constraint>(batch, 1), ctx);

    REQUIRE(r.status == solve_status::unsatisfiable);
    REQUIRE_FALSE(r.diagnostics.empty());
}

// ============================================================================
// Test 6 — unification_solver: handles() correct positives
// ============================================================================

TEST_CASE (

"unification_solver: handles same_type / convertible / subtype"
,
"[vakya][constraints]"
)
 {
    unification_solver solver;
    REQUIRE(solver.handles(constraint_kind::same_type));
    REQUIRE(solver.handles(constraint_kind::convertible));
    REQUIRE(solver.handles(constraint_kind::subtype));
}

// ============================================================================
// Test 7 — unification_solver: handles() correct negatives
// ============================================================================

TEST_CASE (

"unification_solver: does not handle implements / same_rank"
,
"[vakya][constraints]"
)
 {
    unification_solver solver;
    REQUIRE_FALSE(solver.handles(constraint_kind::implements));
    REQUIRE_FALSE(solver.handles(constraint_kind::same_rank));
    REQUIRE_FALSE(solver.handles(constraint_kind::broadcastable));
}

// ============================================================================
// Test 8 — composite_solver: same_type routes to unification backend
// ============================================================================

TEST_CASE (

"composite_solver: same_type routes to unification_solver"
,
"[vakya][constraints]"
)
 {
    type_arena arena;
    substitution subst;
    type_var_generator gen;

    composite_solver cs{unification_solver{}, rule_constraint_solver{}, graph_constraint_solver{}};

    // Unify T with Int via composite — should succeed (routed to unification)
    type_var_id tid = gen.fresh(); subst.make_var();
    type_ref t_ref  = arena.intern_variable(tid);
    type_ref int_ref = arena.intern_primitive<integer_type_tag>();

    constraint c;
    c.kind = constraint_kind::same_type;
    c.operands.push_back(t_ref);
    c.operands.push_back(int_ref);

    const constraint batch[1] = {c};
    solve_context ctx{&arena, &subst};
    auto r = cs.solve(std::span<const constraint>(batch, 1), ctx);

    REQUIRE(r.status == solve_status::solved);
    // T should now be bound to Int
    type_ref pruned = prune(t_ref, subst, arena);
    REQUIRE(pruned == int_ref);
}

// ============================================================================
// Test 9 — graph_constraint_solver: acyclic graph → solved
// ============================================================================

TEST_CASE (

"graph_constraint_solver: acyclic dependency graph → solved"
,
"[vakya][constraints]"
)
 {
    type_arena arena;
    substitution subst;
    graph_constraint_solver solver;

    type_ref int_ref   = arena.intern_primitive<integer_type_tag>();
    type_ref float_ref = arena.intern_primitive<float_type_tag>();
    type_ref bool_ref  = arena.intern_primitive<bool_type_tag>();

    // Build a linear chain: int → float → bool (acyclic)
    constraint c1, c2;
    c1.kind = constraint_kind::broadcastable;
    c1.operands.push_back(int_ref);
    c1.operands.push_back(float_ref);

    c2.kind = constraint_kind::broadcastable;
    c2.operands.push_back(float_ref);
    c2.operands.push_back(bool_ref);

    const constraint batch[2] = {c1, c2};
    solve_context ctx{&arena, &subst};
    auto r = solver.solve(std::span<const constraint>(batch, 2), ctx);

    REQUIRE(r.status == solve_status::solved);
}

// ============================================================================
// Test 10 — graph_constraint_solver: cycle → unsatisfiable
// ============================================================================

TEST_CASE (

"graph_constraint_solver: constraint cycle → unsatisfiable + diagnostic"
,
"[vakya][constraints]"
)
 {
    type_arena arena;
    substitution subst;
    graph_constraint_solver solver;

    type_ref int_ref   = arena.intern_primitive<integer_type_tag>();
    type_ref float_ref = arena.intern_primitive<float_type_tag>();

    // Create a back-edge: int → float AND float → int (cycle)
    constraint c1, c2;
    c1.kind = constraint_kind::same_rank;
    c1.operands.push_back(int_ref);
    c1.operands.push_back(float_ref);

    c2.kind = constraint_kind::same_rank;
    c2.operands.push_back(float_ref);
    c2.operands.push_back(int_ref);

    const constraint batch[2] = {c1, c2};
    solve_context ctx{&arena, &subst};
    auto r = solver.solve(std::span<const constraint>(batch, 2), ctx);

    REQUIRE(r.status == solve_status::unsatisfiable);
    REQUIRE_FALSE(r.diagnostics.empty());
    // diagnostic message should mention "cycle"
    REQUIRE(r.diagnostics[0].message.find("cycle") != std::string::npos);
}

// ============================================================================
// Test 11 — rule_constraint_solver: trait implication closure
// ============================================================================

TEST_CASE (

"rule_constraint_solver: trait implication closure + query"
,
"[vakya][constraints]"
)
 {
    type_arena arena;
    substitution subst;
    rule_constraint_solver solver;

    // trait ids
    constexpr std::uint64_t Numeric  = 0x01;
    constexpr std::uint64_t Addable  = 0x02;
    constexpr std::uint64_t Printable = 0x03;

    // Implications: Numeric => Addable, Addable => Printable
    solver.add_implication(Numeric, Addable);
    solver.add_implication(Addable, Printable);

    type_ref int_ref = arena.intern_primitive<integer_type_tag>();
    // Assert base fact: int implements Numeric
    solver.assert_trait(int_ref.index, Numeric);

    // Query: does int implement Printable? (transitively yes)
    constraint c;
    c.kind = constraint_kind::implements;
    c.operands.push_back(int_ref);
    c.trait_name_hash = Printable;

    const constraint batch[1] = {c};
    solve_context ctx{&arena, &subst};
    auto r = solver.solve(std::span<const constraint>(batch, 1), ctx);

    REQUIRE(r.status == solve_status::solved);
}

// ============================================================================
// Test 12 — composite_solver: mixed-kind batch
// ============================================================================

TEST_CASE (

"composite_solver: mixed-kind batch routes to correct backends"
,
"[vakya][constraints]"
)
 {
    type_arena arena;
    substitution subst;
    type_var_generator gen;

    rule_constraint_solver rule_s;
    constexpr std::uint64_t Numeric = 0x10;
    constexpr std::uint64_t Summable = 0x11;
    rule_s.add_implication(Numeric, Summable);
    type_ref int_ref = arena.intern_primitive<integer_type_tag>();
    rule_s.assert_trait(int_ref.index, Numeric);

    composite_solver cs{unification_solver{}, rule_s, graph_constraint_solver{}};

    // constraint 1: same_type (T == Int) → unification
    type_var_id tid = gen.fresh(); subst.make_var();
    type_ref t_ref  = arena.intern_variable(tid);
    constraint c1;
    c1.kind = constraint_kind::same_type;
    c1.operands.push_back(t_ref);
    c1.operands.push_back(int_ref);

    // constraint 2: implements Summable → rule solver
    constraint c2;
    c2.kind = constraint_kind::implements;
    c2.operands.push_back(int_ref);
    c2.trait_name_hash = Summable;

    const constraint batch[2] = {c1, c2};
    solve_context ctx{&arena, &subst};
    auto r = cs.solve(std::span<const constraint>(batch, 2), ctx);

    REQUIRE(r.status == solve_status::solved);
    // T bound to int_ref after unification
    REQUIRE(prune(t_ref, subst, arena) == int_ref);
}

// ============================================================================
// Test 13 — any_solver: type-erased wrapping
// ============================================================================

TEST_CASE (

"any_solver: type-erases unification_solver correctly"
,
"[vakya][constraints]"
)
 {
    type_arena arena;
    substitution subst;

    any_solver erased{unification_solver{}};

    REQUIRE(erased.handles(constraint_kind::same_type));
    REQUIRE_FALSE(erased.handles(constraint_kind::implements));

    type_ref int_ref  = arena.intern_primitive<integer_type_tag>();
    type_ref bool_ref = arena.intern_primitive<bool_type_tag>();

    constraint c;
    c.kind = constraint_kind::same_type;
    c.operands.push_back(int_ref);
    c.operands.push_back(bool_ref);

    const constraint batch[1] = {c};
    solve_context ctx{&arena, &subst};
    auto r = erased.solve(std::span<const constraint>(batch, 1), ctx);

    // Clash → unsatisfiable even through type erasure
    REQUIRE(r.status == solve_status::unsatisfiable);
}
