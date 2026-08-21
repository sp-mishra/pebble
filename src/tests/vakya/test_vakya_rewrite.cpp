// =============================================================================
// test_vakya_rewrite.cpp — guarded rewrite rules: guard fires/blocks,
//                           plain rule backward compat.
//
// Verifies: include/vakya/rewrite.hpp
//           include/vakya/pattern.hpp  (backward compat path)
//
// Cases:
//   1. plain pattern::rule (no guard) matches and fires.
//   2. plain pattern::rule misses → nullopt.
//   3. guarded rule with always_true_guard fires identically to plain rule.
//   4. guarded rule with guard returning false → blocks rewrite → nullopt.
//   5. guarded rule: guard receives match bindings (can inspect them).
//   6. guarded rule: try_apply(expr) no-env overload ignores guard → fires.
//   7. named guarded rule: name field set correctly.
//   8. factory: guarded(pattern, rhs) produces always_true_guard variant.
//   9. factory: guarded(pattern, rhs, guard) threads custom guard.
//  10. GuardFn concept: lambda with correct signature satisfies concept.
//  11. GuardFn concept: lambda with wrong return type does NOT satisfy concept.
//  12. backward-compat: guarded_rule<P,R,always_true_guard>::try_apply == plain rule.
// =============================================================================

#include "catch_amalgamated.hpp"

#include "vakya/rewrite.hpp"

using namespace vakya::types;
using namespace vakya;
using namespace vakya::pattern;

// ============================================================================
// Shared leaf tag for tests
// ============================================================================

namespace {
    struct r_int_leaf_tag {};
} // namespace

// Make r_int_leaf_tag a terminal so it is accepted as an Operand
template <>
struct vakya::is_terminal<r_int_leaf_tag> : std::true_type {};

// Convenience: build a leaf node for use in patterns and expressions
inline auto int_leaf() { return make_node<r_int_leaf_tag>(); }

// ============================================================================
// Test 1 — plain pattern::rule fires on match
// ============================================================================

TEST_CASE (

"pattern::rule: matches and fires rewrite builder"
,
"[vakya][rewrite]"
)
 {
    constexpr auto X = pattern_var<1>{};
    auto pat = make_node<r_int_leaf_tag>();

    auto r = rule(pat, [](match_result) {
        return make_node<r_int_leaf_tag>();
    });

    auto result = r.try_apply(int_leaf());
    REQUIRE(result.has_value());
}

// ============================================================================
// Test 2 — plain pattern::rule misses → nullopt
// ============================================================================

TEST_CASE (

"pattern::rule: mismatch → nullopt"
,
"[vakya][rewrite]"
)
 {
    // Pattern matches add(leaf, leaf); apply to plain leaf → miss
    auto pat = make_node<add_tag>(make_node<r_int_leaf_tag>(), make_node<r_int_leaf_tag>());

    auto r = rule(pat, [](match_result) {
        return make_node<r_int_leaf_tag>();
    });

    auto result = r.try_apply(int_leaf());
    REQUIRE_FALSE(result.has_value());
}

// ============================================================================
// Test 3 — guarded rule with always_true_guard fires identically to plain rule
// ============================================================================

TEST_CASE (

"guarded rule: always_true_guard fires identical to plain rule"
,
"[vakya][rewrite]"
)
 {
    type_arena arena;
    substitution subst;
    type_var_generator gen;
    type_environment env;
    unification_solver solver;

    auto pat = make_node<r_int_leaf_tag>();
    auto gr = guarded(pat, [](match_result) {
        return make_node<r_int_leaf_tag>();
    });

    auto result = gr.try_apply(int_leaf(), env, solver);
    REQUIRE(result.has_value());
}

// ============================================================================
// Test 4 — guarded rule: blocking guard → nullopt
// ============================================================================

TEST_CASE (

"guarded rule: guard returning false blocks rewrite → nullopt"
,
"[vakya][rewrite]"
)
 {
    type_arena arena;
    substitution subst;
    type_var_generator gen;
    type_environment env;
    unification_solver solver;

    // Guard that always blocks
    auto blocking = [](const match_result& /*m*/, type_environment& /*env*/,
                       unification_solver& /*s*/) -> bool {
        return false;
    };

    auto pat = make_node<r_int_leaf_tag>();
    auto gr = guarded(pat,
                      [](match_result) { return make_node<r_int_leaf_tag>(); },
                      blocking);

    auto result = gr.try_apply(int_leaf(), env, solver);
    REQUIRE_FALSE(result.has_value());
}

// ============================================================================
// Test 5 — guarded rule: guard receives and can inspect match bindings
// ============================================================================

TEST_CASE (

"guarded rule: guard inspects match bindings"
,
"[vakya][rewrite]"
)
 {
    type_arena arena;
    substitution subst;
    type_environment env;
    unification_solver solver;

    bool guard_called = false;

    auto inspecting_guard = [&](const match_result& /*m*/, type_environment& /*e*/,
                                unification_solver& /*s*/) -> bool {
        guard_called = true;
        return true;
    };

    auto pat = make_node<r_int_leaf_tag>();
    auto gr = guarded(pat,
                      [](match_result) { return make_node<r_int_leaf_tag>(); },
                      inspecting_guard);

    auto result = gr.try_apply(int_leaf(), env, solver);
    REQUIRE(result.has_value());
    REQUIRE(guard_called);
}

