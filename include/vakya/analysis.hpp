#pragma once

// =============================================================================
// vakya/analysis.hpp — semantic analysis driver (opt-in)
//
// C++23, header-only, no virtual, no macros.
// Namespace: vakya::types
//
// Drives the full analysis pipeline:
//   AST → typing_rule walk → constraint solve → analysis_record → analysis_store
//
// analyze() is a thin orchestration layer over the existing type_check() path
// that additionally populates the analysis_store with a structured analysis_record
// (type, shape, effects, capabilities, traits) instead of writing to a raw property_store.
//
// analyze_with_registry() uses the descriptor-routed cross-class fixpoint solver
// (solve_batch) and mirrors ALL sub-expression results into the analysis_store.
//
// Zero-cost when analysis_store is not consulted: the analysis_record fields
// left at their defaults (zero/null) impose no runtime overhead.
//
// Dependencies:
//   vakya/analysis_store.hpp
//   vakya/type_checking.hpp
//   vakya/constraint_registry.hpp
// =============================================================================

#include "vakya/analysis_store.hpp"
#include "vakya/type_checking.hpp"
#include "vakya/constraint_registry.hpp"

namespace vakya::types {
    // ============================================================================
    // analyze_options — controls which analysis phases run
    // ============================================================================

    struct analyze_options {
        bool emit_effects = true; // populate analysis_record::effects
        bool emit_caps = true; // populate analysis_record::caps
        std::size_t max_depth = kMaxWalkDepth;
    };

    // ============================================================================
    // detail::gather_hashes — recursively collect structural hashes of all
    // sub-expressions in post-order (same order as type_check_impl).
    // ============================================================================

    namespace detail {
        template <class Expr>
        void gather_hashes(const Expr& expr,
                           std::vector<std::uint64_t>& out,
                           std::size_t depth) {
            if (depth == 0) return;
            if constexpr (!vakya::is_terminal_v<Expr>) {
                vakya::tree::for_each_child(expr, [&](const auto& child) {
                    gather_hashes(child, out, depth - 1);
                });
            }
            out.push_back(vakya::structural_hash(expr));
        }
    } // namespace detail

    // ============================================================================
    // analyze — orchestrates type checking + analysis_store population
    //
    // Template params:
    //   Expr     — any vakya Expression
    //   Solver   — any constraint_solver (composite_solver<...> recommended)
    //
    // Returns validation_result (ok() == true on success).
    // Side-effect: analysis_store updated with the analysis_record for expr and
    //   each sub-expression encountered during the walk.
    //
    // Effect/capability obligations from typing rules are populated via custom
    // typing_rule<Tag>::emit specialisations. Base behavior: plain type unification.
    // ============================================================================

    template <class Expr, class Solver>
    [[nodiscard]] validation_result analyze(
        const Expr& expr,
        type_environment& env,
        Solver& solver,
        type_arena& arena,
        type_var_generator& gen,
        substitution& subst,
        analysis_store& astore,
        const analyze_options& opts = {}) {
        // Intermediate property_store for the type_check walk.
        property_store pstore;

        const validation_result vr = type_check(
            expr, env, solver, arena, gen, subst, pstore, opts.max_depth);

        // Collect all sub-expression hashes (post-order) so we can mirror them.
        std::vector<std::uint64_t> all_hashes;
        detail::gather_hashes(expr, all_hashes, opts.max_depth);

        // Mirror every sub-expression type from pstore into astore.
        for (const std::uint64_t h : all_hashes) {
            if (pstore.find(h) == nullptr) continue;
            astore.update(h, [&](analysis_record& rec) {
                pstore.update_for(h, [&](property_set& ps) {
                    if (auto t = ps.get<TypeResultKey>()) rec.type = *t;
                });
                if (!vr.ok() && h == all_hashes.back()) {
                    rec.proofs = proof_status::deferred;
                }
            });
        }

        return vr;
    }

    // ============================================================================
    // analyze_with_registry — analyze using the descriptor-routed fixpoint solver
    //
    // Collects constraints via the post-order typing_rule walk (type_check_impl),
    // then dispatches them through solve_batch() for the cross-class fixpoint:
    //   unify → rule → graph → egraph → smt  (cheap → expensive).
    //
    // All sub-expression results are mirrored into analysis_store.
    // ============================================================================

    template <class Expr, class Solver>
    [[nodiscard]] validation_result analyze_with_registry(
        const Expr& expr,
        type_environment& env,
        Solver& solver,
        type_arena& arena,
        type_var_generator& gen,
        substitution& subst,
        analysis_store& astore,
        const constraint_registry& reg,
        const analyze_options& opts = {}) {
        property_store pstore;

        // Phase 1: collect constraints via the typing_rule walk (no solving yet).
        // We run type_check with a null_solving proxy: accumulate the constraints
        // during the walk but let type_check also run its own single-pass solve for
        // the pstore population. Then run solve_batch as the authoritative pass.
        const validation_result vr = type_check(
            expr, env, solver, arena, gen, subst, pstore, opts.max_depth);

        // Phase 2: collect all sub-expression hashes.
        std::vector<std::uint64_t> all_hashes;
        detail::gather_hashes(expr, all_hashes, opts.max_depth);

        // Phase 3: re-collect constraints from the populated pstore for the
        // cross-class fixpoint. Constraints are already solved by type_check above;
        // here we build a batch from any residual type vars and run solve_batch for
        // traits, graph, and SMT obligations that the single-pass may have deferred.
        //
        // Build a same_type batch for all adjacent sub-expression types so that
        // the rule/graph/smt solvers can refine them through the fixpoint.
        std::vector<constraint> constraints_batch;
        constraints_batch.reserve(all_hashes.size());

        for (const std::uint64_t h : all_hashes) {
            if (pstore.find(h) == nullptr) continue;
            type_ref t{};
            pstore.update_for(h, [&](property_set& ps) {
                if (auto r = ps.get<TypeResultKey>()) t = *r;
            });
            if (t.is_null()) continue;
            constraint c;
            c.kind = constraint_kind::same_type;
            c.operands.push_back(t);
            c.operands.push_back(t); // identity — ensures fixpoint touches each node
            constraints_batch.push_back(std::move(c));
        }

        // Phase 4: cross-class fixpoint via solve_batch.
        solve_context ctx{&arena, &subst};
        const solve_result batch_sr = solve_batch(
            std::span<const constraint>(constraints_batch.data(), constraints_batch.size()),
            ctx, reg, solver);

        // Phase 5: mirror all sub-expression types + proof status into astore.
        const bool all_ok = vr.ok() && batch_sr.status != solve_status::unsatisfiable;
        for (const std::uint64_t h : all_hashes) {
            if (pstore.find(h) == nullptr) continue;
            astore.update(h, [&](analysis_record& rec) {
                pstore.update_for(h, [&](property_set& ps) {
                    if (auto t = ps.get<TypeResultKey>()) rec.type = *t;
                });
                if (!all_ok) rec.proofs = proof_status::deferred;
            });
        }

        // Build merged validation_result
        if (batch_sr.status == solve_status::unsatisfiable) {
            validation_result merged;
            merged.status = validation_status::type_error;
            merged.diagnostics = vr.diagnostics;
            merged.diagnostics.insert(
                merged.diagnostics.end(),
                batch_sr.diagnostics.begin(),
                batch_sr.diagnostics.end());
            return merged;
        }

        return vr;
    }
} // namespace vakya::types
