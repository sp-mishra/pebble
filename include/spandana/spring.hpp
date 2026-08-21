#pragma once
// ============================================================================
// spandana/spring.hpp — Closed-Form Analytical Damped Harmonic Oscillator
// ============================================================================
// Exact closed-form integration across all damping regimes:
//   - Under-damped (zeta < 1): oscillatory with exponential envelope
//   - Critically-damped (zeta = 1): fastest decay without overshoot
//   - Over-damped (zeta > 1): smooth non-oscillatory return
// Zero numerical instability and zero drift across variable frame rates.
// ============================================================================

#include "concepts.hpp"
#include <cmath>
#include <utility>

namespace pebble::spandana {

class AnalyticalSpringDamper {
public:
    constexpr explicit AnalyticalSpringDamper(float stiffness = 180.0f, float damping = 12.0f) noexcept
        : k_(stiffness), c_(damping) {}

    // Step the spring: returns { new_position, new_velocity }
    [[nodiscard]] std::pair<float, float> step(float current, float velocity, float target, float dt) const noexcept {
        if (dt <= 0.0f) return {current, velocity};

        const float x0 = current - target;
        const float v0 = velocity;

        const float omega0 = std::sqrt(k_); // Natural angular frequency
        const float zeta   = c_ / (2.0f * omega0); // Damping ratio

        float x_new = 0.0f;
        float v_new = 0.0f;

        if (zeta < 0.9999f) {
            // ── Under-damped (oscillatory) ──────────────────────────────────
            const float omega_d = omega0 * std::sqrt(1.0f - zeta * zeta);
            const float decay   = std::exp(-zeta * omega0 * dt);
            const float c_cos   = std::cos(omega_d * dt);
            const float c_sin   = std::sin(omega_d * dt);

            const float a = x0;
            const float b = (v0 + zeta * omega0 * x0) / omega_d;

            x_new = decay * (a * c_cos + b * c_sin) + target;
            v_new = decay * ((b * omega_d - a * zeta * omega0) * c_cos -
                             (a * omega_d + b * zeta * omega0) * c_sin);
        } else if (zeta > 1.0001f) {
            // ── Over-damped (non-oscillatory) ───────────────────────────────
            const float alpha = omega0 * std::sqrt(zeta * zeta - 1.0f);
            const float r1    = -zeta * omega0 + alpha;
            const float r2    = -zeta * omega0 - alpha;

            const float c2 = (v0 - r1 * x0) / (r2 - r1);
            const float c1 = x0 - c2;

            const float exp_r1 = std::exp(r1 * dt);
            const float exp_r2 = std::exp(r2 * dt);

            x_new = (c1 * exp_r1 + c2 * exp_r2) + target;
            v_new = c1 * r1 * exp_r1 + c2 * r2 * exp_r2;
        } else {
            // ── Critically damped (zeta = 1) ────────────────────────────────
            const float decay = std::exp(-omega0 * dt);
            const float a = x0;
            const float b = v0 + omega0 * x0;

            x_new = decay * (a + b * dt) + target;
            v_new = decay * (b - omega0 * (a + b * dt));
        }

        return {x_new, v_new};
    }

    [[nodiscard]] constexpr float stiffness() const noexcept { return k_; }
    [[nodiscard]] constexpr float damping() const noexcept { return c_; }

private:
    float k_ = 180.0f;
    float c_ = 12.0f;
};

// 2D Vector Spring Damper
class Vector2SpringDamper {
public:
    constexpr explicit Vector2SpringDamper(float stiffness = 180.0f, float damping = 12.0f) noexcept
        : spring_(stiffness, damping) {}

    [[nodiscard]] std::pair<pebble::math::vec2, pebble::math::vec2>
    step(const pebble::math::vec2& current, const pebble::math::vec2& velocity,
         const pebble::math::vec2& target, float dt) const noexcept {
        auto [px, vx] = spring_.step(current[0], velocity[0], target[0], dt);
        auto [py, vy] = spring_.step(current[1], velocity[1], target[1], dt);
        return {pebble::math::vec2(px, py), pebble::math::vec2(vx, vy)};
    }

private:
    AnalyticalSpringDamper spring_;
};

} // namespace pebble::spandana
