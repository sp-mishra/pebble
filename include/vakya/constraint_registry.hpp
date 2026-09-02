#pragma once

// =============================================================================
// vakya/constraint_registry.hpp — descriptor-routed constraint registry (opt-in)
//
// C++23, header-only, no virtual, no macros.
// Namespace: vakya::types
//
// Replaces the linear `handles(kind)` scan with declarative O(1) routing:
//   Constraint → constraint_descriptor → constraint_registry → solver routing
//
// constraint_descriptor = {
//   constraint_kind kind; fixed stable_id; solver_class target;
//   theory_mask theory; uint8_t cost_hint; string_view symbol; uint64_t name_hash;
// }
//
// solver_class ∈ { unify, rule, graph, egraph, smt }
//
// solve_batch() implements the cross-class fixpoint:
//   unify → rule → graph → egraph → smt   (cheap → expensive)
// Empty buckets are skipped; the SMT branch is never entered if no smt constraints.
//
// Dependencies: vakya/constraints.hpp, containers/descriptor_registry.hpp
// =============================================================================

#include "vakya/constraints.hpp"
#include "containers/descriptor_registry.hpp"

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace vakya::types {
    // ============================================================================
    // solver_class — which backend handles a constraint kind
    // ============================================================================

    enum class solver_class : std::uint8_t {
        unify = 0,
        rule = 1,
        graph = 2,
        egraph = 3,
        smt = 4,
    };

    inline constexpr std::size_t kNumSolverClasses = 5;

    // ============================================================================
    // theory_mask — bitmask of applicable theories (for documentation / SMT lowering)
    // ============================================================================

    using theory_mask = std::uint32_t;
    inline constexpr theory_mask kTheoryEquality = 1u << 0;
    inline constexpr theory_mask kTheoryArithmetic = 1u << 1;
    inline constexpr theory_mask kTheoryArrays = 1u << 2;
    inline constexpr theory_mask kTheoryBitvector = 1u << 3;

    // ============================================================================
    // constraint_descriptor — registrable per-kind descriptor
    // ============================================================================

    struct constraint_descriptor {
        // RegistrableDescriptor fields
        std::uint32_t stable_id = 0; // same value as the constraint_kind cast
        std::uint64_t name_hash = 0;
        solver_class category = solver_class::unify; // used as category for registry

        // Routing metadata
        constraint_kind kind = constraint_kind::same_type;
        solver_class target = solver_class::unify; // primary solver class
        theory_mask theory = kTheoryEquality;
        std::uint8_t cost_hint = 0; // 0 = cheap, 255 = expensive
        std::string_view symbol{};
    };

    static_assert(containers::RegistrableDescriptor<constraint_descriptor>);

    using constraint_registry = containers::descriptor_registry<constraint_descriptor>;

    // ============================================================================
    // make_builtin_constraint_registry — seeded with all builtin mappings
    // ============================================================================

    [[nodiscard]] inline constraint_registry make_builtin_constraint_registry() {
        constraint_registry reg;

        auto add = [&](constraint_kind kind, solver_class target, theory_mask theory,
                       std::uint8_t cost, std::string_view sym) {
            constraint_descriptor d;
            d.stable_id = static_cast<std::uint32_t>(kind);
            d.name_hash = containers::desc_name_hash(sym);
            d.category = target; // group by solver class
            d.kind = kind;
            d.target = target;
            d.theory = theory;
            d.cost_hint = cost;
            d.symbol = sym;
            reg.register_desc(d);
        };

        // Unification backend
        add(constraint_kind::same_type, solver_class::unify, kTheoryEquality, 0, "same_type");
        add(constraint_kind::convertible, solver_class::unify, kTheoryEquality, 2, "convertible");
        add(constraint_kind::subtype, solver_class::unify, kTheoryEquality, 3, "subtype");

        // EasyRules forward-chaining
        add(constraint_kind::implements, solver_class::rule, kTheoryEquality, 5, "implements");
        add(constraint_kind::requires_cap, solver_class::rule, kTheoryEquality, 5, "requires_cap");

        // LiteGraph Tarjan SCC
        add(constraint_kind::same_rank, solver_class::graph, kTheoryEquality, 10, "same_rank");
        add(constraint_kind::broadcastable, solver_class::graph, kTheoryEquality, 10, "broadcastable");
        add(constraint_kind::compatible, solver_class::graph, kTheoryEquality, 10, "compatible");

        // EGraph saturation  (constraint_kind::equivalent = 1000, first ext-band id)
        {
            constexpr auto kEquivalent = static_cast<constraint_kind>(
                kConstraintKindExtensionBase + 0);
            add(kEquivalent, solver_class::egraph, kTheoryEquality, 20, "equivalent");
        }

        // SMT / Tarka bridge (user + higher ext-band)
        add(constraint_kind::user, solver_class::smt, kTheoryArithmetic | kTheoryEquality, 50, "user");

        // --------------------------------------------------------------------
        // Semantic-optimization kinds (ext band +20..+26). Each is seeded here
        // for a cheap fast-path solver class; anything the fast path can't decide
        // falls through to the SMT band (ext-band kinds route to the Tarka bridge
        // with zero extra solver code). Kind constants are DEFINED in their owning
        // headers (single source of truth); the registry only routes them. The
        // offsets are mirrored here to avoid a registry→phase-header dependency.
        // --------------------------------------------------------------------
        auto ext = [](int off) {
            return static_cast<constraint_kind>(kConstraintKindExtensionBase + off);
        };
        add(ext(20), solver_class::graph, kTheoryEquality | kTheoryArrays, 12, "disjoint"); // alias.hpp
        add(ext(21), solver_class::rule, kTheoryEquality, 6, "effect_subsume"); // effect_row.hpp
        add(ext(22), solver_class::unify, kTheoryEquality | kTheoryArithmetic, 4, "value_eq"); // value_param.hpp
        add(ext(23), solver_class::rule, kTheoryEquality, 6, "transition"); // typestate.hpp
        add(ext(24), solver_class::graph, kTheoryEquality | kTheoryArrays, 12, "no_conflict"); // rw_summary.hpp
        add(ext(25), solver_class::smt, kTheoryArithmetic | kTheoryEquality, 40, "refine_sub"); // refine.hpp
        add(ext(26), solver_class::egraph, kTheoryEquality, 20, "equiv_cert"); // proof_carrying.hpp

        return reg;
    }

    // ============================================================================
    // batch_by_class — partition constraints into per-solver-class buckets
    // ============================================================================

    using solver_bucket = std::vector<constraint>;
    using solver_buckets = std::array<solver_bucket, kNumSolverClasses>;

    [[nodiscard]] inline solver_buckets batch_by_class(
        std::span<const constraint> batch,
        const constraint_registry& reg) {
        solver_buckets buckets;
        for (const constraint& c : batch) {
            const constraint_descriptor* desc = reg.find(static_cast<std::uint32_t>(c.kind));
            solver_class cls = desc ? desc->target : solver_class::smt; // unknown → smt
            buckets[static_cast<std::size_t>(cls)].push_back(c);
        }
        return buckets;
    }

    // ============================================================================
    // solve_batch — cross-class fixpoint solver
    //
    // Executes buckets in dependency order: unify → rule → graph → egraph → smt.
    // Repeats until no bucket produces new bindings (fixpoint).
    // Empty buckets are skipped entirely; SMT branch skipped if no smt constraints.
    //
    // Solver must be callable as: solver.solve(span<const constraint>, solve_context)
    // Returns solve_result with merged diagnostics and substitution.
    // ============================================================================

    template <class Solver>
    [[nodiscard]] solve_result solve_batch(
        std::span<const constraint> all_constraints,
        solve_context ctx,
        const constraint_registry& reg,
        Solver& solver) {
        solver_buckets buckets = batch_by_class(all_constraints, reg);

        solve_result accum;
        bool changed = true;

        while (changed) {
            changed = false;

            for (std::size_t cls = 0; cls < kNumSolverClasses; ++cls) {
                auto& bucket = buckets[cls];
                if (bucket.empty()) continue;

                const std::size_t prev_subst_size = accum.substitution.size();
                solve_result r = solver.solve(
                    std::span<const constraint>(bucket.data(), bucket.size()), ctx);

                for (auto& br : r.substitution) accum.substitution.push_back(br);
                accum.diagnostics.insert(
                    accum.diagnostics.end(),
                    r.diagnostics.begin(),
                    r.diagnostics.end());

                if (r.status == solve_status::unsatisfiable) {
                    accum.status = solve_status::unsatisfiable;
                    return accum;
                }

                accum.status = join_status(accum.status, r.status);

                if (accum.substitution.size() > prev_subst_size) changed = true;
            }
        }

        return accum;
    }
} // namespace vakya::types
