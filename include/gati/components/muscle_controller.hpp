#pragma once
// ============================================================================
// gati/components/muscle_controller.hpp — ECS muscle control components.
// ============================================================================

#include "../math.hpp"

#include <algorithm>
#include <cstdint>
#include <span>
#include <string_view>

namespace gati {

struct MuscleController {
    Scalar activation = 0.0f;
    Scalar activation_time_const = 0.01f;
    Scalar deactivation_time_const = 0.04f;
    std::uint32_t prakriti_constraint_id = 0;

    [[nodiscard]] Scalar neural_step(Scalar neural_target, Scalar dt) noexcept {
        const Scalar u = std::clamp(neural_target, 0.0f, 1.0f);
        const Scalar tau = (u >= activation)
            ? std::max(activation_time_const, 1e-5f)
            : std::max(deactivation_time_const, 1e-5f);
        activation += (u - activation) * (dt / tau);
        activation = std::clamp(activation, 0.0f, 1.0f);
        return activation;
    }
};

struct MuscleGroup {
    std::span<const std::uint32_t> controller_ids{};
    std::string_view name{};
};

} // namespace gati

