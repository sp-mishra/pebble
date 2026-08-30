#pragma once
// ============================================================================
// kalpana/brush/stabilizer.hpp — Stroke Input Stabilization
// ============================================================================
// StrokeStabilizer: a streaming filter fed one BrushPoint at a time, emitting
// smoothed BrushPoints into stroke_segment.
//
// Configurable modes (policy, no virtuals):
//   OneEuro      — adaptive low-pass: heavy smoothing when slow, low lag fast
//                  (Casiez, Roussel & Vogel, CHI 2012)
//   PullLag      — smoothed point trails cursor by spring/rope of length L
//   CatmullRomFit — buffer M raw samples, emit C¹ spline resampled at spacing
//
// strength = 0 → pass-through (bit-identical to unfiltered input).
// All modes are O(1)/sample, zero heap allocations.
// PointFilter concept: BrushPoint operator()(BrushPoint) noexcept streaming.
// ============================================================================

#ifndef KALPANA_BRUSH_STABILIZER_HPP
#define KALPANA_BRUSH_STABILIZER_HPP

#include <kalpana/brush/brush.hpp>
#include <containers/numeric/math_vector.hpp>
#include <cmath>
#include <algorithm>
#include <array>
#include <cstddef>

namespace kalpana {

// ---------------------------------------------------------------------------
// PointFilter concept — all stabilizer modes satisfy this
// ---------------------------------------------------------------------------
template<typename T>
concept PointFilter = requires(T& flt, BrushPoint p) {
    { flt(p) } -> std::same_as<BrushPoint>;
};

// ---------------------------------------------------------------------------
// OneEuroFilter — adaptive low-pass stabilizer
// Reference: Casiez, Roussel & Vogel, CHI 2012
// ---------------------------------------------------------------------------
struct OneEuroFilter {
    // Tuning parameters (sane defaults; user override via StrokeStabilizer)
    float min_cutoff   = 1.0f;   // Hz, cutoff at rest
    float beta         = 0.007f; // speed-adaptive gain
    float d_cutoff     = 1.0f;   // Hz, derivative low-pass cutoff
    float strength     = 1.0f;   // [0,1], 0 = pass-through

    struct State {
        float  x{};           // filtered value
        float  dx{};          // filtered derivative
        float  prev_raw{};    // previous raw value
        bool   initialized{false};
    };

    State sx{}, sy{};  // separate state per axis

    // Apply filter to a single axis value x_raw at rate dt_s (seconds)
    [[nodiscard]] float filter_axis(State& s, float x_raw, float dt_s) noexcept {
        if (!s.initialized) {
            s.x    = x_raw;
            s.dx   = 0.0f;
            s.prev_raw = x_raw;
            s.initialized = true;
            return x_raw;
        }

        // Derivative low-pass
        const float alpha_d = alpha_from_cutoff(d_cutoff, dt_s);
        const float dx_raw  = (x_raw - s.prev_raw) / (dt_s + 1e-9f);
        s.dx = s.dx + alpha_d * (dx_raw - s.dx);
        s.prev_raw = x_raw;

        // Adaptive cutoff
        const float cutoff  = min_cutoff + beta * std::fabs(s.dx);
        const float alpha   = alpha_from_cutoff(cutoff, dt_s);

        // Blend filtered vs raw by strength
        const float filtered = s.x + alpha * (x_raw - s.x);
        s.x = filtered;
        return s.x + (1.0f - strength) * (x_raw - s.x);
    }

    [[nodiscard]] BrushPoint operator()(BrushPoint p) noexcept {
        if (strength < 1e-6f) return p;
        constexpr float DT = 1.0f / 60.0f; // assume 60 Hz
        p.pos[0] = filter_axis(sx, p.pos[0], DT);
        p.pos[1] = filter_axis(sy, p.pos[1], DT);
        return p;
    }

    void reset() noexcept { sx = {}; sy = {}; }

private:
    [[nodiscard]] static float alpha_from_cutoff(float cutoff, float dt) noexcept {
        constexpr float kTwoPi = 6.2831853f;
        const float tau_e = 1.0f / (kTwoPi * cutoff);
        return 1.0f / (1.0f + tau_e / (dt + 1e-9f));
    }
};

static_assert(PointFilter<OneEuroFilter>);

// ---------------------------------------------------------------------------
// PullLagFilter — smoothed point trails cursor by a spring/rope
// ---------------------------------------------------------------------------
struct PullLagFilter {
    float strength = 0.8f;  // [0,1], 0 = pass-through, 1 = max lag
    float rope_len = 8.0f;  // pixels; rope slack before pulling starts

    struct State {
        pebble::math::vec2 anchor{};
        bool               initialized{false};
    } state;

    [[nodiscard]] BrushPoint operator()(BrushPoint p) noexcept {
        if (strength < 1e-6f) return p;

        if (!state.initialized) {
            state.anchor = p.pos;
            state.initialized = true;
            return p;
        }

        const float dx = p.pos[0] - state.anchor[0];
        const float dy = p.pos[1] - state.anchor[1];
        const float dist = std::sqrt(dx*dx + dy*dy);

        if (dist > rope_len) {
            // Pull anchor toward cursor
            const float pull = (dist - rope_len) / dist;
            state.anchor[0] += dx * pull * strength;
            state.anchor[1] += dy * pull * strength;
        }

        BrushPoint out = p;
        out.pos = state.anchor;
        return out;
    }

