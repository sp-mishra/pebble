#pragma once
// =============================================================================
// medha/context.hpp — transaction_context
//
// C++23, header-only, no virtual, no macros.
//
// transaction_context: per-attempt state machine.
//   - Thread-affine by default (never used concurrently).
//   - Destructor auto-aborts (RAII §12.5).
//   - load/store drive read-your-writes (§12.7).
//   - Nested scopes flattened: nested commit merges write set; nested abort discards.
//   - Smriti arena for transaction-local scratch (opt-in).
//
// Snapshot semantics (§19.4):
//   A snapshot_token is captured at transaction construction.
//   Resources may opt in to tx_snapshot(R&) CPO for a resource-native handle.
//   Without a resource CPO, snapshot boundary is established on first read
//   (first-read-version caching): snapshot_token.boundary = version of first load.
//   Repeated reads of the same key return the write-set value (if written) or
//   the value from the snapshot — the resource's tx_read must honour the
//   snapshot boundary when the resource is snapshot-capable.
//   tx_read and tx_stage are RESOURCE HOOKS; they must not bypass Medha's
//   read/write-set bookkeeping.
//
// Usage:
//   transaction_context ctx{ options{} };
//   auto v = ctx.load(resource_handle, key);
//   ctx.store(resource_handle, key, new_value);
//   auto r = ctx.commit();
// =============================================================================

#include "medha/fwd.hpp"
#include "medha/commit.hpp"
#include "medha/diagnostics.hpp"
#include "medha/effects.hpp"
#include "medha/identity.hpp"
#include "medha/isolation.hpp"
#include "medha/key.hpp"
#include "medha/options.hpp"
#include "medha/read_set.hpp"
#include "medha/resource_handle.hpp"
#include "medha/resource_traits.hpp"
#include "medha/retry.hpp"
#include "medha/version.hpp"
#include "medha/write_set.hpp"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <algorithm>
#include <vector>

namespace medha {
    // ============================================================================
    // tx_phase — internal state machine for the context
    // ============================================================================

    enum class tx_phase : std::uint8_t {
        idle = 0, // no active transaction
        active = 1, // body running
        validating = 2,
        committing = 3,
        committed = 4,
        aborted = 5,
    };

    // ============================================================================
    // transaction_context
    // ============================================================================

    class transaction_context {
    public:
        explicit transaction_context(options opts = {})
            : opts_(std::move(opts))
              , phase_(tx_phase::active)
              , start_(std::chrono::steady_clock::now()) {}


        // Non-copyable; non-movable (thread-affine).
        transaction_context(const transaction_context&) = delete;
        transaction_context& operator=(const transaction_context&) = delete;
        transaction_context(transaction_context&&) = delete;
        transaction_context& operator=(transaction_context&&) = delete;

        // RAII: auto-abort if not committed.
        ~transaction_context() noexcept {
            if (phase_ == tx_phase::active || phase_ == tx_phase::validating) {
                abort();
            }
        }

        // -------------------------------------------------------------------------
        // load — read a value from resource, recording into read set.
        // Lookup order: write_set → parent → resource (§12.7 read-your-writes).
        // -------------------------------------------------------------------------

        template <transactional_resource R>
        [[nodiscard]] std::expected<resource_value < R>
        ,
        tx_error
        >
        load(resource_handle<R>& handle, resource_key<R> key) {
            assert(phase_ == tx_phase::active);
            enlist(handle.resource());

            auto ck = canonicalize(handle.id(), handle.resource(), key);

            // Read-your-writes: check write set first
            if constexpr (resource_traits<R>::value_trivially_copyable) {
                if (const auto* we = write_set_.find(ck)) {
                    if (we->storage == value_storage_kind::inline_copy) {
                        resource_value<R> v{};
                        static_assert(sizeof(v) <= kInlineValueBytes,
                                      "value type too large for inline write_set storage");
                        __builtin_memcpy(&v, we->inline_bytes, sizeof(v));
                        return v;
                    }
                }
            }
            // Resource-owned staging falls through to the resource, which owns
            // the typed attempt-local value and provides read-your-writes.

            // Delegate to resource
            auto r = handle.read(*this, key);
            if (!r) return std::unexpected(r.error());

            // Record in read set (we need a version; use a placeholder if resource
            // doesn't provide version tracking — resource must provide current_version CPO)
            version_stamp vs{};
            if constexpr (requires { tx_version(handle.resource(), key); }) {
                vs = tx_version(handle.resource(), key);
            }
            read_set_.record(ck, vs);

            // Establish snapshot boundary on first read (first-read-version caching, §19.4).
            // If isolation is snapshot or serializable and no resource-provided snapshot
            // was acquired at construction, pin the snapshot to the first read's version.
            if (!snapshot_.is_valid()) {
                snapshot_.boundary = vs;
                snapshot_.resource_provided = false;
                snapshot_.valid = true;
            }

            ++stats_reads_;

            return r;
        }

