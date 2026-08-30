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
#include <numbers>
#include <utility>

namespace pebble::spandana {

// Constexpr square root (Newton–Raphson) so spring constants can be cached at
// construction in a constant-evaluated context. Falls back identically to
// std::sqrt precision at runtime after a few iterations.
[[nodiscard]] constexpr float sqrt_constexpr(float x) noexcept {
    if (x <= 0.0f) return 0.0f;
    float g = x;
    for (int i = 0; i < 12; ++i) g = 0.5f * (g + x / g);
    return g;
}

class AnalyticalSpringDamper {
public:
    constexpr explicit AnalyticalSpringDamper(float stiffness = 180.0f, float damping = 12.0f) noexcept
        : k_(stiffness), c_(damping),
          omega0_(sqrt_constexpr(stiffness)),
          zeta_(damping / (2.0f * sqrt_constexpr(stiffness))) {}

    // Step the spring: returns { new_position, new_velocity }
    [[nodiscard]] std::pair<float, float> step(float current, float velocity, float target, float dt) const noexcept {
        if (dt <= 0.0f) return {current, velocity};

        const float x0 = current - target;
        const float v0 = velocity;

        // omega0 (natural angular frequency) and zeta (damping ratio) are fixed
        // by the spring constants — cached at construction, not recomputed here.
        const float omega0 = omega0_;
        const float zeta   = zeta_;

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
    float omega0_ = sqrt_constexpr(180.0f);
    float zeta_   = 12.0f / (2.0f * sqrt_constexpr(180.0f));
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

// Angular Spring Damper — springs toward a target angle along the shortest arc.
// Wraps the error into (-pi, pi] before integrating so a spring from 350° to
// 10° travels +20° instead of -340°. Output angle is not re-wrapped, matching
// AnalyticalSpringDamper's positional semantics (callers wrap for display).
class AngleSpringDamper {
public:
    constexpr explicit AngleSpringDamper(float stiffness = 180.0f, float damping = 12.0f) noexcept
        : spring_(stiffness, damping) {}

    [[nodiscard]] std::pair<float, float>
    step(float current, float velocity, float target, float dt) const noexcept {
        // Shortest-arc target relative to current: current + wrapped_error.
        const float shortest_target = current + wrap_pi(target - current);
        return spring_.step(current, velocity, shortest_target, dt);
    }

private:
    static constexpr float kPi  = std::numbers::pi_v<float>;
    static constexpr float kTau = 2.0f * kPi;

    [[nodiscard]] static float wrap_pi(float a) noexcept {
        a = std::fmod(a + kPi, kTau);
        if (a < 0.0f) a += kTau;
        return a - kPi;
    }

    AnalyticalSpringDamper spring_;
};

} // namespace pebble::spandana
