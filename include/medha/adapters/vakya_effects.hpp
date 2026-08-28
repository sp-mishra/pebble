#pragma once
// =============================================================================
// medha/adapters/vakya_effects.hpp — optional bridge between medha::effect_mask
// and vakya::types::effect_mask.
//
// C++23, header-only, no virtual, no macros.
//
// Medha core uses its own effect_mask = uint64_t.
// This adapter provides bidirectional conversion when Vakya is present.
// Bit assignments are compatible by construction.
// =============================================================================

#include "medha/effects.hpp"

#if __has_include("vakya/types/effect.hpp")
#  include "vakya/types/effect.hpp"
#  define MEDHA_HAS_VAKYA_EFFECTS 1
#endif

namespace medha::adapters::vakya_effects {
#ifdef MEDHA_HAS_VAKYA_EFFECTS

    // Convert medha::effect_mask → vakya::types::effect_mask
    // Bits are compatible when Medha uses the Vakya extension band convention.
    [[nodiscard]] inline vakya::types::effect_mask
    to_vakya(medha::effect_mask m) noexcept {
        return static_cast<vakya::types::effect_mask>(m);
    }

    // Convert vakya::types::effect_mask → medha::effect_mask
    [[nodiscard]] inline medha::effect_mask
    from_vakya(vakya::types::effect_mask m) noexcept {
        return static_cast<medha::effect_mask>(m);
    }

#endif  // MEDHA_HAS_VAKYA_EFFECTS
} // namespace medha::adapters::vakya_effects
