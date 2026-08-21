#pragma once
// ============================================================================
// spandana/blend_space.hpp — Parametric 2D Directional Blend Spaces & Locomotion
// ============================================================================
// Maps velocity vectors (vx, vy) to multi-clip animation sample weights with
// smooth phase synchronization across locomotion cycles (Walk, Run, Strafe, Turn).
// ============================================================================

#include "flipbook.hpp"
#include "containers/numeric/math_vector.hpp"
#include <vector>
#include <cmath>
#include <algorithm>

namespace pebble::spandana {

struct BlendSample2D {
    pebble::math::vec2  coordinate{0.0f, 0.0f}; // e.g. (0, 0)=Idle, (0, 1)=Walk, (0, 3)=Run, (1, 0)=StrafeRight
    const FlipbookClip* clip = nullptr;
};

struct WeightedClipSample {
    const FlipbookClip* clip = nullptr;
    float               weight = 0.0f;
};

class BlendSpace2D {
public:
    BlendSpace2D() = default;

    void add_sample(pebble::math::vec2 coord, const FlipbookClip* clip) {
        samples_.push_back(BlendSample2D{coord, clip});
    }

    // Evaluate normalized blend weights given current 2D velocity vector
    [[nodiscard]] std::vector<WeightedClipSample> evaluate_weights(
        const pebble::math::vec2& velocity) const noexcept {

        if (samples_.empty()) return {};
        if (samples_.size() == 1) {
            return {{samples_[0].clip, 1.0f}};
        }

        std::vector<WeightedClipSample> result;
        result.reserve(samples_.size());

        // 1. Check for exact coordinate matches
        for (const auto& sample : samples_) {
            const float dx = velocity[0] - sample.coordinate[0];
            const float dy = velocity[1] - sample.coordinate[1];
            if (dx * dx + dy * dy < 1e-6f) {
                result.push_back({sample.clip, 1.0f});
                return result;
            }
        }

        // 2. Inverse Distance Weighting (Shepard's Method with power p=2)
        float total_weight = 0.0f;
        for (const auto& sample : samples_) {
            const float dx = velocity[0] - sample.coordinate[0];
            const float dy = velocity[1] - sample.coordinate[1];
            const float dist_sq = dx * dx + dy * dy;
            const float w = 1.0f / std::max(dist_sq, 1e-4f);

            result.push_back({sample.clip, w});
            total_weight += w;
        }

        // 3. Normalize weights (sum w_i = 1.0)
        if (total_weight > 0.0f) {
            const float inv_total = 1.0f / total_weight;
            for (auto& item : result) {
                item.weight *= inv_total;
            }
        }

        return result;
    }

    [[nodiscard]] std::size_t sample_count() const noexcept {
        return samples_.size();
    }

private:
    std::vector<BlendSample2D> samples_;
};

// Evaluator with phase synchronization to eliminate foot sliding
class BlendSpaceAnimator {
public:
    explicit BlendSpaceAnimator(const BlendSpace2D* space = nullptr)
        : space_(space) {}

    void set_blend_space(const BlendSpace2D* space) noexcept {
        space_ = space;
    }

    void set_velocity(const pebble::math::vec2& v) noexcept {
        velocity_ = v;
    }

    void update(float dt) noexcept {
        if (!space_) return;

        // Phase advance based on speed
        const float speed = std::sqrt(velocity_[0] * velocity_[0] + velocity_[1] * velocity_[1]);
        const float playback_rate = std::max(1.0f, speed);
        phase_ += dt * playback_rate;
        if (phase_ > 1000.0f) phase_ -= 1000.0f; // Wrap phase cleanly
    }

    [[nodiscard]] std::vector<WeightedClipSample> current_weights() const noexcept {
        return space_ ? space_->evaluate_weights(velocity_) : std::vector<WeightedClipSample>{};
    }

    [[nodiscard]] float phase() const noexcept { return phase_; }
    [[nodiscard]] const pebble::math::vec2& velocity() const noexcept { return velocity_; }

private:
    const BlendSpace2D* space_ = nullptr;
    pebble::math::vec2  velocity_{0.0f, 0.0f};
    float               phase_ = 0.0f;
};

} // namespace pebble::spandana
