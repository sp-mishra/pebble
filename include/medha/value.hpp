#pragma once
// =============================================================================
// medha/value.hpp — value capability traits + staging concepts
//
// C++23, header-only, no virtual, no macros.
//
// Drives where a staged write lives:
//   trivially_copyable  → inline in write_set (SmallVector)
//   move_only           → resource must set resource_stages_values = true
//   opaque/serialized   → same staging path, resource owns staging handle
//
// Staging handle: opaque POD returned by the resource's stage CPO.
// =============================================================================

#include <cstdint>

namespace medha {
    // ============================================================================
    // staging_handle — opaque handle returned when resource owns staging
    // ============================================================================

    struct staging_handle {
        std::uint64_t value = 0; // resource-defined opaque id

        [[nodiscard]] constexpr bool valid() const noexcept { return value != 0; }
        [[nodiscard]] constexpr bool operator==(const staging_handle&) const noexcept = default;
    };

    // ============================================================================
    // value_storage_kind — how staged values are stored in the write set
    // ============================================================================

    enum class value_storage_kind : std::uint8_t {
        inline_copy, // trivially copyable; stored as bytes inline
        resource_owned, // non-trivial/move-only; resource owns staging
    };

    // ============================================================================
    // value_storage_traits<R> — derived from resource_traits<R>
    // ============================================================================

    template <class R>
    struct value_storage_traits {
        // Derive from resource_traits without dependency on full resource_traits.hpp
        // (included by caller).
        static constexpr bool trivially_copyable = false;
        static constexpr bool resource_stages = false;
        static constexpr value_storage_kind kind = value_storage_kind::resource_owned;
    };
} // namespace medha
