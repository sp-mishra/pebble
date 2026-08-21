#pragma once
// =============================================================================
// medha/effects.hpp — effect classes for transaction admission
//
// C++23, header-only, no virtual, no macros.
//
// Medha core owns effect_mask = uint64_t directly — no Vakya dependency.
// Optional adapter: medha/adapters/vakya_effects.hpp maps medha::effect_mask
// ↔ vakya::types::effect_mask for projects that use both.
//
// Effect class lattice:
//   pure          — no externally visible effect
//   reversible    — auto-undone by rollback
//   staged        — in-tx record; published after commit only
//   idempotent    — safe to replay (retry-safe)
//   compensatable — registered compensation action exists
//   irreversible  — cannot rollback or replay
//
// Admission policy:
//   allowed:  pure | reversible | staged | idempotent | compensatable
//   rejected: irreversible, unknown (unset bits in known-effects mask)
// =============================================================================

#include <cstdint>

namespace medha {
    // ============================================================================
    // effect_mask — Medha-owned bitmask; no external dependency
    // ============================================================================

    using effect_mask = std::uint64_t;

    // ============================================================================
    // Effect-class bit positions
    // ============================================================================

    inline constexpr effect_mask kEffectPure = effect_mask{1} << 0;
    inline constexpr effect_mask kEffectReversible = effect_mask{1} << 1;
    inline constexpr effect_mask kEffectStaged = effect_mask{1} << 2;
    inline constexpr effect_mask kEffectIdempotent = effect_mask{1} << 3;
    inline constexpr effect_mask kEffectCompensatable = effect_mask{1} << 4;
    inline constexpr effect_mask kEffectIrreversible = effect_mask{1} << 5;

    // All known effect bits
    inline constexpr effect_mask kEffectKnownMask =
        kEffectPure | kEffectReversible | kEffectStaged |
        kEffectIdempotent | kEffectCompensatable | kEffectIrreversible;

    // All admissible bits (irreversible is NOT admissible)
    inline constexpr effect_mask kEffectAdmissibleMask =
        kEffectPure | kEffectReversible | kEffectStaged |
        kEffectIdempotent | kEffectCompensatable;

    // ============================================================================
    // Admission — which effects are allowed inside a transaction
    // ============================================================================

    [[nodiscard]] constexpr bool effect_admitted_in_transaction(effect_mask m) noexcept {
        // Irreversible is always rejected
        if (m & kEffectIrreversible) return false;
        // Unknown bits (not in known set) are rejected
        if (m & ~kEffectKnownMask) return false;
        return true;
    }

    // ============================================================================
    // effect_descriptor — named effect class record (constexpr)
    // ============================================================================

    struct tx_effect_descriptor {
        const char* name = "";
        effect_mask bits = 0;
    };

    inline constexpr tx_effect_descriptor kPureEffect = {"pure", kEffectPure};
    inline constexpr tx_effect_descriptor kReversibleEffect = {"reversible", kEffectReversible};
    inline constexpr tx_effect_descriptor kStagedEffect = {"staged", kEffectStaged};
    inline constexpr tx_effect_descriptor kIdempotentEffect = {"idempotent", kEffectIdempotent};
    inline constexpr tx_effect_descriptor kCompensatableEffect = {"compensatable", kEffectCompensatable};
    inline constexpr tx_effect_descriptor kIrreversibleEffect = {"irreversible", kEffectIrreversible};
} // namespace medha
