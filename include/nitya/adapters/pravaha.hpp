#pragma once
// Optional Pravaha integration for Nitya maintenance work.
//
// This header deliberately does not participate in nitya/nitya.hpp: WAL
// reserve/publish/durability remains dependency-free and synchronous at its
// correctness boundary. Include this file only when scheduling recovery or
// retention through Pravaha is desired.
//
// Concurrency note (Item 9): Pravaha's Runner::submit executes the submitted
// task graph synchronously (it runs the IR inline) and there is no deferred
// future/handle to compose against yet. These adapters are therefore honestly
// *blocking*: they submit the work, drain the backend, and return the result.
// The `_blocking` names are canonical; the historical `*_async` names are kept
// as aliases for source compatibility and behave identically. When Pravaha
// grows a real deferred-completion handle, a non-draining overload can be added
// here without changing the blocking contract these names carry today.

#include "nitya/nitya.hpp"
#include "pravaha/pravaha.hpp"

#include <chrono>
#include <concepts>
#include <functional>
#include <utility>

namespace nitya::pravaha_adapter {
    // A Pravaha runner this adapter can drive: it accepts a task via submit()
    // (result is contextually convertible to bool for success) and exposes a
    // drainable backend. Constrains the previously duck-typed Runner so misuse
    // is a clear concept error rather than a deep template failure.
    template <typename R>
    concept pravaha_runner = requires(R& r) {
        { r.submit(::pravaha::task("probe", [] {})) };
        { r.backend_ref().drain() };
    };

    // Blocking recovery: submit the scan through the runner, drain, return status.
    template <typename Wal, pravaha_runner Runner, typename RecordCallback>
    [[nodiscard]] Result<recovery_status> recover_blocking(
        Wal& log,
        Runner& runner,
        RecordCallback&& on_record,
        const lsn_t start_lsn = 0,
        const recovery_mode mode = recovery_mode::stop_at_first_error) {
        recovery_status status{};
        auto work = [&] {
            auto stream = log.recover(start_lsn, mode);
            for (const auto& record : stream) {
                on_record(record);
            }
            status = stream.status();
            if (status.error == LogError::EndOfLog) {
                status.error = LogError::Success;
            }
        };

        auto submitted = runner.submit(::pravaha::task("nitya_recovery", std::move(work)));
        if (!submitted) return std::unexpected(LogError::InternalError);
        runner.backend_ref().drain();
        return status;
    }

    // Blocking retention evaluation: submit the rule pass, drain, return.
    template <typename Wal, pravaha_runner Runner>
    [[nodiscard]] Result<void> apply_retention_rules_blocking(
        Wal& log,
        Runner& runner,
        const std::chrono::seconds max_segment_age,
        std::function<void(const segment_descriptor &)> on_archive = {},
        std::function<void(const segment_descriptor &)> on_delete = {}) {
        auto work = [&log, max_segment_age, on_archive = std::move(on_archive), on_delete = std::move(on_delete)] {
            log.apply_retention_rules(max_segment_age, on_archive, on_delete);
        };

        auto submitted = runner.submit(::pravaha::task("nitya_retention", std::move(work)));
        if (!submitted) return std::unexpected(LogError::InternalError);
        runner.backend_ref().drain();
        return {};
    }

    // Historical names — identical blocking behaviour, retained for compatibility.
    template <typename Wal, pravaha_runner Runner, typename RecordCallback>
    [[nodiscard]] Result<recovery_status> recover_async(
        Wal& log,
        Runner& runner,
        RecordCallback&& on_record,
        const lsn_t start_lsn = 0,
        const recovery_mode mode = recovery_mode::stop_at_first_error) {
        return recover_blocking(log, runner, std::forward<RecordCallback>(on_record), start_lsn, mode);
    }

    template <typename Wal, pravaha_runner Runner>
    [[nodiscard]] Result<void> apply_retention_rules_async(
        Wal& log,
        Runner& runner,
        const std::chrono::seconds max_segment_age,
        std::function<void(const segment_descriptor &)> on_archive = {},
        std::function<void(const segment_descriptor &)> on_delete = {}) {
        return apply_retention_rules_blocking(
            log, runner, max_segment_age, std::move(on_archive), std::move(on_delete));
    }
} // namespace nitya::pravaha_adapter
