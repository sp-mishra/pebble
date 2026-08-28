#pragma once
// =============================================================================
// medha/version.hpp — version_stamp + snapshot_token + distributed_version_stamp
//
// C++23, header-only, no virtual, no macros.
//
// version_stamp: monotone counter + generation guard.
// Validation compares the full stamp (value AND generation).
// Overflow of value → validation failure (conservative default).
//
// snapshot_token (§19.4): stable read snapshot for snapshot isolation.
//   Captures either (a) a resource-provided snapshot handle obtained via the
//   tx_snapshot(resource) CPO at transaction start, or (b) a first-read
//   version_stamp when no snapshot CPO is available.
//   All loads within a snapshot-isolation transaction observe versions ≤
//   snapshot_version; a load whose current version exceeds snapshot_version
//   must re-read from the snapshot (resource responsibility) or fail with
//   conflict.
//
//   Two reads of the same key in one transaction MUST observe the same value;
//   read-your-writes is maintained by checking the write set first.
//
// ABA-resistance contract:
//   version_stamp is ABA-resistant under the stated resource contract:
//   the resource must bump `generation` on slot reuse so that a stale
//   reader observing the same `value` cannot alias a newer slot.
//   Medha's version validation detects this because it compares the
//   full stamp (value, generation), not just the value counter.
//   Correctness depends on the resource honouring the generation bump
//   obligation; Medha cannot enforce resource-side invariants.
// =============================================================================

#include <cstdint>

namespace medha {
    // ============================================================================
    // version_stamp — local monotone version with resource-contract ABA protection
    // ============================================================================

    struct version_stamp {
        std::uint64_t value = 0; // monotone counter
        std::uint32_t generation = 0; // bumped on slot reuse by the resource (ABA contract)

        [[nodiscard]] constexpr bool operator==(const version_stamp&) const noexcept = default;
        [[nodiscard]] constexpr bool operator!=(const version_stamp&) const noexcept = default;

        // Bump version; if value would overflow, bump generation and reset value.
        [[nodiscard]] constexpr version_stamp next() const noexcept {
            if (value == ~std::uint64_t{0}) {
                return version_stamp{0, generation + 1};
            }
            return version_stamp{value + 1, generation};
        }

        // Returns true iff this stamp is strictly "newer" than other
        // (same generation, larger value, or newer generation).
        [[nodiscard]] constexpr bool newer_than(const version_stamp& other) const noexcept {
            if (generation != other.generation) return generation > other.generation;
            return value > other.value;
        }
    };

    // ============================================================================
    // snapshot_token — transaction-start snapshot handle (§19.4)
    //
    // Captures the snapshot boundary for snapshot/serializable isolation.
    // Acquisition strategies (in priority order):
    //   1. Resource provides tx_snapshot(R&) CPO → resource-native handle
    //   2. No CPO → first-read version_stamp is used as the snapshot boundary
    //
    // snapshot_token is attached to transaction_context at construction (for
    // snapshot isolation) or at first read (for first-read-version caching).
    // ============================================================================

    struct snapshot_token {
        version_stamp boundary{}; // snapshot boundary stamp
        bool resource_provided = false; // true if from tx_snapshot CPO
        bool valid = false; // set when snapshot is acquired

        [[nodiscard]] constexpr bool is_valid() const noexcept { return valid; }

        // Returns true iff a given version is within this snapshot (≤ boundary).
        [[nodiscard]] constexpr bool within_snapshot(const version_stamp& vs) const noexcept {
            if (!valid) return true; // no snapshot constraint yet
            return !vs.newer_than(boundary);
        }
    };


    // Not used by Medha core; reserved for future distributed adapters.
    // ============================================================================

    struct distributed_version_stamp {
        std::uint64_t value;
        std::uint32_t generation; // ABA guard (same semantics as local)
        std::uint64_t owner_node;
        std::uint64_t epoch;
        std::uint64_t term; // leader/consensus term

        [[nodiscard]] constexpr bool operator==(const distributed_version_stamp&) const noexcept = default;
    };
} // namespace medha
