#pragma once

// =============================================================================
// vakya/types/opt_handles.hpp — optimization-layer forward handles + scalar enums (opt-in)
//
// C++23, header-only, no virtual, no macros.
// Namespace: vakya::types
//
// The semantic-optimization layer widens analysis_record with side-arena
// handles and small classification enums. To keep analysis_store.hpp's
// dependency surface tiny (it must stay a leaf that new phase headers hang off),
// this header declares ONLY the trivially-copyable handle aliases and the POD
// enums the record needs. All logic lives in the per-phase headers, each of
// which owns the arena that mints its handle type.
//
// Every field the record gains defaults to null / unknown / 0 — zero-cost when
// the corresponding phase is never exercised.
//
// Dependencies: containers/handle/generational_handle.hpp
// =============================================================================

#include "containers/handle/generational_handle.hpp"

#include <cstdint>

namespace vakya::types {
    // ============================================================================
    // Side-arena handle tags (defined here so multiple phase headers + the record
    // agree on one handle type each; the owning arena lives in its phase header).
    // ============================================================================

    struct region_tag {}; // vakya/types/region.hpp
    struct effect_row_tag {}; // vakya/types/effect_row.hpp
    struct rw_summary_tag {}; // vakya/types/rw_summary.hpp

    using region_ref = containers::generational_handle<region_tag, std::uint32_t>;
    using effect_row_ref = containers::generational_handle<effect_row_tag, std::uint32_t>;
    using rw_summary_ref = containers::generational_handle<rw_summary_tag, std::uint32_t>;

    // ============================================================================
    // typestate_id — affine typestate protocol state index (0 == unknown/initial)
    // ============================================================================

    using typestate_id = std::uint32_t;
    inline constexpr typestate_id kNoTypestate = 0;

    // ============================================================================
    // execution_affinity — capability-inferred scheduling hint
    //
    // A neutral fact Vakya proves once; consumers (Pravaha/Lithe) map it to their
    // own scheduling domains via a consumer-side adapter. Vakya keeps no downward
    // dependency on any consumer.
    // ============================================================================

    enum class execution_affinity : std::uint8_t {
        unknown = 0, // not synthesized
        pure = 1, // no effects, no aliasing — freely reorderable / parallel
        cpu_bound = 2, // memory/compute heavy, effect-free
        io_bound = 3, // filesystem / network effects present
        sequential = 4 // ordering-sensitive (exceptions, shared writes)
    };

    // ============================================================================
    // cost_class — compile-time cost lattice band
    //
    // Join semilattice ordered tiny < small < moderate < heavy; unknown is the
    // bottom (absorbing on the "can't decide" side, not on the max monoid).
    // ============================================================================

    enum class cost_class : std::uint8_t {
        unknown = 0, // symbolic / undetermined
        tiny = 1, // leaf / constant
        small = 2, // bounded small loop / few ops
        moderate = 3, // static extent above small band
        heavy = 4 // static extent above moderate band / nested
    };
} // namespace vakya::types
