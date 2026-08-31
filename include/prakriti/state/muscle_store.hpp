#pragma once
// ============================================================================
// prakriti/state/muscle_store.hpp — SoA muscle constraint storage.
// ============================================================================

#include "../constraints/muscle.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace prakriti {

template <typename Cfg = MuscleConstraintCfg<>>
struct MuscleStore {
    std::vector<Index> origin;
    std::vector<Index> insertion;
    std::vector<Scalar> rest_length;
    std::vector<Scalar> tendon_slack_length;
    std::vector<Scalar> max_isometric_force;
    std::vector<Scalar> activation;
    std::vector<Scalar> pennation_angle;
    std::vector<Scalar> optimal_fiber_length;
    std::vector<Scalar> lambda_accum;

    using FatigueCol = typename Cfg::fatigue::StateColumn;
    [[no_unique_address]] FatigueCol fatigue_col{};

    [[nodiscard]] std::uint32_t add(const MuscleConstraint<Cfg>& c) {
        const std::uint32_t id = static_cast<std::uint32_t>(size());
        origin.push_back(c.origin);
        insertion.push_back(c.insertion);
        rest_length.push_back(c.rest_length);
        tendon_slack_length.push_back(c.tendon_slack_length);
        max_isometric_force.push_back(c.max_isometric_force);
        activation.push_back(c.activation);
        pennation_angle.push_back(c.pennation_angle);
        optimal_fiber_length.push_back(c.optimal_fiber_length);
        lambda_accum.push_back(c.lambda_accum);
        fatigue_col.push_back(c.fatigue_state);
        return id;
    }

    void reserve(std::size_t n) {
        origin.reserve(n);
        insertion.reserve(n);
        rest_length.reserve(n);
        tendon_slack_length.reserve(n);
        max_isometric_force.reserve(n);
        activation.reserve(n);
        pennation_angle.reserve(n);
        optimal_fiber_length.reserve(n);
        lambda_accum.reserve(n);
        fatigue_col.reserve(n);
    }

    void compact() noexcept {
        // SoA is append-only right now; IDs stay stable.
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return origin.size();
    }
};

} // namespace prakriti
