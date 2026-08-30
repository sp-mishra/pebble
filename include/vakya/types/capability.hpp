#pragma once

// =============================================================================
// vakya/types/capability.hpp — capability descriptor + mask (opt-in)
//
// C++23, header-only, no virtual, no macros.
// Namespace: vakya::types
//
// Capabilities become first-class constraints in the reasoning layer.
// requires_capability(T, Cap) lowers to constraint_kind::requires_cap.
// Simple membership: rule solver. Path-sensitive: Tarka SMT solver.
//
// capability_descriptor satisfies containers::RegistrableDescriptor.
// capability_mask = uint64_t bitmask (builtin caps fit in low 32 bits;
//   ext caps >= kCapabilityExtensionBase use higher bits or a SparseSet overlay).
//
// Dependencies: vakya/types/type_registry.hpp, containers/descriptor_registry.hpp
// =============================================================================

#include "vakya/types/type_registry.hpp"
#include "containers/descriptor_registry.hpp"

#include <cstdint>
#include <string_view>

namespace vakya::types {
    // ============================================================================
    // capability_category — category enum for capability registry entries
    // ============================================================================

    enum class capability_category : std::uint32_t {
        builtin = 0,
        extension = 1,
    };

    // ============================================================================
    // capability_descriptor — registrable capability metadata
    // ============================================================================

    struct capability_descriptor {
        std::uint32_t stable_id = 0;
        std::uint64_t name_hash = 0;
        capability_category category = capability_category::builtin;

        std::string_view symbol{};
        std::uint64_t bit_mask = 0; // single-bit mask in capability_mask
    };

    static_assert(containers::RegistrableDescriptor<capability_descriptor>);

    using capability_registry = containers::descriptor_registry<capability_descriptor>;

    // ============================================================================
    // Builtin capability stable_ids (< kExtensionIdBase)
    // ============================================================================

    inline constexpr std::uint32_t kCapRead = 1u;
    inline constexpr std::uint32_t kCapWrite = 2u;
    inline constexpr std::uint32_t kCapNetwork = 3u;
    inline constexpr std::uint32_t kCapExecute = 4u;
    inline constexpr std::uint32_t kCapAllocate = 5u;

    inline constexpr std::uint32_t kCapabilityExtensionBase = containers::kDescRegistryExtensionBase;

    // ============================================================================
    // capability_mask — bitmask of capabilities on a type-registry entry
    //
    // Builtin caps use bits 0-4; extension caps may use a descriptor lookup.
    // ============================================================================

    using capability_mask = std::uint64_t;

    inline constexpr capability_mask kCapMaskRead = 1ULL << 0;
    inline constexpr capability_mask kCapMaskWrite = 1ULL << 1;
    inline constexpr capability_mask kCapMaskNetwork = 1ULL << 2;
    inline constexpr capability_mask kCapMaskExecute = 1ULL << 3;
    inline constexpr capability_mask kCapMaskAllocate = 1ULL << 4;

    [[nodiscard]] constexpr bool has_capability(capability_mask mask, capability_mask cap) noexcept {
        return (mask & cap) != 0;
    }

    [[nodiscard]] constexpr capability_mask add_capability(capability_mask mask,
                                                           capability_mask cap) noexcept {
        return mask | cap;
    }

    // ============================================================================
    // make_builtin_capability_registry
    // ============================================================================

    [[nodiscard]] inline capability_registry make_builtin_capability_registry() {
        capability_registry reg;

        auto add = [&](std::uint32_t id, std::string_view sym, capability_mask bit) {
            capability_descriptor d;
            d.stable_id = id;
            d.name_hash = containers::desc_name_hash(sym);
            d.category = capability_category::builtin;
            d.symbol = sym;
            d.bit_mask = bit;
            reg.register_desc(d);
        };

        add(kCapRead, "Read", kCapMaskRead);
        add(kCapWrite, "Write", kCapMaskWrite);
        add(kCapNetwork, "Network", kCapMaskNetwork);
        add(kCapExecute, "Execute", kCapMaskExecute);
        add(kCapAllocate, "Allocate", kCapMaskAllocate);

        return reg;
    }
} // namespace vakya::types