    void reset() noexcept { state = {}; }
};

static_assert(PointFilter<PullLagFilter>);

// ---------------------------------------------------------------------------
// CatmullRomFilter — buffer M raw samples, emit C¹ spline points
// Emits resampled points at uniform arc-length spacing.
// Buffer flush: on reset() emits remaining buffered points.
// ---------------------------------------------------------------------------
template<std::size_t BufSize = 8>
struct CatmullRomFilter {
    float strength = 0.8f;  // blend factor toward spline output
    float spacing  = 4.0f;  // target arc-length spacing between emitted points

    std::array<BrushPoint, BufSize> buf{};
    std::size_t buf_size{0};
    BrushPoint  last_emitted{};
    bool        last_valid{false};

    [[nodiscard]] BrushPoint operator()(BrushPoint p) noexcept {
        if (strength < 1e-6f) return p;

        if (buf_size < BufSize) {
            buf[buf_size++] = p;
        } else {
            // Shift buffer
            for (std::size_t i = 0; i + 1 < BufSize; ++i) buf[i] = buf[i+1];
            buf[BufSize - 1] = p;
        }

        if (buf_size < 2) return p;

        // Catmull-Rom midpoint: blend raw with spline estimate
        const BrushPoint& p0 = buf[buf_size >= 4 ? buf_size - 4 : 0];
        const BrushPoint& p1 = buf[buf_size >= 3 ? buf_size - 3 : 0];
        const BrushPoint& p2 = buf[buf_size >= 2 ? buf_size - 2 : 0];
        const BrushPoint& p3 = buf[buf_size - 1];

        // t=0.5 Catmull-Rom interpolation
        auto cr = [](float a, float b, float c, float d) noexcept -> float {
            return 0.5f * (-a + 3.0f*b - 3.0f*c + d) * 0.125f
                   + 0.5f * (3.0f*a - 5.0f*b + 4.0f*c - d) * 0.25f
                   + 0.5f * (-a + c) * 0.5f + b;
        };

        BrushPoint smoothed = p3;
        smoothed.pos[0] = cr(p0.pos[0], p1.pos[0], p2.pos[0], p3.pos[0]);
        smoothed.pos[1] = cr(p0.pos[1], p1.pos[1], p2.pos[1], p3.pos[1]);

        BrushPoint out;
        out.pos[0] = p3.pos[0] + strength * (smoothed.pos[0] - p3.pos[0]);
        out.pos[1] = p3.pos[1] + strength * (smoothed.pos[1] - p3.pos[1]);
        out.pressure = p3.pressure;
        out.tilt_x   = p3.tilt_x;
        out.tilt_y   = p3.tilt_y;
        out.velocity = p3.velocity;
        return out;
    }

    void reset() noexcept { buf_size = 0; last_valid = false; }
};

static_assert(PointFilter<CatmullRomFilter<>>);

// ---------------------------------------------------------------------------
// StabilizerMode — tag for selecting filter at compile time
// ---------------------------------------------------------------------------
enum class StabilizerMode { OneEuro, PullLag, CatmullRom, PassThrough };

// ---------------------------------------------------------------------------
// StrokeStabilizer<Mode> — wraps filter, provides apply() streaming interface
// ---------------------------------------------------------------------------
template<StabilizerMode Mode = StabilizerMode::OneEuro>
class StrokeStabilizer {
public:
    static constexpr StabilizerMode mode = Mode;

    StrokeStabilizer() = default;

    // Fluent configuration
    StrokeStabilizer& strength(float s) noexcept {
        if constexpr (Mode == StabilizerMode::OneEuro)
            filter_.strength = s;
        else if constexpr (Mode == StabilizerMode::PullLag)
            filter_.strength = s;
        else if constexpr (Mode == StabilizerMode::CatmullRom)
            filter_.strength = s;
        return *this;
    }

    // Apply filter to a single incoming BrushPoint
    [[nodiscard]] BrushPoint apply(BrushPoint p) noexcept {
        if constexpr (Mode == StabilizerMode::PassThrough)
            return p;
        else
            return filter_(p);
    }

    void reset() noexcept {
        if constexpr (Mode != StabilizerMode::PassThrough)
            filter_.reset();
    }

private:
    using Filter = std::conditional_t<Mode == StabilizerMode::OneEuro,    OneEuroFilter,
                   std::conditional_t<Mode == StabilizerMode::PullLag,    PullLagFilter,
                   std::conditional_t<Mode == StabilizerMode::CatmullRom, CatmullRomFilter<>,
                   OneEuroFilter>>>; // PassThrough uses stub

    Filter filter_{};
};

// Convenience aliases
using OneEuroStabilizer    = StrokeStabilizer<StabilizerMode::OneEuro>;
using PullLagStabilizer    = StrokeStabilizer<StabilizerMode::PullLag>;
using CatmullRomStabilizer = StrokeStabilizer<StabilizerMode::CatmullRom>;

} // namespace kalpana

#endif // KALPANA_BRUSH_STABILIZER_HPP
