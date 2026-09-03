#pragma once

// =============================================================================
// containers/handle/generational_handle.hpp — generic generational handle
//
// A stale-safe, phantom-typed index pair.  Every store (registries, code
// versions, roots, profiling counters) needs a handle primitive that detects
// use after erase without RTTI or heap allocation.  This header provides the
// shared primitive; per-store generation bumping lives in the store (slot_map).
//
// Tag gives phantom-type safety: a handle<backend_tag> never compares equal to
// a handle<code_version_tag>, even if their numeric fields happen to match.
// Tag need not be defined — a forward declaration is sufficient.
//
// Zero Lithe dependency.  Trivially copyable.  constexpr-constructible.
// No virtual, no macros.  Header-only C++23.
// =============================================================================

#include <concepts>
#include <cstdint>
#include <functional>

namespace containers {
    // =========================================================================
    // generational_handle<Tag, Index>
    // =========================================================================

    template <class Tag, std::unsigned_integral Index = std::uint32_t>
    struct generational_handle {
        using tag_type = Tag;
        using index_type = Index;

        Index index = 0;
        Index generation = 0;

        // The null handle: index 0, generation 0.  No live slot should ever
        // carry index 0 so null is distinguishable without a separate flag.
        [[nodiscard]] constexpr bool is_null() const noexcept {
            return index == 0 && generation == 0;
        }

        [[nodiscard]] constexpr bool operator==(const generational_handle&) const noexcept
        = default;
        [[nodiscard]] constexpr bool operator!=(const generational_handle&) const noexcept
        = default;

        [[nodiscard]] static constexpr generational_handle null() noexcept {
            return generational_handle{};
        }
    };

    static_assert(std::is_trivially_copyable_v<generational_handle<struct _dummy_tag>>);

    // Convenience alias: the null sentinel for any handle type.
    template <class Tag, std::unsigned_integral Index = std::uint32_t>
    inline constexpr generational_handle<Tag, Index> null_handle{};
} // namespace containers

namespace pebble::containers {
    using ::containers::generational_handle;
    template <class Tag, std::unsigned_integral Index = std::uint32_t>
    inline constexpr generational_handle<Tag, Index> null_handle = ::containers::null_handle<Tag, Index>;
} // namespace pebble::containers

// =============================================================================
// std::hash specialization — enables use as an unordered_map / unordered_set key
// =============================================================================
template <class Tag, std::unsigned_integral Index>
struct std::hash<containers::generational_handle<Tag, Index>> {
    [[nodiscard]] constexpr std::size_t
    operator()(const containers::generational_handle<Tag, Index>& h) const noexcept {
        // FNV-1a–style fold of index and generation into one word.
        const auto a = static_cast<std::size_t>(h.index);
        const auto b = static_cast<std::size_t>(h.generation);
        constexpr std::size_t kPrime = 0x9e3779b97f4a7c15ULL;
        return (a ^ (b * kPrime)) * kPrime;
    }
}; // namespace std
