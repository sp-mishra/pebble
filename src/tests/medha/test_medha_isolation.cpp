// =============================================================================
// test_medha_isolation.cpp — Unit tests for medha isolation + serializable protocol
//
// Verifies:
//   medha/isolation.hpp        (select_serializable_protocol, serializable_available)
//   medha/options.hpp          (isolation enum)
//   medha/conflict.hpp         (conflict_policy_traits)
//
// Cases:
//   1.  snapshot: serializable_protocol = none for snapshot isolation
//   2.  serializable: protocol = resource_provided when atomic_multi_key_commit
//   3.  serializable: protocol = read_write_key_locking when thread_safe_commit
//   4.  serializable: protocol = none when neither trait set → MEDHA-SER-011
//   5.  serializable_available: false when resource has no protocol
//   6.  serializable_available: true when thread_safe_commit
//   7.  conflict_policy_traits<optimistic>: no locks, can_deadlock=false
//   8.  conflict_policy_traits<pessimistic>: acquires_locks, can_deadlock=true
//   9.  conflict_policy_traits<deterministic>: acquires_locks, can_deadlock=false
//  10.  isolation enum values are distinct
// =============================================================================

#include "catch_amalgamated.hpp"

#include "medha/conflict.hpp"
#include "medha/isolation.hpp"
#include "medha/options.hpp"

using namespace medha;

// ============================================================================
// Resource types for isolation testing
// ============================================================================

namespace {
    struct no_protocol_resource {};

    struct atomic_commit_resource {};

    struct thread_safe_resource {};
} // namespace

template <>
struct medha::resource_traits<no_protocol_resource> {
    static constexpr bool transactional = true;
    static constexpr bool supports_atomic_multi_key_commit = false;
    static constexpr bool thread_safe_commit = false;
    static constexpr bool aba_safe = true;
    static constexpr bool distributed_capable = false;
    static constexpr bool supports_prepare_commit = false;
    static constexpr bool supports_idempotent_remote_ops = false;
    static constexpr bool supports_durable_decision_log = false;
    static constexpr bool supports_remote_recovery = false;
    static constexpr bool value_trivially_copyable = false;
    static constexpr bool value_move_only = false;
    static constexpr bool resource_stages_values = false;
    static constexpr bool supports_snapshot = true;
    static constexpr bool supports_rollback = false;
    using key_type = int;
    using value_type = int;
};

template <>
struct medha::resource_traits<atomic_commit_resource> {
    static constexpr bool transactional = true;
    static constexpr bool supports_atomic_multi_key_commit = true;
    static constexpr bool thread_safe_commit = false;
    static constexpr bool aba_safe = true;
    static constexpr bool distributed_capable = false;
    static constexpr bool supports_prepare_commit = false;
    static constexpr bool supports_idempotent_remote_ops = false;
    static constexpr bool supports_durable_decision_log = false;
    static constexpr bool supports_remote_recovery = false;
    static constexpr bool value_trivially_copyable = false;
    static constexpr bool value_move_only = false;
    static constexpr bool resource_stages_values = false;
    static constexpr bool supports_snapshot = true;
    static constexpr bool supports_rollback = false;
    using key_type = int;
    using value_type = int;
};

template <>
struct medha::resource_traits<thread_safe_resource> {
    static constexpr bool transactional = true;
    static constexpr bool supports_atomic_multi_key_commit = false;
    static constexpr bool thread_safe_commit = true;
    static constexpr bool aba_safe = true;
    static constexpr bool distributed_capable = false;
    static constexpr bool supports_prepare_commit = false;
    static constexpr bool supports_idempotent_remote_ops = false;
    static constexpr bool supports_durable_decision_log = false;
    static constexpr bool supports_remote_recovery = false;
    static constexpr bool value_trivially_copyable = false;
    static constexpr bool value_move_only = false;
    static constexpr bool resource_stages_values = false;
    static constexpr bool supports_snapshot = true;
    static constexpr bool supports_rollback = false;
    using key_type = int;
    using value_type = int;
};

