#pragma once

// ============================================================================
// sanchaya/sync/cdc_sync.hpp — 7-Step Snapshot + CDC Replication & Autonomous Promotion
// ============================================================================
//
// change_record<BufferPolicy> is parameterised on a storage policy:
//
//   string_view_buffer  (default)
//     table_name and payload are std::string_view — zero-copy, zero allocation.
//     Use on the hot write-behind path when the source page is pinned and will
//     remain alive for the duration of processing.
//
//   owning_string_buffer
//     table_name and payload are std::string — owning copies.
//     Use when the CDC record must outlive the originating page (archival,
//     out-of-band serialisation, cross-thread hand-off).
//
// Convenience aliases:
//   change_record<>            — same as change_record_view (zero-copy)
//   change_record_view         — std::string_view fields (default, zero alloc)
//   change_record_owned        — std::string fields (archival copy)
// ============================================================================

#include "sanchaya/fwd.hpp"
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>

namespace sanchaya::sync {

    struct change_position {
        std::uint64_t sequence{0};
        std::uint64_t lsn{0};
    };

    struct replica_checkpoint {
        change_position              source_position;
        std::chrono::system_clock::time_point source_commit_time;
        std::chrono::system_clock::time_point applied_at;
    };

    enum class change_op : std::uint8_t {
        insert,
        update,
        delete_op
    };

    // ── Buffer policy tags ────────────────────────────────────────────────────

    /// Non-owning view — zero allocation. Source page must stay pinned. (default)
    struct string_view_buffer {};

    /// Owning copy — heap allocation per record. Safe for archival / cross-thread use.
    struct owning_string_buffer {};

    // ── change_record<BufferPolicy> ───────────────────────────────────────────
    template <class BufferPolicy = string_view_buffer>
    struct change_record {
        static_assert(
            std::is_same_v<BufferPolicy, string_view_buffer> ||
            std::is_same_v<BufferPolicy, owning_string_buffer>,
            "BufferPolicy must be string_view_buffer (default) or owning_string_buffer");

        using string_t = std::conditional_t<
            std::is_same_v<BufferPolicy, string_view_buffer>,
            std::string_view,
            std::string
        >;

        std::uint64_t sequence{0};
        change_op     op{change_op::insert};
        string_t      table_name{};
        string_t      payload{};
    };

    /// Zero-copy alias — use on the hot CDC path when source page is pinned.
    using change_record_view  = change_record<string_view_buffer>;

    /// Owning alias — use for archival or cross-thread hand-off.
    using change_record_owned = change_record<owning_string_buffer>;

    // ── Autonomous Promotion Calculator ──────────────────────────────────────
    struct promotion_metrics {
        double predicted_queries{0.0};
        double avg_latency_delta_ms{0.0};
        double remote_savings{0.0};
        double build_cost{0.0};
        double maint_cost{0.0};
        double storage_cost{0.0};
        double schema_maint_cost{0.0};
        double freshness_penalty{0.0};
        double confidence{1.0};
        double payback_period{0.0};
    };

    class autonomous_tiering_evaluator {
    public:
        [[nodiscard]] static constexpr double compute_expected_benefit(
            const promotion_metrics& m) noexcept
        {
            return (m.predicted_queries * m.avg_latency_delta_ms) + m.remote_savings
                   - m.build_cost - m.maint_cost - m.storage_cost
                   - m.schema_maint_cost - m.freshness_penalty;
        }

        [[nodiscard]] static constexpr bool should_promote(
            const promotion_metrics& m,
            double threshold     = 50.0,
            double min_confidence = 0.7,
            double max_payback   = 3600.0) noexcept
        {
            return (compute_expected_benefit(m) > threshold) &&
                   (m.confidence >= min_confidence) &&
                   (m.payback_period <= max_payback);
        }
    };

} // namespace sanchaya::sync
