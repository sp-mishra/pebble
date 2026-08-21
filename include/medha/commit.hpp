#pragma once
// =============================================================================
// medha/commit.hpp — commit_report, proof_status, proof_summary, trace_id
//
// C++23, header-only, no virtual, no macros.
//
// proof_status: Medha-owned canonical enum (no Vakya dependency).
//   proven      — all obligations discharged by solver (sat/unsat as required)
//   refuted     — counterexample found; obligation fails
//   unknown     — solver returned unknown (not proven, not refuted)
//   unsupported — obligation type not supported by the available backend
//   timeout     — solver exceeded time limit
//   deferred    — no SMT backend available; obligation not attempted
//
// Rules (hard):
//   unknown / unsupported / timeout / deferred are NOT treated as proven.
//   refuted → diagnostic or hard error per policy.
//
// Optional Vakya adapter: medha/adapters/vakya_proof.hpp maps
//   medha::proof_status ↔ vakya::types::proof_status.
// =============================================================================

#include "medha/fwd.hpp"

#include <chrono>
#include <cstdint>
#include <optional>

namespace medha {
    // ============================================================================
    // proof_status — Medha canonical (§23, issue 2 of refinement)
    // ============================================================================

    enum class proof_status : std::uint8_t {
        proven = 0, // obligation fully discharged
        refuted = 1, // counterexample found — hard failure per policy
        unknown = 2, // solver returned unknown (not a proof)
        unsupported = 3, // obligation type not supported by backend
        timeout = 4, // solver exceeded resource/time limit
        deferred = 5, // no SMT backend; not attempted
    };

    // ============================================================================
    // proof_summary — aggregated result for a transaction
    // ============================================================================

    struct proof_summary {
        proof_status status = proof_status::deferred;
        std::uint32_t proven = 0;
        std::uint32_t refuted = 0;
        std::uint32_t deferred = 0; // includes unknown + unsupported + timeout + deferred
    };

    // ============================================================================
    // trace_id — thin wrapper (used even without nadi)
    // ============================================================================

    struct trace_id {
        std::uint64_t value = 0;
        [[nodiscard]] constexpr bool valid() const noexcept { return value != 0; }
    };

    // ============================================================================
    // commit_report (§15)
    // ============================================================================

    struct commit_report {
        tx_status status = tx_status::internal_error;
        std::uint32_t attempts = 0;
        std::uint32_t conflicts = 0;
        std::uint32_t resources_touched = 0;
        std::uint32_t reads = 0;
        std::uint32_t writes = 0;
        std::chrono::nanoseconds elapsed{};
        proof_summary proofs{}; // default = deferred
        std::optional<trace_id> telemetry{}; // nadi "medha.tx" channel
    };
} // namespace medha