// =============================================================================
// Cases 1-4: select_serializable_protocol
// =============================================================================

TEST_CASE (

"isolation: snapshot → serializable_protocol::none"
,
"[medha][isolation]"
)
 {
    auto p = select_serializable_protocol<no_protocol_resource>(isolation::snapshot);
    REQUIRE(p == serializable_protocol::none);
}

TEST_CASE (

"isolation: serializable + atomic_multi_key_commit → resource_provided"
,
"[medha][isolation]"
)
 {
    auto p = select_serializable_protocol<atomic_commit_resource>(isolation::serializable);
    REQUIRE(p == serializable_protocol::resource_provided);
}

TEST_CASE (

"isolation: serializable + thread_safe_commit → read_write_key_locking"
,
"[medha][isolation]"
)
 {
    auto p = select_serializable_protocol<thread_safe_resource>(isolation::serializable);
    REQUIRE(p == serializable_protocol::read_write_key_locking);
}

TEST_CASE (

"isolation: serializable + no protocol → none (MEDHA-SER-011 path)"
,
"[medha][isolation]"
)
 {
    auto p = select_serializable_protocol<no_protocol_resource>(isolation::serializable);
    REQUIRE(p == serializable_protocol::none);
    // Caller should emit MEDHA-SER-011 when p == none for serializable
}

// =============================================================================
// Cases 5-6: serializable_available
// =============================================================================

TEST_CASE (

"isolation: serializable_available false for no-protocol resource"
,
"[medha][isolation]"
)
 {
    REQUIRE_FALSE(serializable_available<no_protocol_resource>);
}

TEST_CASE (

"isolation: serializable_available true for thread_safe_commit resource"
,
"[medha][isolation]"
)
 {
    REQUIRE(serializable_available<thread_safe_resource>);
}

// =============================================================================
// Cases 7-9: conflict_policy_traits
// =============================================================================

TEST_CASE (

"conflict: optimistic_tag has no locks"
,
"[medha][isolation]"
)
 {
    using T = conflict_policy_traits<conflict::optimistic>;
    REQUIRE_FALSE(T::acquires_locks);
    REQUIRE_FALSE(T::can_deadlock);
}

TEST_CASE (

"conflict: pessimistic acquires locks, can deadlock"
,
"[medha][isolation]"
)
 {
    using T = conflict_policy_traits<conflict::pessimistic>;
    REQUIRE(T::acquires_locks);
    REQUIRE(T::can_deadlock);
}

TEST_CASE (

"conflict: deterministic acquires locks, cannot deadlock"
,
"[medha][isolation]"
)
 {
    using T = conflict_policy_traits<conflict::deterministic>;
    REQUIRE(T::acquires_locks);
    REQUIRE_FALSE(T::can_deadlock);
}

// =============================================================================
// Case 10: isolation enum
// =============================================================================

TEST_CASE (

"isolation: enum values distinct"
,
"[medha][isolation]"
)
 {
    REQUIRE(isolation::snapshot     != isolation::serializable);
    REQUIRE(static_cast<int>(isolation::snapshot)     == 0);
    REQUIRE(static_cast<int>(isolation::serializable) == 1);
}

// =============================================================================
// Cases 11-16: commit_capability enum + select_serializable_protocol (§19)
// New resource types that use commit_capability directly (not legacy bools).
// =============================================================================

namespace {
    struct cap_none_resource {};

    struct cap_atomic_multi_resource {};

    struct cap_pessimistic_resource {};

    struct cap_ssi_resource {};

    struct cap_deterministic_resource {};

    struct cap_serial_val_resource {};
} // namespace

