#pragma once

// ============================================================================
// sanchaya/planner/cost_model.hpp — Multidimensional Placement Cost Model & IR Properties
// ============================================================================

#include "sanchaya/fwd.hpp"
#include <cstdint>
#include <cstddef>

namespace sanchaya::optimizer {

    enum class objective_goal : std::uint8_t {
        latency_first,
        throughput_first,
        resource_balanced,
        offline_first
    };

    enum class confidence_level : std::uint8_t {
        uncalibrated,
        low,
        medium,
        high,
        exact
    };

    struct plan_cost {
        double startup_latency_ms{0.0};
        double total_latency_ms{0.0};
        double normalized_cpu_cost{0.0};
        double io_bytes{0.0};
        double network_transfer_bytes{0.0};
        double peak_memory_bytes{0.0};
        double temporary_storage_bytes{0.0};
        double freshness_penalty{0.0};
        double coordination_risk{0.0};
        confidence_level confidence{confidence_level::uncalibrated};

        [[nodiscard]] constexpr double balanced_score(
            double latency_ref, double memory_budget, double network_budget) const noexcept
        {
            const double latency_score = total_latency_ms / (latency_ref > 0.0 ? latency_ref : 1.0);
            const double memory_score  = peak_memory_bytes / (memory_budget > 0.0 ? memory_budget : 1.0);
            const double network_score = network_transfer_bytes / (network_budget > 0.0 ? network_budget : 1.0);
            return (0.4 * latency_score) + (0.3 * memory_score) + (0.2 * network_score) + (0.1 * coordination_risk);
        }
    };

    struct ordering_property {
        std::size_t field_index{0};
        sort_direction direction{sort_direction::ascending};
        null_placement nulls{null_placement::nulls_last};
        collation_type collation{collation_type::binary};
    };

    struct key_set {
        containers::SmallVector<std::size_t, 4> field_indices;
    };

    struct logical_properties {
        std::size_t estimated_cardinality{0};
        containers::SmallVector<key_set, 2> unique_keys{};
        containers::SmallVector<ordering_property, 2> orderings{};
        std::uint64_t nullability_mask{0};
        consistency_requirement required_consistency{consistency_requirement::eventual};
        residency_requirement required_residency{residency_requirement::any};
        std::uint64_t plan_fingerprint{0};
    };

    class multidimensional_cost_model {
    public:
        [[nodiscard]] constexpr plan_cost estimate(const logical_properties& props) const noexcept {
            plan_cost cost{};
            cost.total_latency_ms = static_cast<double>(props.estimated_cardinality) * 0.001;
            cost.peak_memory_bytes = static_cast<double>(props.estimated_cardinality) * 64.0;
            cost.confidence = confidence_level::medium;
            return cost;
        }
    };

} // namespace sanchaya::optimizer
