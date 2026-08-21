#pragma once
// =============================================================================
// medha/fwd.hpp — forward declarations, tx_error, tx_status
//
// C++23, header-only, no virtual, no macros.
// Namespace: medha (alias: namespace tx = medha)
// =============================================================================

#include <cstdint>
#include <string_view>

namespace medha {
    // ============================================================================
    // tx_status — outcome of a commit attempt
    // ============================================================================

    enum class tx_status : std::uint8_t {
        committed = 0,
        aborted = 1,
        conflict = 2,
        retry_exhausted = 3,
        validation_failed = 4,
        serialization_unavailable = 5,
        unsupported_resource = 6,
        unsupported_effect = 7,
        out_of_memory = 8,
        rejected = 9,
        partial_commit = 10, // best_effort: some resources committed, some did not

        // Distribution-ready: only future distributed adapters produce these in v1
        in_doubt = 64, // cannot determine remote commit outcome
        recovery_required = 65, // must resolve via recovery protocol
        remote_timeout = 66,
        participant_failed = 67,

        internal_error = 255,
    };

    // ============================================================================
    // tx_error — error type for std::expected-based fallible APIs
    // ============================================================================

    struct tx_error {
        tx_status status = tx_status::internal_error;
        std::string_view message{}; // borrowed; lifetime = error site

        [[nodiscard]] constexpr bool is_conflict() const noexcept {
            return status == tx_status::conflict;
        }

        [[nodiscard]] constexpr bool is_oom() const noexcept {
            return status == tx_status::out_of_memory;
        }

        [[nodiscard]] constexpr bool is_retriable() const noexcept {
            return status == tx_status::conflict || status == tx_status::validation_failed;
        }
    };

    // Forward declarations
    class commit_report;
    class transaction_context;
    class atomic_scope;

    namespace dsl {
        struct plan;
        struct executable_plan;
        struct bindings;
    } // namespace dsl
} // namespace medha