template <>
struct medha::resource_traits<cap_none_resource> {
    static constexpr bool transactional = true;
    static constexpr medha::commit_capability commit_protocol = medha::commit_capability::none;
    static constexpr bool aba_safe = true;
    static constexpr bool value_trivially_copyable = true;
    static constexpr bool value_move_only = false;
    static constexpr bool resource_stages_values = false;
    static constexpr bool supports_snapshot = true;
    static constexpr bool supports_rollback = false;
    static constexpr bool distributed_capable = false;
    static constexpr bool supports_prepare_commit = false;
    static constexpr bool supports_idempotent_remote_ops = false;
    static constexpr bool supports_durable_decision_log = false;
    static constexpr bool supports_remote_recovery = false;
    using key_type = int;
    using value_type = int;
};

template <>
struct medha::resource_traits<cap_atomic_multi_resource> {
    static constexpr bool transactional = true;
    static constexpr medha::commit_capability commit_protocol = medha::commit_capability::atomic_multi_key;
    static constexpr bool aba_safe = true;
    static constexpr bool value_trivially_copyable = true;
    static constexpr bool value_move_only = false;
    static constexpr bool resource_stages_values = false;
    static constexpr bool supports_snapshot = true;
    static constexpr bool supports_rollback = false;
    static constexpr bool distributed_capable = false;
    static constexpr bool supports_prepare_commit = false;
    static constexpr bool supports_idempotent_remote_ops = false;
    static constexpr bool supports_durable_decision_log = false;
    static constexpr bool supports_remote_recovery = false;
    using key_type = int;
    using value_type = int;
};

template <>
struct medha::resource_traits<cap_pessimistic_resource> {
    static constexpr bool transactional = true;
    static constexpr medha::commit_capability commit_protocol = medha::commit_capability::pessimistic;
    static constexpr bool aba_safe = true;
    static constexpr bool value_trivially_copyable = true;
    static constexpr bool value_move_only = false;
    static constexpr bool resource_stages_values = false;
    static constexpr bool supports_snapshot = true;
    static constexpr bool supports_rollback = false;
    static constexpr bool distributed_capable = false;
    static constexpr bool supports_prepare_commit = false;
    static constexpr bool supports_idempotent_remote_ops = false;
    static constexpr bool supports_durable_decision_log = false;
    static constexpr bool supports_remote_recovery = false;
    using key_type = int;
    using value_type = int;
};

template <>
struct medha::resource_traits<cap_ssi_resource> {
    static constexpr bool transactional = true;
    static constexpr medha::commit_capability commit_protocol = medha::commit_capability::ssi;
    static constexpr bool aba_safe = true;
    static constexpr bool value_trivially_copyable = true;
    static constexpr bool value_move_only = false;
    static constexpr bool resource_stages_values = false;
    static constexpr bool supports_snapshot = true;
    static constexpr bool supports_rollback = false;
    static constexpr bool distributed_capable = false;
    static constexpr bool supports_prepare_commit = false;
    static constexpr bool supports_idempotent_remote_ops = false;
    static constexpr bool supports_durable_decision_log = false;
    static constexpr bool supports_remote_recovery = false;
    using key_type = int;
    using value_type = int;
};

template <>
struct medha::resource_traits<cap_deterministic_resource> {
    static constexpr bool transactional = true;
    static constexpr medha::commit_capability commit_protocol = medha::commit_capability::deterministic;
    static constexpr bool aba_safe = true;
    static constexpr bool value_trivially_copyable = true;
    static constexpr bool value_move_only = false;
    static constexpr bool resource_stages_values = false;
    static constexpr bool supports_snapshot = true;
    static constexpr bool supports_rollback = false;
    static constexpr bool distributed_capable = false;
    static constexpr bool supports_prepare_commit = false;
    static constexpr bool supports_idempotent_remote_ops = false;
    static constexpr bool supports_durable_decision_log = false;
    static constexpr bool supports_remote_recovery = false;
    using key_type = int;
    using value_type = int;
};

