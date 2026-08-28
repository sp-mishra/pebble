#pragma once
// =============================================================================
// medha/identity.hpp — transaction_id, attempt_id, idempotency_token (§5b.1)
//
// C++23, header-only, no virtual, no macros.
//
// Stable identities used locally for telemetry, replay, and AOT metadata.
// Future distributed adapters must carry these in every remote message.
// =============================================================================

#include <cstdint>

namespace medha {
    // ============================================================================
    // transaction_id — 128-bit stable id
    // ============================================================================

    struct transaction_id {
        std::uint64_t hi = 0;
        std::uint64_t lo = 0;

        [[nodiscard]] constexpr bool operator==(const transaction_id&) const noexcept = default;
        [[nodiscard]] constexpr bool valid() const noexcept { return hi != 0 || lo != 0; }
    };

    // ============================================================================
    // attempt_id — transaction_id + attempt counter
    // ============================================================================

    struct attempt_id {
        transaction_id tx{};
        std::uint32_t attempt = 0; // 0-based; matches §20.5 retry loop counter
    };

    // ============================================================================
    // idempotency_token — for idempotent remote retry (§5b.1)
    // ============================================================================

    struct idempotency_token {
        transaction_id tx{};
        std::uint32_t attempt = 0;
        std::uint64_t operation_hash = 0; // structural hash of the staged operation
    };
} // namespace medha
