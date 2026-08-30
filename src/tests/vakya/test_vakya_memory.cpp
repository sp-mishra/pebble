// =============================================================================
// test_vakya_memory.cpp — memory-reasoning cluster (regions / typestate / rw summaries).
//
// Verifies:
//   vakya/types/region.hpp     — region algebra + syntactic disjointness
//   vakya/alias.hpp            — kDisjointKind, may_alias, disjoint_solver
//   vakya/types/typestate.hpp  — protocols, check_transition, affine_scope
//   vakya/types/rw_summary.hpp — rw summaries, predict_conflict, no_conflict_solver
//
// Cases:
//   1.  region_arena: distinct roots are syntactically disjoint
//   2.  region_arena: interning is structural (same projection → same handle)
//   3.  region_arena: distinct concrete fields of one root are disjoint
//   4.  region_arena: nested region (parent vs field) is NOT disjoint
//   5.  region_arena: symbolic index defers (not disjoint)
//   6.  region_arena: unite_alias makes aliases() true
//   7.  may_alias: disjoint roots → false; same region → true
//   8.  disjoint_solver: satisfies constraint_solver + solved on disjoint roots
//   9.  disjoint_solver: unsatisfiable when proven aliased
//  10.  disjoint_solver: routes through solve_batch to solved
//  11.  typestate: check_transition legal edge → ok
//  12.  typestate: illegal method → illegal_method
//  13.  affine_scope: advance moves state; double-consume → consumed
//  14.  affine_scope: leaked reports non-terminal live resource
//  15.  rw_summary: intern is structural; rw_insert keeps sorted-unique
//  16.  predict_conflict: disjoint write sets → no_conflict
//  17.  predict_conflict: write vs read on same region → conflict
//  18.  predict_conflict: symbolic pair → deferred
//  19.  no_conflict_solver: satisfies concept + deferred under no_smt via solve_batch
// =============================================================================

#include "catch_amalgamated.hpp"

#include "vakya/types/region.hpp"
#include "vakya/alias.hpp"
#include "vakya/types/typestate.hpp"
#include "vakya/types/rw_summary.hpp"
#include "vakya/constraint_registry.hpp"
#include "vakya/constraint_solvers.hpp"
#include "vakya/smt.hpp"
#include "containers/descriptor_registry.hpp"

#include <span>

using namespace vakya::types;

// ============================================================================
// 1. region_arena: distinct roots are syntactically disjoint
// ============================================================================

TEST_CASE("opt region: distinct roots disjoint", "[opt][region]") {
    region_arena arena;
    const region_ref a = arena.fresh_region();
    const region_ref b = arena.fresh_region();
    CHECK(regions_syntactically_disjoint(arena, a, b));
    CHECK_FALSE(a == b);
}

// ============================================================================
// 2. region_arena: structural interning
// ============================================================================

TEST_CASE("opt region: structural interning", "[opt][region]") {
    region_arena arena;
    const region_ref root = arena.fresh_region();
    const std::uint64_t f = 0xABCDu;
    const region_ref p1 = arena.project_field(root, f);
    const region_ref p2 = arena.project_field(root, f);
    CHECK(p1 == p2); // same projection interns to one handle
}

// ============================================================================
// 3. region_arena: distinct concrete fields disjoint
// ============================================================================

TEST_CASE("opt region: distinct fields disjoint", "[opt][region]") {
    region_arena arena;
    const region_ref root = arena.fresh_region();
    const region_ref fx = arena.project_field(root, 1u);
    const region_ref fy = arena.project_field(root, 2u);
    CHECK(regions_syntactically_disjoint(arena, fx, fy));
}

// ============================================================================
// 4. region_arena: nested region not disjoint
// ============================================================================

TEST_CASE("opt region: nested not disjoint", "[opt][region]") {
    region_arena arena;
    const region_ref root = arena.fresh_region();
    const region_ref fx = arena.project_field(root, 1u);
    // root vs root.field — parent overlaps child.
    CHECK_FALSE(regions_syntactically_disjoint(arena, root, fx));
}

// ============================================================================
// 5. region_arena: symbolic index defers
// ============================================================================

TEST_CASE("opt region: symbolic index not disjoint", "[opt][region]") {
    region_arena arena;
    const region_ref root = arena.fresh_region();
    const region_ref si = arena.project_index(root, kSymbolicIndex);
    const region_ref i0 = arena.project_index(root, 0u);
    CHECK_FALSE(regions_syntactically_disjoint(arena, si, i0));
}

// ============================================================================
// 6. region_arena: unite_alias
// ============================================================================

