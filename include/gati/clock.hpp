#pragma once
// ============================================================================
// gati/clock.hpp — Fixed-Timestep Game Clock (Gaffer "Fix Your Timestep")
// ============================================================================
// Decouples simulation rate (deterministic) from presentation rate (smooth).
// ============================================================================

#include "math.hpp"
#include <cstdint>

namespace gati {

struct ClockConfig {
    Scalar   hz = Scalar(60);               // fixed simulation frequency (steps per second)
    Scalar   max_frame_dt = Scalar(0.25);   // clamp on single advance() — spiral-of-death guard
    int      max_steps = 8;                 // cap fixed steps drained per frame
};

class Clock {
public:
    constexpr explicit Clock(ClockConfig cfg = {}) noexcept
        : dt_(Scalar(1) / cfg.hz), max_frame_dt_(cfg.max_frame_dt), max_steps_(cfg.max_steps) {}

    constexpr explicit Clock(Scalar hz) noexcept : Clock(ClockConfig{.hz = hz}) {}

    // Fold real wall-clock elapsed time into the accumulator.
    constexpr void advance(Scalar real_dt) noexcept {
        if (real_dt > max_frame_dt_) real_dt = max_frame_dt_;
        if (real_dt < Scalar(0)) real_dt = Scalar(0);
        accumulator_ += real_dt;
        steps_this_frame_ = 0;
    }

    // Drains one fixed dt if a full step is available and per-frame cap is not hit.
    [[nodiscard]] constexpr bool should_step() noexcept {
        if (accumulator_ < dt_ || steps_this_frame_ >= max_steps_) return false;
        accumulator_ -= dt_;
        ++steps_this_frame_;
        ++total_steps_;
        return true;
    }

    [[nodiscard]] constexpr Scalar dt() const noexcept { return dt_; }

    // Interpolation factor in [0, 1) between previous and current simulation step
    [[nodiscard]] constexpr Scalar alpha() const noexcept { return accumulator_ / dt_; }

    [[nodiscard]] constexpr std::uint64_t total_steps() const noexcept { return total_steps_; }
    [[nodiscard]] constexpr int steps_last_frame() const noexcept { return steps_this_frame_; }

private:
    Scalar        dt_;
    Scalar        max_frame_dt_;
    int           max_steps_;
    Scalar        accumulator_ = Scalar(0);
    int           steps_this_frame_ = 0;
    std::uint64_t total_steps_ = 0;
};

} // namespace gati
