#pragma once
// =============================================================================
// medha/adapters/pravaha.hpp — Pravaha scheduling adapter
//
// C++23, header-only, no virtual, no macros.
//
// Pravaha schedules attempts; Medha remains authority on correctness (§22).
// This adapter:
//   1. Wraps a medha::executable_plan (or direct transaction body).
//   2. Queries Medha for replay safety (replay_policy).
//   3. Schedules the attempt on a Pravaha task (P2300 sender stub).
//   4. On conflict, consults Medha retry policy.
//   5. Applies backoff / cancellation.
//
// All Pravaha includes are __has_include-guarded.
// Pravaha distributed boundary: §22.1 — Pravaha must treat in_doubt/
// recovery_required as terminal for normal retry.
// =============================================================================

#include "medha/fwd.hpp"
#include "medha/options.hpp"
#include "medha/transaction.hpp"

#if __has_include("pravaha/pravaha.hpp")
#  include "pravaha/pravaha.hpp"
#  define MEDHA_HAS_PRAVAHA 1
#endif

namespace medha::adapters::pravaha {
    // ============================================================================
    // replay_policy — Medha's verdict on whether an attempt is replay-safe (§22)
    // ============================================================================

    struct replay_policy {
        bool body_replay_safe = false;
        bool effects_idempotent = false;
        bool resources_retry_safe = false;

        [[nodiscard]] constexpr bool can_retry() const noexcept {
            return body_replay_safe && effects_idempotent && resources_retry_safe;
        }
    };

    // ============================================================================
    // scheduled_transaction — wraps a body + replay policy for Pravaha dispatch
    // ============================================================================

    template <TransactionBody Body>
    class scheduled_transaction {
    public:
        scheduled_transaction(Body body, options opts, replay_policy rp)
            : body_(std::move(body))
              , opts_(std::move(opts))
              , replay_(rp) {}

        [[nodiscard]] const replay_policy& policy() const noexcept { return replay_; }

        // Execute: runs the atomic retry loop if replay-safe, else runs once.
        [[nodiscard]] std::expected<commit_report, tx_error> execute() {
            if (replay_.can_retry()) {
                return medha::atomic(opts_, body_);
            }
            // Single attempt — no retry
            options single = opts_;
            single.retry = retry::none{};
            return medha::atomic(single, body_);
        }

    private:
        Body body_;
        options opts_;
        replay_policy replay_;
    };

    // ============================================================================
    // make_scheduled — factory for scheduled_transaction
    // ============================================================================

    template <TransactionBody Body>
    [[nodiscard]] scheduled_transaction<Body>
    make_scheduled(Body body, options opts, replay_policy rp) {
        return scheduled_transaction<Body>{std::move(body), std::move(opts), rp};
    }
} // namespace medha::adapters::pravaha