TEST_CASE("opt region: unite_alias", "[opt][region]") {
    region_arena arena;
    const region_ref a = arena.fresh_region();
    const region_ref b = arena.fresh_region();
    CHECK_FALSE(arena.aliases(a, b));
    arena.unite_alias(a, b);
    CHECK(arena.aliases(a, b));
}

// ============================================================================
// 7. may_alias
// ============================================================================

TEST_CASE("opt alias: may_alias basic", "[opt][alias]") {
    region_arena arena;
    const region_ref a = arena.fresh_region();
    const region_ref b = arena.fresh_region();
    CHECK_FALSE(may_alias(arena, a, b)); // disjoint roots
    CHECK(may_alias(arena, a, a));       // same region always may-alias
    CHECK(may_alias(arena, region_ref{}, a)); // null → conservative true
}

// ============================================================================
// 8. disjoint_solver: concept + solved
// ============================================================================

TEST_CASE("opt alias: disjoint_solver solved", "[opt][alias]") {
    static_assert(constraint_solver<disjoint_solver>);
    region_arena arena;
    const region_ref a = arena.fresh_region();
    const region_ref b = arena.fresh_region();

    disjoint_solver solver(arena);
    const constraint c = make_disjoint_constraint(a, b);
    solve_context ctx{nullptr, nullptr};
    const solve_result r = solver.solve(std::span<const constraint>(&c, 1), ctx);
    CHECK(r.status == solve_status::solved);
}

// ============================================================================
// 9. disjoint_solver: unsatisfiable when aliased
// ============================================================================

TEST_CASE("opt alias: disjoint_solver refuted on alias", "[opt][alias]") {
    region_arena arena;
    const region_ref a = arena.fresh_region();
    const region_ref b = arena.fresh_region();
    arena.unite_alias(a, b); // now proven aliased

    disjoint_solver solver(arena);
    const constraint c = make_disjoint_constraint(a, b);
    solve_context ctx{nullptr, nullptr};
    const solve_result r = solver.solve(std::span<const constraint>(&c, 1), ctx);
    CHECK(r.status == solve_status::unsatisfiable);
}

// ============================================================================
// 10. disjoint routes via solve_batch (registry seeds kDisjointKind → graph)
// ============================================================================

TEST_CASE("opt alias: disjoint via solve_batch", "[opt][alias]") {
    region_arena regions;
    const region_ref a = regions.fresh_region();
    const region_ref b = regions.fresh_region();

    auto reg = make_builtin_constraint_registry();
    const constraint_descriptor* d =
        reg.find(static_cast<std::uint32_t>(kDisjointKind));
    REQUIRE(d != nullptr);
    CHECK(d->target == solver_class::graph);

    type_arena arena;
    substitution subst;
    unification_solver us;
    rule_constraint_solver rs;
    disjoint_solver gs(regions); // graph-class slot handles kDisjointKind
    smt_constraint_solver<no_smt_backend> ss;
    composite_solver solver(us, rs, gs, ss);

    const constraint c = make_disjoint_constraint(a, b);
    solve_context ctx{&arena, &subst};
    const solve_result r =
        solve_batch(std::span<const constraint>(&c, 1), ctx, reg, solver);
    CHECK(r.status == solve_status::solved);
}

// ============================================================================
// 11-12. typestate: check_transition
// ============================================================================

namespace {
    // A minimal File protocol: Closed(1) --open--> Open(2) --close--> Closed(1).
    enum : typestate_id { kClosed = 1, kOpen = 2 };
    constexpr std::uint64_t kOpenM = 0x0Eu, kCloseM = 0x0Cu, kReadM = 0x0Du;

    constexpr transition kFileEdges[] = {
        transition{kClosed, kOpenM, kOpen, false},
        transition{kOpen, kReadM, kOpen, false},
        transition{kOpen, kCloseM, kClosed, true}, // close consumes the handle
    };
} // namespace

TEST_CASE("opt typestate: legal transition", "[opt][typestate]") {
    protocol_descriptor proto;
    proto.stable_id = 1;
    proto.transitions = kFileEdges;
    proto.transition_count = 3;
    proto.initial = kClosed;
    proto.terminal = kClosed;

    const transition_result tr = check_transition(proto, kClosed, kOpenM);
    CHECK(tr.status == transition_status::ok);
    CHECK(tr.next == kOpen);
}

TEST_CASE("opt typestate: illegal method", "[opt][typestate]") {
    protocol_descriptor proto;
    proto.transitions = kFileEdges;
    proto.transition_count = 3;
    // read is illegal from Closed.
    const transition_result tr = check_transition(proto, kClosed, kReadM);
    CHECK(tr.status == transition_status::illegal_method);
}

// ============================================================================
// 13. affine_scope: advance + double-consume
// ============================================================================

