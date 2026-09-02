#pragma once

// =============================================================================
// vakya/alias.hpp — disjointness constraints + may_alias query (opt-in)
//
// C++23, header-only, no virtual, no macros.
// Namespace: vakya::types
//
// Layers a constraint + solver seam over region.hpp:
//   kDisjointKind          — ext-band constraint "regions A and B are disjoint"
//   make_disjoint_constraint(a, b)
//   disjoint_solver        — constraint_solver satisfying the concept:
//                              * concrete disjointness → solved (union_find / syntactic)
//                              * proven aliased        → unsatisfiable
//                              * symbolic index        → deferred (routed to SMT band)
//   may_alias(arena, a, b) — O(α(n)) conservative query for consumers.
//
// disjoint routes through the graph solver class (registered in
// constraint_registry) for the cheap root check; anything it can't decide falls
// through to the SMT band (kind >= kConstraintKindExtensionBase already routes to
// the Tarka bridge with zero solver code). With no_smt_backend the residual is
// simply deferred — never a spurious failure.
//
// Dependencies: vakya/types/region.hpp, vakya/constraints.hpp
// =============================================================================

#include "vakya/types/region.hpp"
#include "vakya/constraints.hpp"

#include <cstdint>
#include <span>

namespace vakya::types {
    // ============================================================================
    // kDisjointKind — ext-band constraint kind (see docs routing table).
    // The reasoning layer consumed +0 (equivalent), +1/+2/+3 (refine/prove/arith), +10/+11/+12
    // (shape dim eq/pos/matmul). Disjointness claims +20.. to avoid all collisions.
    // ============================================================================

    inline constexpr constraint_kind kDisjointKind =
        static_cast<constraint_kind>(kConstraintKindExtensionBase + 20);

    // ============================================================================
    // make_disjoint_constraint — operands carry region handles packed into the
    // type_ref-shaped operand slots is NOT possible (operands are type_refs), so
    // the two region handles travel in the payload: low 32 bits = a.index,
    // high 32 bits = b.index. The generation is validated by the solver via arena
    // lookup, so index packing is sufficient for the fast path.
    // ============================================================================

    [[nodiscard]] inline constraint
    make_disjoint_constraint(region_ref a, region_ref b) noexcept {
        constraint c;
        c.kind = kDisjointKind;
        c.payload = (static_cast<std::uint64_t>(a.index) & 0xFFFFFFFFULL) |
            (static_cast<std::uint64_t>(b.index) << 32);
        return c;
    }

    // Recover the two region indices packed into a disjoint constraint payload.
    [[nodiscard]] inline std::pair<std::uint32_t, std::uint32_t>
    unpack_disjoint(const constraint& c) noexcept {
        return {
            static_cast<std::uint32_t>(c.payload & 0xFFFFFFFFULL),
            static_cast<std::uint32_t>(c.payload >> 32)
        };
    }

    // ============================================================================
    // may_alias — conservative aliasing query for consumers.
    //
    // Returns false ONLY when disjointness is provable (distinct roots, or distinct
    // concrete selectors of a shared parent). Proven-aliased or undecidable → true
    // (the safe answer). This is the O(α(n)) fact Vakya writes once so Crank/Pravaha
    // need not re-derive it.
    // ============================================================================

    [[nodiscard]] inline bool
    may_alias(region_arena& arena, region_ref a, region_ref b) noexcept {
        if (a.is_null() || b.is_null()) return true; // unknown region → assume aliasing
        if (arena.aliases(a, b)) return true; // proven same class
        if (regions_syntactically_disjoint(arena, a, b)) return false;
        return true;
    }

    // ============================================================================
    // disjoint_solver — constraint_solver for kDisjointKind over a region_arena.
    //
    // Holds a non-owning pointer to the region_arena so it can resolve the packed
    // indices to live region handles. handles() gates dispatch so the composite
    // hot path skips it entirely for non-disjoint kinds (zero cost when unused).
    // ============================================================================

    class disjoint_solver {
    public:
        disjoint_solver() = default;
        explicit disjoint_solver(region_arena& arena) noexcept : arena_(&arena) {}

        [[nodiscard]] bool handles(constraint_kind k) const noexcept {
            return k == kDisjointKind;
        }

        [[nodiscard]] solve_result solve(std::span<const constraint> batch,
                                         solve_context /*ctx*/) {
            solve_result r;
            if (!arena_) {
                r.status = solve_status::deferred;
                return r;
            }

            for (const constraint& c : batch) {
                if (!handles(c.kind)) continue;
                const auto [ai, bi] = unpack_disjoint(c);
                // Rebuild handles by scanning the arena for matching live index.
                // region handles are dense 1-based; generation is looked up.
                const region_ref a = resolve(ai);
                const region_ref b = resolve(bi);
                if (a.is_null() || b.is_null()) {
                    r.status = join_status(r.status, solve_status::deferred);
                    continue;
                }
                // Proven aliasing is the stronger fact: check it before the
                // syntactic fast path, since unite_alias() can merge two regions
                // that still have distinct roots (e.g. an assignment/borrow), and
                // regions_syntactically_disjoint would otherwise report them
                // disjoint on root_id alone.
                if (arena_->aliases(a, b)) {
                    r.status = solve_status::unsatisfiable;
                    r.diagnostics.push_back(
                        solver_diagnostic{
                            "regions proven aliased; disjointness refuted",
                            constraint_ref{}
                        });
                    return r;
                }
                else if (regions_syntactically_disjoint(*arena_, a, b)) {
                    r.status = join_status(r.status, solve_status::solved);
                }
                else {
                    // Can't decide (symbolic index / cross-projection) → defer to SMT.
                    r.status = join_status(r.status, solve_status::deferred);
                }
            }
            return r;
        }

    private:
        // Resolve a live region handle from a bare index. region handles are minted
        // in a slot_map (1-based, generation starts at 1 and is never bumped here —
        // region_arena never erases), so generation is 1 for every live region.
        [[nodiscard]] region_ref resolve(std::uint32_t idx) const noexcept {
            if (idx == 0) return region_ref{};
            const region_ref candidate{idx, 1};
            return arena_->get(candidate) ? candidate : region_ref{};
        }

        region_arena* arena_ = nullptr;
    };

    static_assert(constraint_solver<disjoint_solver>);
} // namespace vakya::types