        // -------------------------------------------------------------------------
        // store — stage a write (inline for trivially-copyable values).
        // -------------------------------------------------------------------------

        template <transactional_resource R>
        [[nodiscard]] std::expected<void, tx_error>
        store(resource_handle<R>& handle, resource_key<R> key, resource_value<R> value)
            requires resource_traits < R > ::value_trivially_copyable {
            assert(phase_ == tx_phase::active);
            enlist(handle.resource());
            if (auto staged = handle.stage(*this, key, value); !staged) return std::unexpected(staged.error());

            auto ck = canonicalize(handle.id(), handle.resource(), key);

            version_stamp base{};
            if (const auto* rv = read_set_.find(ck)) base = *rv;

            write_set_.stage_inline(ck, base, value);
            read_set_.mark_shadowed(ck);
            ++stats_writes_;
            return {};
        }

        // Resource-owned staging is the generic path for values that cannot be
        // copied into Medha's fixed inline write set. The resource keeps the
        // typed staged value for this attempt; Medha records an opaque handle
        // only so lifecycle, validation, and rollback remain coordinated.
        template <transactional_resource R>
        [[nodiscard]] std::expected<void, tx_error>
        store(resource_handle<R>& handle, resource_key<R> key, resource_value<R> value)
            requires (!resource_traits < R > ::value_trivially_copyable &&
                resource_traits < R > ::resource_stages_values) {
            assert(phase_ == tx_phase::active);
            enlist(handle.resource());
            if (auto staged = handle.stage(*this, key, std::move(value)); !staged)
                return std::unexpected(staged.error());
            auto ck = canonicalize(handle.id(), handle.resource(), key);
            version_stamp base{};
            if (const auto* rv = read_set_.find(ck)) base = *rv;
            // The staged value remains resource-owned; a null opaque handle is
            // valid because rollback/commit dispatch through the participant.
            write_set_.stage_handle(ck, base, {});
            read_set_.mark_shadowed(ck);
            ++stats_writes_;
            return {};
        }

        template <transactional_resource R>
        [[nodiscard]] std::expected<void, tx_error>
        store_handle(resource_handle<R>& handle, resource_key<R> key, staging_handle h) {
            assert(phase_ == tx_phase::active);
            auto ck = canonicalize(handle.id(), handle.resource(), key);
            version_stamp base{};
            if (const auto* rv = read_set_.find(ck)) base = *rv;
            write_set_.stage_handle(ck, base, h);
            read_set_.mark_shadowed(ck);
            ++stats_writes_;
            return {};
        }

        // -------------------------------------------------------------------------
        // defer_compensation — register a noexcept undo action for compensatable effects.
        //
        // Called during the transaction body to register a compensation action for
        // any effect with kEffectCompensatable. Compensations run in LIFO order if
        // the transaction aborts after they were registered.
        //
        // Contract:
        //   fn must be noexcept — if compensation itself fails, the transaction
        //   status transitions to tx_status::recovery_required (not aborted).
        //
        // Usage:
        //   ctx.defer_compensation([] noexcept { undo_reservation(); });
        // -------------------------------------------------------------------------

        // compensation_fn must not throw; noexcept enforced by convention since
        // std::function does not support noexcept specifiers on Apple libc++.
        using compensation_fn = std::function<void()>;

        void defer_compensation(compensation_fn fn) {
            assert(phase_ == tx_phase::active);
            compensations_.push_back(std::move(fn));
        }

        // -------------------------------------------------------------------------
        // commit — validate + apply staged writes (§20.2)
        // Returns commit_report on success; tx_error on failure.
        //
        // transaction_context::commit() performs ONE attempt only.
        // The retry loop is owned by medha::atomic(). Retry-related options
        // (retry::bounded, retry::backoff) in options are ignored by commit()
        // and are only meaningful when passed to atomic().
        // -------------------------------------------------------------------------

