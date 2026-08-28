#pragma once
// =============================================================================
// medha/options.hpp — isolation, retry, conflict, distribution, replay policy structs
//
// C++23, header-only, no virtual, no macros.
// All policy structs are empty or tiny — [[no_unique_address]] in context.
//
// replay_safety:
//   Declares whether the transaction body can be safely re-executed on retry.
//   atomic() enforces that retry is not retry::none when replay_safety is
//   body_and_effects_idempotent, and warns when retry is non-none but
//   replay_safety is unknown or non_idempotent.
//
// partial_commit_policy:
//   When a multi-resource commit partially succeeds, controls the fallback.
// =============================================================================

#include <chrono>
#include <cstdint>
#include <variant>

namespace medha {
    // ============================================================================
    // isolation
    // ============================================================================

    enum class isolation : std::uint8_t {
        snapshot = 0, // read-stable; write/write conflict prevented; write-skew possible
        serializable = 1, // full serializability via locking or resource-provided protocol
        read_committed = 2, // no dirty reads; each read sees a committed value; write-skew + phantoms possible
    };

    // ============================================================================
    // replay_safety — explicit caller declaration for retry safety (§20.5)
    //
    // Retry safety rules enforced by atomic():
    //   non_idempotent + non-none retry → MEDHA-004 (rejected)
    //   unknown + non-none retry        → allowed (permissive; caller accepts re-execution risk)
    //   body_idempotent, body_and_effects_idempotent, unknown_but_retry_allowed
    //       + non-none retry            → allowed
    //
    // unknown_but_retry_allowed is an alias/annotation for callers wanting explicit opt-in semantics.
    // ============================================================================

    enum class replay_safety : std::uint8_t {
        unknown = 0, // no declaration; non-none retry allowed (permissive)
        non_idempotent = 1, // irreversible effects; any non-none retry → MEDHA-004 (rejected)
        body_idempotent = 2, // body re-execution safe; effects not declared
        body_and_effects_idempotent = 3, // full idempotency; safest for aggressive retry
        unknown_but_retry_allowed = 4, // explicit opt-in: undeclared safety, retry proceeds anyway
    };

    // ============================================================================
    // partial_commit_policy — multi-resource commit failure handling (§21.2)
    //
    // require_atomic_coordinator (abort_all)
    //   Abort ALL resources if preparation fails before any commit begins.
    //   Atomicity is achievable only via a coordinator, two-phase commit,
    //   resource-provided atomic commit, or a compensating action.
    //
    // best_effort
    //   Commit as many resources as possible; report all outcomes.
    //   Guaranteed to report tx_status::partial_commit (never tx_status::committed)
    //   when at least one resource fails.
    // ============================================================================

    enum class partial_commit_policy : std::uint8_t {
        require_atomic_coordinator = 0, // abort if any prepare fails (default, conservative)
        best_effort = 1, // commit as many as possible; partial_commit status
        abort_all = require_atomic_coordinator, // alias
    };

    // ============================================================================
    // retry policies
    // ============================================================================

    namespace retry {
        struct none {};

        struct bounded {
            std::uint32_t max = 3;
        };

        struct backoff {
            std::uint32_t max = 5;
            std::chrono::nanoseconds base = std::chrono::milliseconds{1};
            double factor = 2.0;
        };
    } // namespace retry

    // ============================================================================
    // conflict policies
    // ============================================================================

    namespace conflict {
        struct optimistic {}; // validate at commit; abort on conflict
        struct pessimistic {}; // acquire locks pre-read
        struct deterministic {}; // resource-ordered; no aborts
    } // namespace conflict

    // ============================================================================
    // distribution policies (§14.1; only none is implemented in v1)
    // ============================================================================

    namespace distribution {
        struct none {};

        struct resource_defined {
            std::chrono::milliseconds timeout{5000};
        };

        struct coordinated {
            // distributed_commit_protocol injected from fwd.hpp when available
            std::chrono::milliseconds timeout{10000};
        };
    } // namespace distribution

    // ============================================================================
    // options
    // ============================================================================

    struct options {
        medha::isolation isolation = medha::isolation::snapshot;

        std::variant<retry::none,
                     retry::bounded,
                     retry::backoff> retry = retry::none{};

        std::variant<conflict::optimistic,
                     conflict::pessimistic,
                     conflict::deterministic> conflict = conflict::optimistic{};

        std::variant<distribution::none,
                     distribution::resource_defined,
                     distribution::coordinated> distribution = distribution::none{};

        medha::replay_safety replay = replay_safety::unknown;
        medha::partial_commit_policy partial = partial_commit_policy::require_atomic_coordinator;
    };
} // namespace medha
