#pragma once
// =============================================================================
// medha/isolation.hpp — isolation protocol selection + requirements
//
// C++23, header-only, no virtual, no macros.
//
// snapshot:     read-stable; write/write conflict prevented; write-skew possible.
// serializable: requires one of:
//   1. read/write-key locking (pessimistic)
//   2. serial validation
//   3. SSI
//   4. resource-provided serializable commit (atomic_multi_key_within_resource)
//   5. deterministic single-thread executor
// commit_capability::none or atomic_single_key → serializable unavailable.
// Missing protocol → MEDHA-SER-011 diagnostic.
//
// Note: atomic_multi_key_within_resource applies to a SINGLE resource only.
// It does NOT guarantee atomicity across multiple resources.
// Cross-resource atomicity requires a coordinator, two-phase commit, or a
// resource-provided atomic batch protocol.
// =============================================================================

#include "medha/options.hpp"
#include "medha/resource_traits.hpp"

namespace medha {
    // ============================================================================
    // serializable_protocol — enumeration of valid protocols (§19)
    // ============================================================================

    enum class serializable_protocol : std::uint8_t {
        none = 0,
        read_write_key_locking = 1, // pessimistic commit_capability
        serial_validation = 2,
        ssi = 3, // serializable snapshot isolation
        resource_provided = 4, // atomic_multi_key_within_resource commit_capability
        deterministic_executor = 5, // deterministic commit_capability
    };

    // ============================================================================
    // supports_serializable — explicit capability predicate (§19.1)
    //
    // Replaces enum-ordering comparisons. Exhaustive switch: no new enum value
    // silently returns true unless this predicate is updated.
    // ============================================================================

    [[nodiscard]] constexpr bool
    supports_serializable(commit_capability cap) noexcept {
        switch (cap) {
        case commit_capability::atomic_multi_key_within_resource:
        case commit_capability::pessimistic:
        case commit_capability::serial_validation:
        case commit_capability::ssi:
        case commit_capability::deterministic:
            return true;
        case commit_capability::none:
        case commit_capability::atomic_single_key:
            return false;
        }
        return false; // unreachable; satisfies -Wreturn-type
    }

    namespace detail {
        // Detect whether a resource_traits specialization defines commit_protocol.
        // Falls back to inferring from legacy bool fields for backward compat.
        template <class R>
        concept has_commit_protocol = requires {
            { resource_traits<R>::commit_protocol } -> std::convertible_to<commit_capability>;
        };

        template <class R>
        [[nodiscard]] constexpr commit_capability infer_commit_capability() noexcept {
            if constexpr (has_commit_protocol<R>) {
                return resource_traits<R>::commit_protocol;
            }
            else {
                // Legacy bool fields: supports_atomic_multi_key_commit infers atomic_multi_key_within_resource
                if constexpr (requires { resource_traits<R>::supports_atomic_multi_key_commit; } &&
                    resource_traits<R>::supports_atomic_multi_key_commit) {
                    return commit_capability::atomic_multi_key_within_resource;
                }
                else if constexpr (requires { resource_traits<R>::thread_safe_commit; } &&
                    resource_traits<R>::thread_safe_commit) {
                    return commit_capability::pessimistic;
                }
                else {
                    return commit_capability::none;
                }
            }
        }
    } // namespace detail

    // ============================================================================
    // select_serializable_protocol<R> — map commit_capability → protocol (§19)
    // ============================================================================

    template <class R>
    [[nodiscard]] constexpr serializable_protocol
    select_serializable_protocol(medha::isolation iso) noexcept {
        if (iso != medha::isolation::serializable) return serializable_protocol::none;

        constexpr auto cap = detail::infer_commit_capability<R>();
        using cc = commit_capability;

        if constexpr (cap == cc::atomic_multi_key_within_resource) return serializable_protocol::resource_provided;
        if constexpr (cap == cc::pessimistic) return serializable_protocol::read_write_key_locking;
        if constexpr (cap == cc::serial_validation) return serializable_protocol::serial_validation;
        if constexpr (cap == cc::ssi) return serializable_protocol::ssi;
        if constexpr (cap == cc::deterministic) return serializable_protocol::deterministic_executor;

        return serializable_protocol::none;
    }

    template <class R>
    inline constexpr bool serializable_available =
        supports_serializable(detail::infer_commit_capability<R>());

    // ============================================================================
    // range_serializable_available<R> — serializable for range/predicate reads (§20.4)
    //
    // Hard rule: serializable isolation is UNAVAILABLE for range/predicate reads
    // unless the resource provides range validation, predicate locking, SSI, or
    // an equivalent protocol.
    //
    // Only resources with commit_capability::ssi or ::atomic_multi_key_within_resource satisfy
    // this — pessimistic/serial_validation/deterministic cover point reads only.
    //
    // Merely recording a range in read_set does NOT make it serializable.
    // The resource's tx_validate CPO must enforce phantom prevention.
    //
    // Note: atomic_multi_key_within_resource on an individual resource does NOT guarantee
    // atomicity across multiple resources.
    // ============================================================================

    [[nodiscard]] constexpr bool
    supports_range_serializable(commit_capability cap) noexcept {
        switch (cap) {
        case commit_capability::ssi:
        case commit_capability::atomic_multi_key_within_resource:
            return true;
        default:
            return false;
        }
    }

    template <class R>
    inline constexpr bool range_serializable_available =
        supports_range_serializable(detail::infer_commit_capability<R>());
} // namespace medha