        [[nodiscard]] std::expected<commit_report, tx_error> commit() {
            if (phase_ != tx_phase::active) {
                return std::unexpected(tx_error{tx_status::rejected, "context not active"});
            }
            phase_ = tx_phase::validating;
            for (const auto& p : participants_) {
                if (auto r = p.validate(p.object, *this); !r) {
                    abort();
                    return std::unexpected(r.error());
                }
            }

            // Build report
            commit_report report{};
            report.attempts = attempt_count_ + 1;
            report.conflicts = conflict_count_;
            report.reads = stats_reads_;
            report.writes = stats_writes_;
            report.elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - start_);

            // Validation: check all read versions (simplified; resource-specific validation
            // is done through validate CPO when resource provides it).
            // For now: optimistic path — assume no external version tracking (resource provides).
            // Full serializable locking not implemented here; deferred to resource validate CPO.

            phase_ = tx_phase::committing;
            for (const auto& p : participants_) {
                if (auto r = p.commit(p.object, *this); !r) {
                    abort();
                    return std::unexpected(r.error());
                }
            }
            phase_ = tx_phase::committed;

            report.status = tx_status::committed;
            report.resources_touched = static_cast<std::uint32_t>(touched_resources_.size());
            return report;
        }

        // -------------------------------------------------------------------------
        // abort — rollback all staged writes, run compensations (LIFO)
        // -------------------------------------------------------------------------

        void abort() noexcept {
            if (phase_ == tx_phase::committed || phase_ == tx_phase::aborted) return;
            // Rollback: staged values with non-trivial dtors should run here.
            // For Smriti arena-backed scratch: arena.rollback(checkpoint_) would go here.
            write_set_.clear();
            read_set_.clear();

            // Run compensations in LIFO order.
            // If a compensation function itself fails (which cannot happen since
            // compensation_fn is noexcept), tx_status would be recovery_required.
            for (auto it = compensations_.rbegin(); it != compensations_.rend(); ++it) {
                (*it)();
            }
            compensations_.clear();
            for (auto it = participants_.rbegin(); it != participants_.rend(); ++it) {
                it->rollback(it->object, *this);
            }

            phase_ = tx_phase::aborted;
        }

        // -------------------------------------------------------------------------
        // Accessors
        // -------------------------------------------------------------------------

        [[nodiscard]] tx_phase phase() const noexcept { return phase_; }
        [[nodiscard]] const read_set& reads() const noexcept { return read_set_; }
        [[nodiscard]] const write_set& writes() const noexcept { return write_set_; }
        [[nodiscard]] const options& tx_options() const noexcept { return opts_; }
        [[nodiscard]] const snapshot_token& snapshot() const noexcept { return snapshot_; }

        void record_touched(resource_id id) {
            for (auto rid : touched_resources_) {
                if (rid == id) return;
            }
            touched_resources_.push_back(id);
        }

        void record_conflict() noexcept { ++conflict_count_; }
        void increment_attempt() noexcept { ++attempt_count_; }

        template <transactional_resource R>
        void enlist(R& resource) {
            const auto object = static_cast<void*>(&resource);
            if (std::ranges::any_of(participants_, [object](const participant& p) {
                return p.object == object;
            })) return;
            participants_.push_back({
                object,
                [](void* p, transaction_context& c) { return tx_validate(*static_cast<R*>(p), c); },
                [](void* p, transaction_context& c) { return tx_commit(*static_cast<R*>(p), c); },
                [](void* p, transaction_context& c) noexcept { tx_rollback(*static_cast<R*>(p), c); }
            });
        }

        // Reset for retry: clear state, keep options.
        void reset_for_retry() noexcept {
            write_set_.clear();
            read_set_.clear();
            touched_resources_.clear();
            compensations_.clear();
            snapshot_ = {}; // clear snapshot; re-acquired on first read next attempt
            stats_reads_ = 0;
            stats_writes_ = 0;
            phase_ = tx_phase::active;
        }

    private:
        struct participant {
            void* object;
            std::expected<void, tx_error> (*validate)(void*, transaction_context&);
            std::expected<void, tx_error> (*commit)(void*, transaction_context&);
            void (*rollback)(void*, transaction_context&) noexcept;
        };

        options opts_;
        tx_phase phase_;
        read_set read_set_;
        write_set write_set_;
        snapshot_token snapshot_; // snapshot boundary (§19.4)
        std::vector<resource_id> touched_resources_;
        std::vector<participant> participants_;
        std::vector<compensation_fn> compensations_; // LIFO on abort (§compensatable effects)
        std::chrono::steady_clock::time_point start_;
        std::uint32_t attempt_count_ = 0;
        std::uint32_t conflict_count_ = 0;
        std::uint32_t stats_reads_ = 0;
        std::uint32_t stats_writes_ = 0;
    };
} // namespace medha
