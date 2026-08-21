#pragma once
// =============================================================================
// medha/resource_traits.hpp — static type metadata for transactional resources
//
// C++23, header-only, no virtual, no macros.
// Specialize resource_traits<R> for each resource type.
// All fields are static constexpr — unused paths compile out.
//
// commit_capability describes the strongest serializable protocol a resource
// can offer. Medha uses this (not raw bools) to select isolation protocols.
// =============================================================================

#include <cstdint>

namespace medha {
    // ============================================================================
    // commit_capability — serializable protocol strength offered by a resource (§19)
    //
    // none             — no protocol; serializable isolation unavailable
    // atomic_single_key — safe single-key CAS; snapshot only (no serializable)
    // atomic_multi_key_within_resource — atomic multi-key CAS within one resource;
    //                   enables resource_provided serializable for point reads;
    //                   does NOT guarantee atomicity across multiple resources
    // pessimistic       — external locking; enables read_write_key_locking
    // serial_validation — supports post-phase validation; enables serial_validation
    // ssi               — supports serializable snapshot isolation
    // deterministic     — single-thread executor; all orderings equivalent
    // ============================================================================

    enum class commit_capability : std::uint8_t {
        none = 0,
        atomic_single_key = 1,
        atomic_multi_key_within_resource = 2,
        pessimistic = 3,
        serial_validation = 4,
        ssi = 5,
        deterministic = 6,

        // Deprecated alias — use atomic_multi_key_within_resource
        atomic_multi_key = atomic_multi_key_within_resource,
    };

    // ============================================================================
    // resource_traits<R> — primary template (all false/none; specialize per resource)
    // ============================================================================

    template <class R>
    struct resource_traits {
        // Must be true for a resource to participate in transactions
        static constexpr bool transactional = false;

        // ---- value capabilities ------------------------------------------------
        static constexpr bool value_trivially_copyable = false;
        static constexpr bool value_move_only = false;
        // resource owns staging; Medha stores opaque handle, never the value
        static constexpr bool resource_stages_values = false;

        // ---- snapshot/rollback -------------------------------------------------
        static constexpr bool supports_snapshot = false;
        static constexpr bool supports_rollback = false;

        // ---- commit capability (replaces thread_safe_commit + atomic_multi_key) -
        static constexpr commit_capability commit_protocol = commit_capability::none;

        // ---- versioning ---------------------------------------------------------
        static constexpr bool aba_safe = true;

        // ---- distribution-ready capabilities (all false in v1) -----------------
        static constexpr bool distributed_capable = false;
        static constexpr bool supports_prepare_commit = false;
        static constexpr bool supports_idempotent_remote_ops = false;
        static constexpr bool supports_durable_decision_log = false;
        static constexpr bool supports_remote_recovery = false;

        // ---- capability extensions (§6.1): checked by resource_capability_checker
        static constexpr bool supports_savepoints = false; // partial rollback within tx
        static constexpr bool supports_prepare = false; // 2PC prepare vote
        static constexpr bool supports_recovery = false; // can recover from durable log
        static constexpr bool supports_range_reads = false; // range/prefix key queries
        static constexpr bool supports_predicate_validation = false; // phantom prevention for ranges

        // key_type / value_type must be defined in specializations
        // using key_type   = ...;
        // using value_type = ...;
    };

    // ============================================================================
    // Alias helpers
    // ============================================================================

    template <class R>
    using resource_key = typename resource_traits<R>::key_type;

    template <class R>
    using resource_value = typename resource_traits<R>::value_type;
} // namespace medha
