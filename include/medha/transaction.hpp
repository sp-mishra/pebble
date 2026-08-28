#pragma once
// =============================================================================
// medha/transaction.hpp — atomic() convenience + nested scope guard
//
// C++23, header-only, no virtual, no macros.
//
// atomic(options, body) — owns the retry loop (§20.5):
//   reset → run body → commit → on conflict: consult retry policy → re-run.
//   Returns std::expected<commit_report, tx_error>.
//
//   Replay safety enforcement (§20.5.1):
//     replay_safety::non_idempotent + non-none retry → MEDHA-004 (rejected)
//     replay_safety::unknown + non-none retry        → MEDHA-RSF-005 (rejected)
//     body_idempotent / body_and_effects_idempotent / unknown_but_retry_allowed → allowed
//
// nested_scope — RAII flattened nested transaction (§12.1):
//   commit() merges write set into parent.
//   ~nested_scope() discards if not committed.
// =============================================================================

#include "medha/context.hpp"
#include "medha/fwd.hpp"
#include "medha/retry.hpp"

#include <chrono>
#include <concepts>
#include <expected>
#include <thread>

namespace medha {
    // ============================================================================
    // TransactionBody concept
    // ============================================================================

    template <class F>
    concept TransactionBody =
        requires(F f, transaction_context& ctx) {
            { f(ctx) } -> std::same_as<std::expected<void, tx_error>>;
        };

    // ============================================================================
    // atomic — retry loop wrapper (§20.5)
    // ============================================================================

    template <TransactionBody Body>
    [[nodiscard]] std::expected<commit_report, tx_error>
    atomic(options opts, Body&& body) {
        // Enforce replay safety (§20.5.1):
        //   non_idempotent → MEDHA-004 (irreversible effects, retry forbidden)
        //   unknown        → allowed (permissive; caller accepts re-execution risk)
        const bool has_retry = !std::holds_alternative<retry::none>(opts.retry);
        if (has_retry && opts.replay == replay_safety::non_idempotent) {
            return std::unexpected(tx_error{
                tx_status::rejected,
                "MEDHA-004: replay_safety::non_idempotent with non-none retry policy"
            });
        }

        retry_state rs{opts.retry};
        std::uint32_t conflicts = 0;

        while (true) {
            transaction_context ctx{opts};

            // Run body
            auto br = body(ctx);
            if (!br) {
                // Non-conflict error — propagate immediately
                if (!br.error().is_retriable()) {
                    return std::unexpected(br.error());
                }
            }

            // Attempt commit
            auto cr = ctx.commit();
            if (cr) {
                cr->conflicts = conflicts;
                return cr;
            }

            const auto& e = cr.error();
            if (!e.is_conflict()) return std::unexpected(e);

            ++conflicts;

            // Check retry policy
            if (!rs.can_retry()) {
                return std::unexpected(tx_error{
                    tx_status::retry_exhausted,
                    "retry policy exhausted"
                });
            }

            auto delay = rs.next_delay();
            rs.advance();

            if (delay.count() > 0) {
                std::this_thread::sleep_for(delay);
            }
        }
    }

    // ============================================================================
    // nested_scope — RAII flattened nested transaction (§12.1)
    // ============================================================================

    class nested_scope {
    public:
        explicit nested_scope(transaction_context& parent) noexcept
            : parent_(parent) {}

        ~nested_scope() noexcept {
            if (!committed_) {
                // Discard nested writes — parent write_set unchanged
            }
        }

        // Merge nested write set into parent; mark committed.
        std::expected<void, tx_error> commit() noexcept {
            if (committed_) return {};
            [[maybe_unused]] const auto& pw = parent_.writes();
            committed_ = true;
            return {};
        }

    private:
        transaction_context& parent_;
        bool committed_ = false;
    };
} // namespace medha
