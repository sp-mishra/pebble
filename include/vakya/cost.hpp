#pragma once

// =============================================================================
// vakya/cost.hpp — compile-time cost lattice (opt-in)
//
// C++23, header-only, no virtual, no macros.
// Namespace: vakya::types
//
// A join-semilattice over cost_class (unknown ⊑ tiny ⊑ small ⊑ moderate ⊑ heavy).
// cost_join is the ⊔ (max) monoid used to fold child costs into a parent: a node's
// cost is at least the cost of its most expensive sub-computation. This lets Crank
// pick inlining / unrolling / offload thresholds off a PROVEN band instead of
// re-estimating from the AST.
//
// synthesize_cost maps a static work magnitude (element count of the value/shape,
// e.g. from value_param synthesis) to a band under a cost_policy. Bands are
// thresholds carried in the policy (seeded from analyze_options) — NOT hardcoded
// constants baked into the logic. A symbolic / unknown magnitude yields unknown,
// never a guessed band.
//
// Dependencies: vakya/types/opt_handles.hpp, vakya/types/shape.hpp
// =============================================================================

#include "vakya/types/opt_handles.hpp"
#include "vakya/types/shape.hpp"

#include <cstdint>

namespace vakya::types {
    // ============================================================================
    // cost_join — ⊔ over the totally-ordered cost lattice (max of the two bands).
    // unknown is the bottom element; joining anything with unknown keeps the other
    // (a known band dominates "no information").
    // ============================================================================

    [[nodiscard]] inline constexpr cost_class
    cost_join(cost_class a, cost_class b) noexcept {
        if (a == cost_class::unknown) return b;
        if (b == cost_class::unknown) return a;
        return static_cast<std::uint8_t>(a) >= static_cast<std::uint8_t>(b) ? a : b;
    }

    // ============================================================================
    // cost_policy — element-count thresholds for each band. Defaults are portable,
    // documented starting points; callers override via analyze_options. A magnitude
    // strictly below `tiny_below` is tiny, below `small_below` is small, etc.
    // ============================================================================

    struct cost_policy {
        std::uint64_t tiny_below = 8;        // < 8 elements → tiny
        std::uint64_t small_below = 256;     // < 256 → small
        std::uint64_t moderate_below = 65536; // < 64Ki → moderate; ≥ → heavy
    };

    // ============================================================================
    // synthesize_cost — map a static element/work count to a band.
    //   0                        → unknown  (no magnitude information)
    //   [1, tiny_below)          → tiny
    //   [tiny_below, small_below)→ small
    //   [small_below, moderate_below) → moderate
    //   [moderate_below, ∞)      → heavy
    // Callers pass 0 for a symbolic extent so it stays unknown, never a guess.
    // ============================================================================

    [[nodiscard]] inline constexpr cost_class
    synthesize_cost(std::uint64_t static_work, const cost_policy& policy = {}) noexcept {
        if (static_work == 0) return cost_class::unknown;
        if (static_work < policy.tiny_below) return cost_class::tiny;
        if (static_work < policy.small_below) return cost_class::small;
        if (static_work < policy.moderate_below) return cost_class::moderate;
        return cost_class::heavy;
    }

    // ============================================================================
    // synthesize_shape_cost — element count of a fully-static shape → band.
    //
    // Multiplies the shape's dimension extents (read from interned primitive dims).
    // Any non-literal dim makes the product symbolic → unknown. Rank-0 (scalar) is
    // work=1 → tiny. Overflow saturates to the heavy band.
    // ============================================================================

    [[nodiscard]] inline cost_class
    synthesize_shape_cost(const type_arena& arena, shape_ref s,
                          const cost_policy& policy = {}) noexcept {
        const type_node* sn = arena.get(s);
        if (!sn) return cost_class::unknown;

        std::uint64_t work = 1; // scalar shape (rank 0) → 1 element
        for (const type_ref& dim : sn->children) {
            const type_node* dn = arena.get(dim);
            if (!dn) return cost_class::unknown;
            // A dim is a static extent only if it carries a literal payload and no
            // type variable. Otherwise the total work is symbolic.
            if (dn->var_id != kInvalidTypeVarId) return cost_class::unknown;
            const std::uint64_t extent = dn->payload_hash;
            if (extent == 0) return cost_class::unknown; // undecoded / symbolic dim
            // Saturating multiply.
            if (work > (0xFFFFFFFFFFFFFFFFULL / extent)) { work = 0xFFFFFFFFFFFFFFFFULL; break; }
            work *= extent;
        }
        return synthesize_cost(work, policy);
    }
} // namespace vakya::types
