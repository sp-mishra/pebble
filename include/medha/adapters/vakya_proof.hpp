#pragma once
// =============================================================================
// medha/adapters/vakya_proof.hpp — optional bridge between medha::proof_status
// and vakya::types::proof_status.
//
// C++23, header-only, no virtual, no macros.
//
// Medha owns proof_status {proven, refuted, unknown, unsupported, timeout, deferred}.
// Vakya owns proof_status {unknown, proven, refuted, deferred}.
// Mapping: refuted→refuted; unknown→unknown; deferred→deferred|unsupported|timeout.
// =============================================================================

#include "medha/commit.hpp"

#if __has_include("vakya/analysis_store.hpp")
#  include "vakya/analysis_store.hpp"
#  define MEDHA_HAS_VAKYA_PROOF 1
#endif

namespace medha::adapters::vakya_proof {
#ifdef MEDHA_HAS_VAKYA_PROOF

    // Map vakya::types::proof_status → medha::proof_status
    [[nodiscard]] inline medha::proof_status
    from_vakya(vakya::types::proof_status vs) noexcept {
        switch (vs) {
        case vakya::types::proof_status::proven: return medha::proof_status::proven;
        case vakya::types::proof_status::refuted: return medha::proof_status::refuted;
        case vakya::types::proof_status::unknown: return medha::proof_status::unknown;
        case vakya::types::proof_status::deferred: return medha::proof_status::deferred;
        }
        return medha::proof_status::unknown;
    }

    // Map medha::proof_status → vakya::types::proof_status
    // unsupported and timeout collapse to deferred (Vakya has no equivalent).
    [[nodiscard]] inline vakya::types::proof_status
    to_vakya(medha::proof_status ps) noexcept {
        switch (ps) {
        case medha::proof_status::proven: return vakya::types::proof_status::proven;
        case medha::proof_status::refuted: return vakya::types::proof_status::refuted;
        case medha::proof_status::unknown: return vakya::types::proof_status::unknown;
        case medha::proof_status::unsupported: return vakya::types::proof_status::deferred;
        case medha::proof_status::timeout: return vakya::types::proof_status::deferred;
        case medha::proof_status::deferred: return vakya::types::proof_status::deferred;
        }
        return vakya::types::proof_status::unknown;
    }

#endif  // MEDHA_HAS_VAKYA_PROOF
} // namespace medha::adapters::vakya_proof