template <>
struct medha::resource_traits<cap_serial_val_resource> {
    static constexpr bool transactional = true;
    static constexpr medha::commit_capability commit_protocol = medha::commit_capability::serial_validation;
    static constexpr bool aba_safe = true;
    static constexpr bool value_trivially_copyable = true;
    static constexpr bool value_move_only = false;
    static constexpr bool resource_stages_values = false;
    static constexpr bool supports_snapshot = true;
    static constexpr bool supports_rollback = false;
    static constexpr bool distributed_capable = false;
    static constexpr bool supports_prepare_commit = false;
    static constexpr bool supports_idempotent_remote_ops = false;
    static constexpr bool supports_durable_decision_log = false;
    static constexpr bool supports_remote_recovery = false;
    using key_type = int;
    using value_type = int;
};

TEST_CASE (

"isolation: commit_capability::none → no serializable protocol"
,
"[medha][isolation][commit_capability]"
)
 {
    auto p = select_serializable_protocol<cap_none_resource>(isolation::serializable);
    REQUIRE(p == serializable_protocol::none);
    REQUIRE_FALSE(serializable_available<cap_none_resource>);
}

TEST_CASE (

"isolation: commit_capability::atomic_multi_key → resource_provided"
,
"[medha][isolation][commit_capability]"
)
 {
    auto p = select_serializable_protocol<cap_atomic_multi_resource>(isolation::serializable);
    REQUIRE(p == serializable_protocol::resource_provided);
    REQUIRE(serializable_available<cap_atomic_multi_resource>);
}

TEST_CASE (

"isolation: commit_capability::pessimistic → read_write_key_locking"
,
"[medha][isolation][commit_capability]"
)
 {
    auto p = select_serializable_protocol<cap_pessimistic_resource>(isolation::serializable);
    REQUIRE(p == serializable_protocol::read_write_key_locking);
    REQUIRE(serializable_available<cap_pessimistic_resource>);
}

TEST_CASE (

"isolation: commit_capability::ssi → ssi protocol"
,
"[medha][isolation][commit_capability]"
)
 {
    auto p = select_serializable_protocol<cap_ssi_resource>(isolation::serializable);
    REQUIRE(p == serializable_protocol::ssi);
    REQUIRE(serializable_available<cap_ssi_resource>);
}

TEST_CASE (

"isolation: commit_capability::deterministic → deterministic_executor"
,
"[medha][isolation][commit_capability]"
)
 {
    auto p = select_serializable_protocol<cap_deterministic_resource>(isolation::serializable);
    REQUIRE(p == serializable_protocol::deterministic_executor);
    REQUIRE(serializable_available<cap_deterministic_resource>);
}

TEST_CASE (

"isolation: commit_capability::serial_validation → serial_validation"
,
"[medha][isolation][commit_capability]"
)
 {
    auto p = select_serializable_protocol<cap_serial_val_resource>(isolation::serializable);
    REQUIRE(p == serializable_protocol::serial_validation);
    REQUIRE(serializable_available<cap_serial_val_resource>);
}

TEST_CASE (

"isolation: snapshot isolation always returns none regardless of commit_capability"
,
"[medha][isolation][commit_capability]"
)
 {
    REQUIRE(select_serializable_protocol<cap_atomic_multi_resource>(isolation::snapshot) ==
            serializable_protocol::none);
    REQUIRE(select_serializable_protocol<cap_pessimistic_resource>(isolation::snapshot) ==
            serializable_protocol::none);
}

TEST_CASE (

"isolation: commit_capability enum values are distinct and ordered"
,
"[medha][isolation][commit_capability]"
)
 {
    using cc = commit_capability;
    REQUIRE(static_cast<int>(cc::none)               == 0);
    REQUIRE(static_cast<int>(cc::atomic_single_key)  == 1);
    REQUIRE(static_cast<int>(cc::atomic_multi_key)   == 2);
    REQUIRE(static_cast<int>(cc::pessimistic)        == 3);
    REQUIRE(static_cast<int>(cc::serial_validation)  == 4);
    REQUIRE(static_cast<int>(cc::ssi)                == 5);
    REQUIRE(static_cast<int>(cc::deterministic)      == 6);
}
