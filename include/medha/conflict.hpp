#pragma once
// =============================================================================
// medha/conflict.hpp — conflict_policy concepts + implementations
//
// C++23, header-only, no virtual, no macros.
//
// Three policies:
//   optimistic   — validate at commit; abort on conflict
//   pessimistic  — acquire read/write locks before access
//   deterministic — resource-ordered acquire; no aborts (deadlock-free)
//
// ConflictPolicy concept: detect() + acquire() + release() operations.
// =============================================================================

#include "medha/options.hpp"

namespace medha {
    // ============================================================================
    // ConflictPolicy concept
    // ============================================================================

    template <class P>
    concept ConflictPolicy = requires {
        typename P::tag_type; // policy tag for compile-time dispatch
    };

    // ============================================================================
    // Optimistic policy tag
    // ============================================================================

    namespace policy {
        struct optimistic_tag {};

        struct pessimistic_tag {};

        struct deterministic_tag {};
    } // namespace policy

    // ============================================================================
    // conflict_policy_traits<Policy> — binds tag to behaviour
    // ============================================================================

    template <class P>
    struct conflict_policy_traits;

    template <>
    struct conflict_policy_traits<conflict::optimistic> {
        using tag_type = policy::optimistic_tag;
        static constexpr bool acquires_locks = false;
        static constexpr bool can_deadlock = false;
    };

    template <>
    struct conflict_policy_traits<conflict::pessimistic> {
        using tag_type = policy::pessimistic_tag;
        static constexpr bool acquires_locks = true;
        static constexpr bool can_deadlock = true; // caller must use deterministic order
    };

    template <>
    struct conflict_policy_traits<conflict::deterministic> {
        using tag_type = policy::deterministic_tag;
        static constexpr bool acquires_locks = true;
        static constexpr bool can_deadlock = false; // lock order enforced by canonical_key <
    };
} // namespace medha
