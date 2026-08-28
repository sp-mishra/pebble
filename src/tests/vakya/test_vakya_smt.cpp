// =============================================================================
// test_vakya_smt.cpp — vakya/smt.hpp: concept checks, stub, Tarka bridge
//
// Verifies: include/vakya/smt.hpp
//
// Cases:
//   1. STATIC: no_smt_backend satisfies smt_backend concept.
//   2. STATIC: smt_constraint_solver<no_smt_backend> satisfies constraint_solver.
//   3. no_smt_backend::check_sat() returns deferred.
//   4. smt_constraint_solver<no_smt_backend>::handles() always returns false.
//   5. smt_constraint_solver<no_smt_backend>::solve() returns deferred.
//  --- Tarka bridge (HAS_Z3 and tarka/tarka.hpp available) ---
//   6. STATIC: tarka_smt_backend<no_solver_backend> satisfies smt_backend.
//   7. STATIC: tarka_smt_constraint_solver<no_solver_backend> satisfies constraint_solver.
//   8. tarka_smt_backend<no_solver_backend>::check_sat() returns deferred.
//   9. tarka_smt_backend<no_solver_backend>::assert_formula() is a no-op.
//  10. tarka_smt_constraint_solver handles constraint_kind::user.
//  11. tarka_smt_constraint_solver does NOT handle same_type / implements.
//  12. tarka_smt_constraint_solver: empty batch → deferred.
//  13. tarka_smt_constraint_solver: null payload skipped (no crash).
//  --- Z3 path ---
//  14. tarka_smt_backend<z3_backend>: assert_tarka + check_sat on SAT term → solved.
//  15. tarka_smt_backend<z3_backend>: UNSAT term → unsatisfiable.
//  16. tarka_smt_backend<z3_backend>: get_value returns correct model value.
//  17. tarka_smt_constraint_solver<z3_backend>: constraint with Tarka Term* → solved.
// =============================================================================

#include "catch_amalgamated.hpp"

#include "vakya/smt.hpp"

using namespace vakya::types;

// ============================================================================
// Tests 1-5 — stub path (no Tarka dependency)
// ============================================================================

TEST_CASE (

"vakya smt: no_smt_backend satisfies smt_backend concept"
,
"[vakya][smt][static]"
)
 {
    STATIC_REQUIRE(smt_backend<no_smt_backend>);
}

TEST_CASE (

"vakya smt: smt_constraint_solver<no_smt_backend> satisfies constraint_solver"
,
"[vakya][smt][static]"
)
 {
    STATIC_REQUIRE(constraint_solver<smt_constraint_solver<no_smt_backend>>);
}

TEST_CASE (

"vakya smt: no_smt_backend check_sat returns deferred"
,
"[vakya][smt][stub]"
)
 {
    no_smt_backend b;
    smt_formula f{42};
    b.assert_formula(f);
    REQUIRE(b.check_sat() == solve_status::deferred);
}

TEST_CASE (

"vakya smt: smt_constraint_solver<no_smt_backend> handles() always false"
,
"[vakya][smt][stub]"
)
 {
    smt_constraint_solver<no_smt_backend> solver;
    REQUIRE(!solver.handles(constraint_kind::same_type));
    REQUIRE(!solver.handles(constraint_kind::user));
    REQUIRE(!solver.handles(constraint_kind::implements));
    REQUIRE(!solver.handles(static_cast<constraint_kind>(kConstraintKindExtensionBase)));
}

TEST_CASE (

"vakya smt: smt_constraint_solver<no_smt_backend> solve returns deferred"
,
"[vakya][smt][stub]"
)
 {
    smt_constraint_solver<no_smt_backend> solver;
    type_arena arena;
    substitution subst;
    solve_context ctx{&arena, &subst};

    const solve_result r = solver.solve({}, ctx);
    REQUIRE(r.status == solve_status::deferred);
}

// ============================================================================
// Tarka bridge tests — gated on tarka/tarka.hpp availability
// ============================================================================

#if __has_include("tarka/tarka.hpp")

#include "tarka/tarka.hpp"
#include "tarka/backends/no_solver.hpp"

using namespace tarka;
using namespace tarka::backend;

// ============================================================================
// Tests 6-7 — static concept checks (Tarka bridge)
// ============================================================================

TEST_CASE ("vakya smt: tarka_smt_backend<no_solver> satisfies smt_backend",
          "[vakya][smt][tarka][static]") {
    STATIC_REQUIRE(smt_backend<tarka_smt_backend<no_solver_backend>>);
}

TEST_CASE ("vakya smt: tarka_smt_constraint_solver<no_solver> satisfies constraint_solver",
          "[vakya][smt][tarka][static]") {
    STATIC_REQUIRE(constraint_solver<tarka_smt_constraint_solver<no_solver_backend>>);
}

// ============================================================================
// Tests 8-9 — tarka_smt_backend with no_solver_backend
// ============================================================================

TEST_CASE ("vakya smt: tarka_smt_backend<no_solver> check_sat returns deferred",
          "[vakya][smt][tarka]") {
    tarka_smt_backend<no_solver_backend> b;
    REQUIRE(b.check_sat() == solve_status::deferred);
}

TEST_CASE ("vakya smt: tarka_smt_backend<no_solver> assert_formula is no-op",
          "[vakya][smt][tarka]") {
    tarka_smt_backend<no_solver_backend> b;
    smt_formula f{0};
    b.assert_formula(f);  // must not crash
    REQUIRE(b.check_sat() == solve_status::deferred);
}

// ============================================================================
// Tests 10-13 — tarka_smt_constraint_solver routing
// ============================================================================