TEST_CASE("opt typestate: affine_scope consume", "[opt][typestate]") {
    protocol_descriptor proto;
    proto.transitions = kFileEdges;
    proto.transition_count = 3;
    proto.terminal = kClosed;

    region_arena regions;
    const region_ref file = regions.fresh_region();

    affine_scope scope;
    scope.track(file, kClosed);
    CHECK(scope.advance(proto, file, kOpenM).status == transition_status::ok);
    CHECK(scope.advance(proto, file, kCloseM).status == transition_status::ok); // consumes
    // Any further use of the consumed handle is rejected.
    CHECK(scope.advance(proto, file, kReadM).status == transition_status::consumed);
}

// ============================================================================
// 14. affine_scope: leaked
// ============================================================================

TEST_CASE("opt typestate: affine_scope leaked", "[opt][typestate]") {
    protocol_descriptor proto;
    proto.transitions = kFileEdges;
    proto.transition_count = 3;
    proto.terminal = kClosed;

    region_arena regions;
    const region_ref file = regions.fresh_region();

    affine_scope scope;
    scope.track(file, kClosed);
    (void)scope.advance(proto, file, kOpenM); // now Open, not terminal
    const auto leaked = scope.leaked(kClosed);
    REQUIRE(leaked.size() == 1);
    CHECK(leaked[0] == file);
}

// ============================================================================
// 15. rw_summary: interning + sorted-unique insert
// ============================================================================

TEST_CASE("opt rw_summary: intern + insert", "[opt][rw]") {
    region_arena regions;
    const region_ref a = regions.fresh_region();
    const region_ref b = regions.fresh_region();

    rw_summary s;
    rw_insert(s.writes, b);
    rw_insert(s.writes, a);
    rw_insert(s.writes, a); // duplicate ignored
    REQUIRE(s.writes.size() == 2);
    CHECK(s.writes[0].index < s.writes[1].index); // sorted

    rw_summary_arena arena;
    const rw_summary_ref r1 = arena.intern_rw_summary(s);
    const rw_summary_ref r2 = arena.intern_rw_summary(s);
    CHECK(r1 == r2); // structural interning
}

// ============================================================================
// 16-18. predict_conflict
// ============================================================================

TEST_CASE("opt rw_summary: disjoint no_conflict", "[opt][rw]") {
    region_arena regions;
    const region_ref a = regions.fresh_region();
    const region_ref b = regions.fresh_region();

    rw_summary sa; rw_insert(sa.writes, a);
    rw_summary sb; rw_insert(sb.writes, b);
    CHECK(predict_conflict(regions, sa, sb) == conflict_result::no_conflict);
}

TEST_CASE("opt rw_summary: write-read conflict", "[opt][rw]") {
    region_arena regions;
    const region_ref a = regions.fresh_region();

    rw_summary sa; rw_insert(sa.writes, a);
    rw_summary sb; rw_insert(sb.reads, a); // reads what A writes
    CHECK(predict_conflict(regions, sa, sb) == conflict_result::conflict);
}

TEST_CASE("opt rw_summary: symbolic deferred", "[opt][rw]") {
    region_arena regions;
    const region_ref root = regions.fresh_region();
    const region_ref si = regions.project_index(root, kSymbolicIndex);
    const region_ref i0 = regions.project_index(root, 0u);

    rw_summary sa; rw_insert(sa.writes, si);
    rw_summary sb; rw_insert(sb.writes, i0);
    CHECK(predict_conflict(regions, sa, sb) == conflict_result::deferred);
}

// ============================================================================
// 19. no_conflict_solver: concept + solve_batch deferred under no_smt
// ============================================================================

TEST_CASE("opt rw_summary: no_conflict_solver deferred", "[opt][rw]") {
    static_assert(constraint_solver<no_conflict_solver>);
    region_arena regions;
    rw_summary_arena summaries;

    const region_ref root = regions.fresh_region();
    const region_ref si = regions.project_index(root, kSymbolicIndex);
    const region_ref i0 = regions.project_index(root, 0u);

    rw_summary sa; rw_insert(sa.writes, si);
    rw_summary sb; rw_insert(sb.writes, i0);
    const rw_summary_ref ra = summaries.intern_rw_summary(sa);
    const rw_summary_ref rb = summaries.intern_rw_summary(sb);

    no_conflict_solver solver(regions, summaries);
    const constraint c = make_noconflict_obligation(ra, rb);
    solve_context ctx{nullptr, nullptr};
    const solve_result r = solver.solve(std::span<const constraint>(&c, 1), ctx);
    CHECK(r.status == solve_status::deferred); // symbolic pair → SMT band
}
