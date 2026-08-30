#pragma once

// =============================================================================
// vakya/exec_affinity.hpp — capability-inferred scheduling affinity (opt-in)
//
// C++23, header-only, no virtual, no macros.
// Namespace: vakya::types
//
// synthesize_affinity folds facts Vākya already proved — the effect row, the
// read/write summary, the cost band — into a neutral execution_affinity hint:
//
//   pure        — no effects, empty write set          → freely reorderable / memoizable
//   io_bound    — IO / Network / FileSystem effect      → offload to an async pool
//   sequential  — Exception effect or open effect tail  → keep ordered (may throw / unknown)
//   cpu_bound   — heavy/moderate cost, memory effect     → parallel compute pool
//   unknown     — insufficient facts                     → consumer decides
//
// The mapping is a documented DEFAULT expressed as an affinity_policy, overridable
// by callers (analyze_options seeds it). Vākya emits only the neutral enum; the
// Pravaha/Lithe scheduler adapter that turns io_bound into an actual thread pool
// lives CONSUMER-SIDE (documented, not shipped here) — Vākya keeps no downward dep.
//
// Inputs come straight off analysis_record + the effect_row_arena, so this is a
// read-only fold: no arena mutation, no allocation.
//
// Dependencies: vakya/analysis_store.hpp, vakya/types/effect.hpp,
//               vakya/types/effect_row.hpp
// =============================================================================

#include "vakya/analysis_store.hpp"
#include "vakya/types/effect.hpp"
#include "vakya/types/effect_row.hpp"

#include <cstdint>

namespace vakya::types {
    // ============================================================================
    // affinity_policy — which effects push toward which affinity. Bitmask groups
    // are DEFAULTS (portable, no hardware assumption); callers may re-point them.
    // ============================================================================

    struct affinity_policy {
        // Effects that make a computation io_bound (worth offloading off the compute
        // path). Default: IO + Network + FileSystem.
        effect_mask io_effects =
            kEffectMaskIO | kEffectMaskNetwork | kEffectMaskFileSystem;

        // Effects that force sequential ordering (observable / may unwind).
        // Default: Exception.
        effect_mask sequential_effects = kEffectMaskException;

        // Cost band at/above which a pure-compute node is treated as cpu_bound
        // (worth a parallel dispatch). Default: moderate.
        cost_class cpu_bound_from = cost_class::moderate;
    };

    // ============================================================================
    // synthesize_affinity — fold the proven facts into a scheduling hint.
    //
    // Precedence (most constraining first):
    //   sequential  > io_bound > cpu_bound > pure > unknown
    // An open effect tail (polymorphic ρ) is conservatively sequential: the caller's
    // effects are unknown, so we must not assume reorderability.
    // ============================================================================

    [[nodiscard]] inline execution_affinity
    synthesize_affinity(const analysis_record& rec,
                        const effect_row_arena* rows = nullptr,
                        const affinity_policy& policy = {}) noexcept {
        // Resolve the effective concrete effect mask + openness from the row if
        // present, else fall back to the flat aggregate mask on the record.
        effect_mask concrete = rec.effects;
        bool open_tail = false;
        if (rows && !rec.effect_row.is_null()) {
            if (const effect_row_node* n = rows->get(rec.effect_row)) {
                concrete = n->concrete;
                open_tail = n->is_open();
            }
        }

        // Most constraining: may unwind, or effects unknown → keep ordered.
        if ((concrete & policy.sequential_effects) != 0 || open_tail)
            return execution_affinity::sequential;

        // Offload I/O-shaped work.
        if ((concrete & policy.io_effects) != 0)
            return execution_affinity::io_bound;

        // No externally-observable effect. Heavy pure compute → parallel pool.
        if (rec.cost != cost_class::unknown &&
            static_cast<std::uint8_t>(rec.cost) >=
                static_cast<std::uint8_t>(policy.cpu_bound_from))
            return execution_affinity::cpu_bound;

        // No effects at all and no write set → freely reorderable / memoizable.
        const bool no_effects = concrete == 0;
        const bool no_writes = rec.rw.is_null(); // conservative: unknown rw ≠ pure
        if (no_effects && no_writes) return execution_affinity::pure;

        return execution_affinity::unknown;
    }
} // namespace vakya::types
