#pragma once
// Optional Pravaha integration for Nitya maintenance work.
//
// This header deliberately does not participate in nitya/nitya.hpp: WAL
// reserve/publish/durability remains dependency-free and synchronous at its
// correctness boundary. Include this file only when scheduling recovery or
// retention through Pravaha is desired.

#include "nitya/nitya.hpp"
#include "pravaha/pravaha.hpp"

#include <chrono>
#include <functional>
#include <string_view>

namespace nitya::pravaha_adapter {
    template <typename Wal, typename Runner, typename RecordCallback>
    [[nodiscard]] Result<recovery_status> recover_async(
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

    template <typename Wal, typename Runner>
    [[nodiscard]] Result<void> apply_retention_rules_async(
        Wal& log,
        Runner& runner,
        const std::chrono::seconds max_segment_age,
        std::function<void(const segment_descriptor&)> on_archive = {},
        std::function<void(const segment_descriptor&)> on_delete = {}) {
        auto work = [&log, max_segment_age, on_archive = std::move(on_archive), on_delete = std::move(on_delete)] {
            log.apply_retention_rules(max_segment_age, on_archive, on_delete);
        };

        auto submitted = runner.submit(::pravaha::task("nitya_retention", std::move(work)));
        if (!submitted) return std::unexpected(LogError::InternalError);
        runner.backend_ref().drain();
        return {};
    }
} // namespace nitya::pravaha_adapter
