#pragma once

// ============================================================================
// sanchaya/sync/cdc_sync.hpp — 7-Step Snapshot + CDC Replication & Autonomous Promotion
// ============================================================================

#include "sanchaya/fwd.hpp"
#include <chrono>
#include <cstdint>
#include <vector>
#include <string>

namespace sanchaya::sync {

    struct change_position {
        std::uint64_t sequence{0};
        std::uint64_t lsn{0};
    };

    struct replica_checkpoint {
        change_position source_position;
        std::chrono::system_clock::time_point source_commit_time;
        std::chrono::system_clock::time_point applied_at;
    };

    enum class change_op : std::uint8_t {
        insert,
        update,
        delete_op
    };

    struct change_record {
        std::uint64_t sequence{0};
        change_op op{change_op::insert};
        std::string table_name;
        std::string payload;
    };

    // Autonomous Promotion Calculator
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
        [[nodiscard]] static constexpr double compute_expected_benefit(const promotion_metrics& m) noexcept {
            return (m.predicted_queries * m.avg_latency_delta_ms) + m.remote_savings
                   - m.build_cost - m.maint_cost - m.storage_cost - m.schema_maint_cost - m.freshness_penalty;
        }

        [[nodiscard]] static constexpr bool should_promote(
            const promotion_metrics& m,
            double threshold = 50.0,
            double min_confidence = 0.7,
            double max_payback = 3600.0) noexcept
        {
            return (compute_expected_benefit(m) > threshold) &&
                   (m.confidence >= min_confidence) &&
                   (m.payback_period <= max_payback);
        }
    };

} // namespace sanchaya::sync
