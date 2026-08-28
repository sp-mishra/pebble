#pragma once
// ============================================================================
// spandana/ik.hpp — 2D Analytical Two-Bone & FABRIK Inverse Kinematics
// ============================================================================
// Closed-form exact law of cosines limb solver and iterative reach solver.
// ============================================================================

#include "containers/numeric/math_vector.hpp"
#include "containers/static/static_vector.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>

namespace pebble::spandana {

struct TwoBoneIKResult {
    float angle1 = 0.0f; // Radians
    float angle2 = 0.0f; // Radians
    bool  reachable = true;
};

class TwoBoneIK {
public:
    constexpr TwoBoneIK(float l1, float l2, float bend_dir = 1.0f) noexcept
        : l1_(l1), l2_(l2), bend_dir_(bend_dir) {}

    // Solve for limb angles given root position and target position
    [[nodiscard]] TwoBoneIKResult solve(const pebble::math::vec2& root,
                                        const pebble::math::vec2& target) const noexcept {
        const float dx = target[0] - root[0];
        const float dy = target[1] - root[1];
        const float dist_sq = dx * dx + dy * dy;
        const float dist = std::sqrt(dist_sq);

        const float max_len = l1_ + l2_;
        const float min_len = std::abs(l1_ - l2_);

        if (dist >= max_len) {
            // Target is beyond reach: stretch fully towards target
            const float base_angle = std::atan2(dy, dx);
            return {base_angle, 0.0f, false};
        }

        if (dist <= min_len) {
            // Target is too close
            const float base_angle = std::atan2(dy, dx);
            return {base_angle, std::numbers::pi_v<float>, false};
        }

        // Law of Cosines
        // cos(gamma) = (l1^2 + l2^2 - dist^2) / (2 * l1 * l2)
        const float cos_angle2 = (l1_ * l1_ + l2_ * l2_ - dist_sq) / (2.0f * l1_ * l2_);
        const float clamped_cos2 = std::clamp(cos_angle2, -1.0f, 1.0f);
        const float angle2_internal = std::acos(clamped_cos2);
        const float angle2 = (std::numbers::pi_v<float> - angle2_internal) * bend_dir_;

        // cos(alpha) = (l1^2 + dist^2 - l2^2) / (2 * l1 * dist)
        const float cos_alpha = (l1_ * l1_ + dist_sq - l2_ * l2_) / (2.0f * l1_ * dist);
        const float clamped_cos_alpha = std::clamp(cos_alpha, -1.0f, 1.0f);
        const float alpha = std::acos(clamped_cos_alpha);

        const float base_angle = std::atan2(dy, dx);
        const float angle1 = base_angle - (alpha * bend_dir_);

        return {angle1, angle2, true};
    }

private:
    float l1_ = 10.0f;
    float l2_ = 10.0f;
    float bend_dir_ = 1.0f; // +1.0 for standard bend, -1.0 for inverted
};

} // namespace pebble::spandana
