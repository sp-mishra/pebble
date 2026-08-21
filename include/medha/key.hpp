#pragma once
// =============================================================================
// medha/key.hpp — canonical_key + canonicalize CPO + read_kind
//
// C++23, header-only, no virtual, no macros.
//
// canonical_key: deterministic identity for a (resource, key) pair.
// Canonicalization CPO: ADL `canonicalize(const R&, const key_type&) → canonical_key`.
// Default: FNV-1a over trivially-copyable key bytes.
// Resources override for structured keys.
//
// read_kind: classifies what the transaction read (point vs range/predicate).
//   serializable isolation guarantees are defined only for point reads.
//   range and predicate reads require resource-specific phantom-prevention.
//
// Deterministic canonicalization is required for:
//   - lock ordering (prevents deadlock, §20.1)
//   - proof reproducibility (§23)
// =============================================================================

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <type_traits>

namespace medha {
    // ============================================================================
    // read_kind — classifies the read scope for conflict detection (§20.1)
    //
    // point     — exact key lookup; covered by Medha's per-key version check
    // range     — ordered scan over [lo, hi]; requires resource range-lock or
    //             resource-level phantom prevention (not provided by Medha core)
    // predicate — arbitrary filter scan; same phantom caveat as range
    // index     — secondary-index lookup; semantics defined by resource
    //
    // Serializable isolation in Medha core covers point reads only.
    // For range/predicate/index: document the phantom-prevention mechanism
    // in the resource's tx_validate CPO or use a resource-provided protocol
    // (commit_capability::ssi or atomic_multi_key_within_resource).
    // ============================================================================

    enum class read_kind : std::uint8_t {
        point = 0, // single key — covered by Medha version check
        range = 1, // [lo, hi] scan — phantom prevention is resource responsibility
        predicate = 2, // arbitrary filter — phantom prevention is resource responsibility
        index = 3, // secondary index — semantics resource-defined
    };

    // ============================================================================
    // resource_id — stable phantom-typed generational handle (minted by slot_map)
    // Defined as an opaque struct here; the slot_map integration uses
    // containers::generational_handle<resource_tag> in context.hpp.
    // ============================================================================

    struct resource_tag {};

    struct resource_id {
        std::uint32_t index = 0;
        std::uint32_t generation = 0;

        [[nodiscard]] constexpr bool valid() const noexcept { return generation != 0; }
        [[nodiscard]] constexpr bool operator==(const resource_id&) const noexcept = default;

        [[nodiscard]] constexpr bool operator<(const resource_id& o) const noexcept {
            if (index != o.index) return index < o.index;
            return generation < o.generation;
        }
    };

    // ============================================================================
    // canonical_key
    // ============================================================================

    struct canonical_key {
        resource_id resource{};
        std::uint64_t key_hash = 0;
        std::span<const std::byte> stable_key_bytes{}; // borrowed; lifetime = attempt

        [[nodiscard]] constexpr bool operator==(const canonical_key& o) const noexcept {
            return resource == o.resource && key_hash == o.key_hash;
        }

        // Deterministic total order: (resource_id, key_hash).
        [[nodiscard]] constexpr bool operator<(const canonical_key& o) const noexcept {
            if (resource < o.resource) return true;
            if (o.resource < resource) return false;
            return key_hash < o.key_hash;
        }
    };

    // ============================================================================
    // FNV-1a helpers (deterministic, constexpr-capable)
    // ============================================================================

    namespace detail {
        inline constexpr std::uint64_t kFnvPrime = 0x00000100'000001B3ULL;
        inline constexpr std::uint64_t kFnvOffset = 0xcbf29ce4'84222325ULL;

        [[nodiscard]] constexpr std::uint64_t fnv1a(const std::byte* data, std::size_t n) noexcept {
            std::uint64_t h = kFnvOffset;
            for (std::size_t i = 0; i < n; ++i) {
                h ^= static_cast<std::uint64_t>(data[i]);
                h *= kFnvPrime;
            }
            return h;
        }
    } // namespace detail

    // ============================================================================
    // canonicalize CPO — deterministic key → canonical_key
    //
    // Users override via ADL: canonicalize(const R&, const K&) → canonical_key
    // Default: FNV-1a over object bytes of trivially-copyable K.
    // ============================================================================

    namespace cpo {
        struct canonicalize_fn {
            template <class R, class K>
            [[nodiscard]] canonical_key operator()(resource_id rid,
                                                   const R&,
                                                   const K& key) const noexcept
                requires std::is_trivially_copyable_v<K> {
                const auto* bytes = reinterpret_cast<const std::byte*>(std::addressof(key));
                const auto hash = detail::fnv1a(bytes, sizeof(K));
                return canonical_key{
                    .resource = rid,
                    .key_hash = hash,
                    .stable_key_bytes = std::span<const std::byte>(bytes, sizeof(K)),
                };
            }
        };
    } // namespace cpo

    inline constexpr cpo::canonicalize_fn canonicalize{};

    // ============================================================================
    // range_key — identifies a [lo, hi) ordered scan for the read set
    // ============================================================================

    struct range_key {
        resource_id resource{};
        std::uint64_t lo_hash = 0; // FNV-1a of lower bound key
        std::uint64_t hi_hash = 0; // FNV-1a of upper bound key (exclusive)
    };

    // ============================================================================
    // predicate_key — identifies an arbitrary-filter scan for the read set
    // ============================================================================

    struct predicate_key {
        resource_id resource{};
        std::uint64_t predicate_hash = 0; // stable hash of the predicate expression
        std::string_view description{}; // human-readable; borrowed lifetime = attempt
    };
} // namespace medha