TEST_CASE ("vakya smt: tarka_smt_constraint_solver handles user kind",
          "[vakya][smt][tarka]") {
    tarka_smt_constraint_solver<no_solver_backend> solver;
    REQUIRE(solver.handles(constraint_kind::user));
    REQUIRE(solver.handles(static_cast<constraint_kind>(kConstraintKindExtensionBase)));
    REQUIRE(solver.handles(static_cast<constraint_kind>(kConstraintKindExtensionBase + 1)));
}

TEST_CASE ("vakya smt: tarka_smt_constraint_solver does not handle same_type/implements",
          "[vakya][smt][tarka]") {
    tarka_smt_constraint_solver<no_solver_backend> solver;
    REQUIRE(!solver.handles(constraint_kind::same_type));
    REQUIRE(!solver.handles(constraint_kind::implements));
    REQUIRE(!solver.handles(constraint_kind::same_rank));
}

TEST_CASE ("vakya smt: tarka_smt_constraint_solver empty batch returns deferred",
          "[vakya][smt][tarka]") {
    tarka_smt_constraint_solver<no_solver_backend> solver;
    type_arena arena;
    substitution subst;
    solve_context ctx{&arena, &subst};

    const solve_result r = solver.solve({}, ctx);
    REQUIRE(r.status == solve_status::deferred);
}

TEST_CASE ("vakya smt: tarka_smt_constraint_solver null payload skipped without crash",
          "[vakya][smt][tarka]") {
    tarka_smt_constraint_solver<no_solver_backend> solver;
    type_arena arena;
    substitution subst;
    solve_context ctx{&arena, &subst};

    constraint c;
    c.kind    = constraint_kind::user;
    c.payload = 0;  // null — must be skipped

    const solve_result r = solver.solve(std::span<const constraint>(&c, 1), ctx);
    REQUIRE(r.status == solve_status::deferred);
}

// ============================================================================
// Z3-specific tests
// ============================================================================

#if defined(HAS_Z3)
#include "tarka/backends/z3_backend.hpp"

// ============================================================================
// Test 14 — tarka_smt_backend<z3_backend>: SAT formula → solved
// ============================================================================

TEST_CASE ("vakya smt: tarka_smt_backend<z3_backend> SAT term → solved",
          "[vakya][smt][tarka][z3]") {
    tarka::Context ctx;
    const Sort bool_s = ctx.bool_sort();
    const Term p      = ctx.make_symbol("p", bool_s);

    tarka_smt_backend<z3_backend> bridge;
    bridge.assert_tarka(p);
    REQUIRE(bridge.check_sat() == solve_status::solved);
}

// ============================================================================
// Test 15 — tarka_smt_backend<z3_backend>: UNSAT term → unsatisfiable
// ============================================================================

TEST_CASE ("vakya smt: tarka_smt_backend<z3_backend> UNSAT term → unsatisfiable",
          "[vakya][smt][tarka][z3]") {
    tarka::Context ctx;
    const Sort bool_s = ctx.bool_sort();
    const Term p      = ctx.make_symbol("p", bool_s);

    // p && !p
    const Term not_p_ch[1] = {p};
    const Term not_p        = ctx.make_term(Op::Not, bool_s, not_p_ch);
    const Term and_ch[2]   = {p, not_p};
    const Term formula      = ctx.make_term(Op::And, bool_s, and_ch);

    tarka_smt_backend<z3_backend> bridge;
    bridge.assert_tarka(formula);
    REQUIRE(bridge.check_sat() == solve_status::unsatisfiable);
}

// ============================================================================
// Test 16 — tarka_smt_backend<z3_backend>: get_value returns model value
// ============================================================================

TEST_CASE ("vakya smt: tarka_smt_backend<z3_backend> get_value extracts model",
          "[vakya][smt][tarka][z3]") {
    tarka::Context ctx;
    const Sort bv32 = ctx.bv_sort(32);
    const Term x    = ctx.make_symbol("x", bv32);
    const Term v7   = ctx.make_value(7u, bv32);

    // x == 7
    const Term ch[2]   = {x, v7};
    const Term formula = ctx.make_term(Op::Eq, ctx.bool_sort(), ch);

    tarka_smt_backend<z3_backend> bridge;
    bridge.assert_tarka(formula);
    REQUIRE(bridge.check_sat() == solve_status::solved);

    const auto val = bridge.get_value(x);
    REQUIRE(val.has_value());
    const auto* bv = std::get_if<tarka::bv_value>(&val.value());
    REQUIRE(bv != nullptr);
    REQUIRE(bv->bits == 7u);
}

// ============================================================================
// Test 17 — tarka_smt_constraint_solver<z3_backend>: Term* in payload → solved
// ============================================================================

TEST_CASE ("vakya smt: tarka_smt_constraint_solver<z3_backend> Term* payload → solved",
          "[vakya][smt][tarka][z3]") {
    tarka::Context ctx;
    const Sort bool_s = ctx.bool_sort();
    const Term p      = ctx.make_symbol("p", bool_s);

    tarka_smt_constraint_solver<z3_backend> solver;
    type_arena arena;
    substitution subst;
    solve_context sctx{&arena, &subst};

    // Store Term pointer in payload (caller contract)
    constraint c;
    c.kind    = constraint_kind::user;
    c.payload = reinterpret_cast<std::uint64_t>(&p);

    const solve_result r = solver.solve(std::span<const constraint>(&c, 1), sctx);
    REQUIRE(r.status == solve_status::solved);
}

#endif // HAS_Z3
#endif // __has_include("tarka/tarka.hpp")
