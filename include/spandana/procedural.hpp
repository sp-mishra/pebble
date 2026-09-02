#pragma once
// ============================================================================
// spandana/procedural.hpp — Procedural Camera Shakes, Trauma & Secondary Jiggle
// ============================================================================
// ScreenShake2D: Rotational & directional trauma with exponential decay.
// JigglePoint2D: Harmonic secondary follower for bouncy accessories.
// ============================================================================

#include "spring.hpp"
#include "containers/numeric/math_vector.hpp"
#include <algorithm>
#include <cmath>

namespace pebble::spandana {
    class ScreenShake2D {
    public:
        constexpr explicit ScreenShake2D(float max_offset = 20.0f, float max_angle = 0.1f) noexcept
            : max_offset_(max_offset), max_angle_(max_angle) {}

        void add_trauma(float amount) noexcept {
            trauma_ = std::clamp(trauma_ + amount, 0.0f, 1.0f);
        }

        void update(float dt, float frequency = 25.0f, float decay_rate = 1.2f) noexcept {
            time_ += dt * frequency;
            trauma_ = std::max(0.0f, trauma_ - decay_rate * dt);
        }

        // Current shake offset (x, y)
        [[nodiscard]] pebble::math::vec2 offset() const noexcept {
            if (trauma_ <= 0.0f) return pebble::math::vec2(0.0f, 0.0f);
            const float shake = trauma_ * trauma_; // Non-linear shake intensity
            const float ox = max_offset_ * shake * pseudo_noise(time_);
            const float oy = max_offset_ * shake * pseudo_noise(time_ + 100.0f);
            return pebble::math::vec2(ox, oy);
        }

        // Current rotational shake angle in radians
        [[nodiscard]] float angle() const noexcept {
            if (trauma_ <= 0.0f) return 0.0f;
            const float shake = trauma_ * trauma_;
            return max_angle_ * shake * pseudo_noise(time_ + 200.0f);
        }

        [[nodiscard]] float trauma() const noexcept { return trauma_; }

    private:
        [[nodiscard]] static float pseudo_noise(float t) noexcept {
            return std::sin(t) * 0.5f + std::sin(t * 2.3f) * 0.3f + std::sin(t * 5.7f) * 0.2f;
        }

        float max_offset_ = 20.0f;
        float max_angle_ = 0.1f;
        float trauma_ = 0.0f;
        float time_ = 0.0f;
    };

    class JigglePoint2D {
    public:
        constexpr explicit JigglePoint2D(float stiffness = 220.0f, float damping = 14.0f) noexcept
            : spring_(stiffness, damping) {}

        void update(const pebble::math::vec2& target_pos, float dt) noexcept {
            auto [pos, vel] = spring_.step(pos_, vel_, target_pos, dt);
            pos_ = pos;
            vel_ = vel;
        }

        [[nodiscard]] const pebble::math::vec2& position() const noexcept { return pos_; }
        [[nodiscard]] const pebble::math::vec2& velocity() const noexcept { return vel_; }

        void set_position(const pebble::math::vec2& p) noexcept {
            pos_ = p;
            vel_ = pebble::math::vec2(0.0f, 0.0f);
        }

    private:
        Vector2SpringDamper spring_;
        pebble::math::vec2 pos_{};
        pebble::math::vec2 vel_{};
    };
} // namespace pebble::spandana
