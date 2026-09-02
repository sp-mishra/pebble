#pragma once

// =============================================================================
// vakya/types/effect.hpp — effect descriptor + mask (opt-in)
//
// C++23, header-only, no virtual, no macros.
// Namespace: vakya::types
//
// Effects become first-class constraints in the reasoning layer. A function type carries an
// effect_mask; calling it emits effect obligations into the constraint batch.
// Simple aggregate checks: rule solver.
// Quantified/path-sensitive obligations: Tarka SMT solver.
//
// effect_descriptor satisfies containers::RegistrableDescriptor.
// effect_mask = uint64_t bitmask (same design as capability_mask).
//
// Dependencies: vakya/types/type_registry.hpp, containers/descriptor_registry.hpp
// =============================================================================

#include "vakya/types/type_registry.hpp"
#include "containers/descriptor_registry.hpp"

#include <cstdint>
#include <string_view>

namespace vakya::types {
    // ============================================================================
    // effect_category — category enum for effect registry entries
    // ============================================================================

    enum class effect_category : std::uint32_t {
        builtin = 0,
        extension = 1,
    };

    // ============================================================================
    // effect_descriptor — registrable effect metadata
    // ============================================================================

    struct effect_descriptor {
        std::uint32_t stable_id = 0;
        std::uint64_t name_hash = 0;
        effect_category category = effect_category::builtin;

        std::string_view symbol{};
        std::uint64_t bit_mask = 0; // single-bit mask in effect_mask
    };

    static_assert(containers::RegistrableDescriptor<effect_descriptor>);

    using effect_registry = containers::descriptor_registry<effect_descriptor>;

    // ============================================================================
    // Builtin effect stable_ids (< kExtensionIdBase)
    // ============================================================================

    inline constexpr std::uint32_t kEffectFileSystem = 1u;
    inline constexpr std::uint32_t kEffectMemory = 2u;
    inline constexpr std::uint32_t kEffectIO = 3u;
    inline constexpr std::uint32_t kEffectNetwork = 4u;
    inline constexpr std::uint32_t kEffectException = 5u;

    inline constexpr std::uint32_t kEffectExtensionBase = containers::kDescRegistryExtensionBase;

    // ============================================================================
    // effect_mask — bitmask of effects on a function type
    // ============================================================================

    using effect_mask = std::uint64_t;

    inline constexpr effect_mask kEffectMaskFileSystem = 1ULL << 0;
    inline constexpr effect_mask kEffectMaskMemory = 1ULL << 1;
    inline constexpr effect_mask kEffectMaskIO = 1ULL << 2;
    inline constexpr effect_mask kEffectMaskNetwork = 1ULL << 3;
    inline constexpr effect_mask kEffectMaskException = 1ULL << 4;

    [[nodiscard]] constexpr bool has_effect(effect_mask mask, effect_mask eff) noexcept {
        return (mask & eff) != 0;
    }

    [[nodiscard]] constexpr effect_mask add_effect(effect_mask mask, effect_mask eff) noexcept {
        return mask | eff;
    }

    // ============================================================================
    // make_builtin_effect_registry
    // ============================================================================

    [[nodiscard]] inline effect_registry make_builtin_effect_registry() {
        effect_registry reg;

        auto add = [&](std::uint32_t id, std::string_view sym, effect_mask bit) {
            effect_descriptor d;
            d.stable_id = id;
            d.name_hash = containers::desc_name_hash(sym);
            d.category = effect_category::builtin;
            d.symbol = sym;
            d.bit_mask = bit;
            reg.register_desc(d);
        };

        add(kEffectFileSystem, "FileSystem", kEffectMaskFileSystem);
        add(kEffectMemory, "Memory", kEffectMaskMemory);
        add(kEffectIO, "IO", kEffectMaskIO);
        add(kEffectNetwork, "Network", kEffectMaskNetwork);
        add(kEffectException, "Exception", kEffectMaskException);

        return reg;
    }
} // namespace vakya::types
