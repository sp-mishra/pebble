#pragma once
// ============================================================================
// kalpana/brush/dynamics.hpp — Input Signal Dynamics & Parameter Bindings
// ============================================================================
// Binds stylus pressure, velocity, tilt, direction, stroke distance, time,
// or random seed to brush properties using customizable response power curves.
// ============================================================================

#include <cmath>
#include <cstdint>
#include <algorithm>

namespace kalpana {
    enum class DynamicsSource : std::uint8_t {
        Constant, // fixed value
        Pressure, // pen pressure [0, 1]
        Velocity, // stroke speed [0, 1]
        Tilt, // pen tilt magnitude [0, 1]
        Direction, // stroke direction angle [0, 1]
        Distance, // cumulative stroke distance (cyclic or ramp)
        Time, // elapsed stroke time
        Random // per-stamp pseudo-random [0, 1]
    };

    // Snapshot of all input signals at a stamp evaluation point
    struct BrushInputState {
        float pressure = 1.0f; // [0, 1]
        float velocity = 0.0f; // [0, 1]
        float tilt = 0.0f; // [0, 1]
        float direction = 0.0f; // [0, 2π] normalized or cyclic
        float distance = 0.0f; // cumulative distance
        float time = 0.0f; // elapsed time in seconds
        float random = 0.0f; // per-stamp seeded random [0, 1]
    };

    // Maps a DynamicsSource input signal [0, 1] to an output range [lo, hi] with power curve
    struct DynamicsBinding {
        DynamicsSource source = DynamicsSource::Constant;
        float base = 1.0f; // constant fallback value when source == Constant
        float lo = 0.0f; // output range min
        float hi = 1.0f; // output range max
        float curve = 1.0f; // power curve: 1=linear, <1=ease-out, >1=ease-in

        [[nodiscard]] float evaluate(const BrushInputState& input) const noexcept {
            if (source == DynamicsSource::Constant) {
                return base;
            }

            float raw = 0.0f;
            switch (source) {
            case DynamicsSource::Constant: raw = base;
                break;
            case DynamicsSource::Pressure: raw = input.pressure;
                break;
            case DynamicsSource::Velocity: raw = input.velocity;
                break;
            case DynamicsSource::Tilt: raw = input.tilt;
                break;
            case DynamicsSource::Direction: raw = input.direction;
                break;
            case DynamicsSource::Distance: raw = std::fmod(input.distance * 0.01f, 1.0f);
                break;
            case DynamicsSource::Time: raw = std::fmod(input.time, 1.0f);
                break;
            case DynamicsSource::Random: raw = input.random;
                break;
            }

            raw = std::clamp(raw, 0.0f, 1.0f);
            if (curve != 1.0f && raw > 0.0f) {
                raw = std::pow(raw, curve);
            }

            return lo + (hi - lo) * raw;
        }

        friend constexpr bool operator==(const DynamicsBinding&, const DynamicsBinding&) = default;
    };
} // namespace kalpana