// ============================================================================
// Test 6 — guarded rule: try_apply(expr) no-env overload bypasses guard
// ============================================================================

TEST_CASE (

"guarded rule: no-env try_apply overload bypasses guard (guard never called)"
,
"[vakya][rewrite]"
)
 {
    bool guard_called = false;

    auto guard_that_tracks = [&](const match_result& /*m*/, type_environment& /*e*/,
                                  unification_solver& /*s*/) -> bool {
        guard_called = true;
        return false; // would block if called
    };

    auto pat = make_node<r_int_leaf_tag>();
    auto gr = guarded(pat,
                      [](match_result) { return make_node<r_int_leaf_tag>(); },
                      guard_that_tracks);

    // Use the no-env overload: guard should NOT be called
    auto result = gr.try_apply(int_leaf());
    REQUIRE(result.has_value());   // fires despite guard returning false
    REQUIRE_FALSE(guard_called);   // guard was never invoked
}

// ============================================================================
// Test 7 — named guarded rule: name field persists
// ============================================================================

TEST_CASE (

"guarded rule: named rule has correct name field"
,
"[vakya][rewrite]"
)
 {
    auto pat = make_node<r_int_leaf_tag>();
    auto gr = guarded("leaf_identity",
                      pat,
                      [](match_result) { return make_node<r_int_leaf_tag>(); },
                      always_true_guard{});

    REQUIRE(gr.name == "leaf_identity");
}

// ============================================================================
// Test 8 — factory guarded(pat, rhs) produces always_true_guard
// ============================================================================

TEST_CASE (

"guarded factory (no guard): produces always_true_guard variant"
,
"[vakya][rewrite]"
)
 {
    auto pat = make_node<r_int_leaf_tag>();
    auto gr = guarded(pat, [](match_result) { return make_node<r_int_leaf_tag>(); });

    // always_true_guard: match → always fires
    type_environment env;
    unification_solver solver;
    auto result = gr.try_apply(int_leaf(), env, solver);
    REQUIRE(result.has_value());
}

// ============================================================================
// Test 9 — factory guarded(pat, rhs, guard): threads custom guard
// ============================================================================

TEST_CASE (

"guarded factory (with guard): custom guard threaded correctly"
,
"[vakya][rewrite]"
)
 {
    int call_count = 0;
    auto counting_guard = [&](const match_result&, type_environment&,
                               unification_solver&) -> bool {
        ++call_count;
        return true;
    };

    auto pat = make_node<r_int_leaf_tag>();
    auto gr = guarded(pat,
                      [](match_result) { return make_node<r_int_leaf_tag>(); },
                      counting_guard);

    type_environment env;
    unification_solver solver;
    (void)gr.try_apply(int_leaf(), env, solver);
    (void)gr.try_apply(int_leaf(), env, solver);

    REQUIRE(call_count == 2);
}

// ============================================================================
// Test 10 — GuardFn concept: correct lambda satisfies concept
// ============================================================================

TEST_CASE (

"GuardFn concept: lambda with correct signature satisfies GuardFn<G, Solver>"
,
"[vakya][rewrite]"
)
 {
    auto ok_guard = [](const match_result&, type_environment&,
                       unification_solver&) -> bool { return true; };

    STATIC_REQUIRE(GuardFn<decltype(ok_guard), unification_solver>);
}

// ============================================================================
// Test 11 — GuardFn concept: wrong return type does NOT satisfy concept
// ============================================================================

TEST_CASE (

"GuardFn concept: lambda returning void does NOT satisfy GuardFn<G, Solver>"
,
"[vakya][rewrite]"
)
 {
    auto bad_guard = [](const match_result&, type_environment&,
                        unification_solver&) -> void {};

    STATIC_REQUIRE_FALSE(GuardFn<decltype(bad_guard), unification_solver>);
}

// ============================================================================
// Test 12 — backward compat: guarded_rule<P,R,always_true_guard> == plain rule
// ============================================================================

TEST_CASE (

"backward compat: guarded_rule with always_true_guard behaves as plain rewrite_rule"
,
"[vakya][rewrite]"
)
 {
    // Build identical pattern/rhs for both styles
    auto pat1 = make_node<add_tag>(make_node<r_int_leaf_tag>(), make_node<r_int_leaf_tag>());
    auto pat2 = make_node<add_tag>(make_node<r_int_leaf_tag>(), make_node<r_int_leaf_tag>());

    auto rhs_fn = [](match_result) { return make_node<r_int_leaf_tag>(); };

    // Plain rule
    auto plain = rule(pat1, rhs_fn);
    // Guarded rule with always_true_guard via no-env try_apply
    auto guarded_r = guarded(pat2, rhs_fn);

    auto expr = make_node<add_tag>(int_leaf(), int_leaf());

    auto r_plain   = plain.try_apply(expr);
    auto r_guarded = guarded_r.try_apply(expr);  // no-env overload

    // Both should match or both miss
    REQUIRE(r_plain.has_value() == r_guarded.has_value());
}
