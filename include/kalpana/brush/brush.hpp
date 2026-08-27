#pragma once
// ============================================================================
// kalpana/brush/brush.hpp — Unified Spectral Brush Pipeline & Dynamics
// ============================================================================
// Zero-virtual, policy-configurable brush pipeline combining stamp shape,
// dynamic signal binding, spectral pigment science, physical deposition, and
// configurable small-buffer inline emission.
// ============================================================================

#include "../color/color.hpp"
#include "../color/spectral.hpp"
#include "containers/numeric/math_vector.hpp"
#include "containers/dynamic/SmallVector.hpp"
#include "dynamics.hpp"
#include "stamp_shape.hpp"
#include "deposition.hpp"
#include "brush_preset.hpp"

#include <vector>
#include <cmath>
#include <algorithm>

namespace kalpana {

struct BrushPoint {
    pebble::math::vec2 pos{0.0f, 0.0f};
    float              pressure = 1.0f;  // [0, 1]
    float              tilt_x   = 0.0f;  // [-1, 1]
    float              tilt_y   = 0.0f;  // [-1, 1]
    float              velocity = 0.0f;  // normalized speed
};

struct BrushStamp {
    pebble::math::vec2       pos{0.0f, 0.0f};
    float                    radius    = 5.0f;
    float                    opacity   = 1.0f;
    Color                    color     = colors::black();
    spectral::SpectralColor  pigment   = spectral::SpectralColor::from_color(colors::black());
    float                    angle     = 0.0f;
    float                    roundness = 1.0f;
    float                    hardness  = 0.8f;

    friend constexpr bool operator==(const BrushStamp&, const BrushStamp&) = default;
};

// Default inline stamp buffer capacity (256 bytes ~ 4-6 stamps before heap spill)
template <
    typename StampPolicy = StampShape<StampPreset::Round>,
    typename StampContainer = containers::dynamic::SmallVector<BrushStamp, 256>
>
class BrushPipeline {
public:
    using stamp_policy_type = StampPolicy;
    using container_type = StampContainer;

    BrushPipeline() = default;

    // Fluent Configuration API
    BrushPipeline& size(float s) noexcept { size_ = s; return *this; }
    BrushPipeline& spacing(float sp) noexcept { spacing_ = sp; return *this; }

    // Pigment and Color
    BrushPipeline& pigment(spectral::SpectralColor p) noexcept {
        pigment_ = p;
        color_ = p.to_color();
        return *this;
    }

    BrushPipeline& color(Color c) noexcept {
        color_ = c;
        pigment_ = spectral::SpectralColor::from_color(c);
        return *this;
    }

    // Dynamics Bindings Access
    [[nodiscard]] DynamicsBinding& flow() noexcept { return flow_dyn_; }
    [[nodiscard]] const DynamicsBinding& flow() const noexcept { return flow_dyn_; }
    [[nodiscard]] DynamicsBinding& opacity() noexcept { return opacity_dyn_; }
    [[nodiscard]] const DynamicsBinding& opacity() const noexcept { return opacity_dyn_; }
    [[nodiscard]] DynamicsBinding& scatter() noexcept { return scatter_dyn_; }
    [[nodiscard]] const DynamicsBinding& scatter() const noexcept { return scatter_dyn_; }
    [[nodiscard]] DynamicsBinding& rotation() noexcept { return rotation_dyn_; }
    [[nodiscard]] const DynamicsBinding& rotation() const noexcept { return rotation_dyn_; }
    [[nodiscard]] DynamicsBinding& size_dyn() noexcept { return size_dyn_; }
    [[nodiscard]] const DynamicsBinding& size_dyn() const noexcept { return size_dyn_; }

    // Deposition & Presets
    BrushPipeline& deposit_mode(deposit::Mode m) noexcept {
        deposit_.mode = m;
        return *this;
    }

    BrushPipeline& deposit_params(deposit::DepositionParams p) noexcept {
        deposit_ = p;
        return *this;
    }

    BrushPipeline& water_params(WaterPhysicsParams w) noexcept {
        water_ = w;
        return *this;
    }

    BrushPipeline& impasto_params(PigmentImpastoParams ip) noexcept {
        impasto_ = ip;
        return *this;
    }

    BrushPipeline& apply_preset(const BrushPreset& preset) noexcept {
        size_ = preset.default_size;
        spacing_ = preset.spacing;
        deposit_ = preset.deposition;
        water_ = preset.water;
        impasto_ = preset.impasto;
        return *this;
    }

    // Stamp Shape Policy Access
    [[nodiscard]] StampPolicy& stamp() noexcept { return stamp_; }
    [[nodiscard]] const StampPolicy& stamp() const noexcept { return stamp_; }

    [[nodiscard]] float size() const noexcept { return size_; }
    [[nodiscard]] float spacing() const noexcept { return spacing_; }
    [[nodiscard]] Color get_color() const noexcept { return color_; }
    [[nodiscard]] spectral::SpectralColor get_pigment() const noexcept { return pigment_; }

    // Stroke evaluation generating dabs along a line trajectory
    [[nodiscard]] StampContainer stroke_segment(
        const BrushPoint& p0, const BrushPoint& p1,
        float cumulative_distance = 0.0f, float elapsed_time = 0.0f) const {
        StampContainer stamps;

        const float dx = p1.pos[0] - p0.pos[0];
        const float dy = p1.pos[1] - p0.pos[1];
        const float dist = std::sqrt(dx * dx + dy * dy);

        const float step = std::max(0.5f, size_ * spacing_);
        const int count = static_cast<int>(dist / step);

        auto make_stamp = [&](float t, const pebble::math::vec2& pos, float press, float vel, float tilt) {
            BrushInputState input{
                .pressure = press,
                .velocity = vel,
                .tilt = tilt,
                .direction = (dist > 1e-4f) ? (std::atan2(dy, dx) / (2.0f * 3.14159265f) + 0.5f) : 0.0f,
                .distance = cumulative_distance + dist * t,
                .time = elapsed_time + t * 0.016f,
                .random = std::fmod(std::sin((cumulative_distance + dist * t) * 12.9898f) * 43758.5453f, 1.0f)
            };
            if (input.random < 0.0f) input.random += 1.0f;

            const float evaluated_size = size_ * size_dyn_.evaluate(input);
            const float evaluated_opacity = opacity_dyn_.evaluate(input) * flow_dyn_.evaluate(input);
            const float scatter_amt = scatter_dyn_.evaluate(input) * size_;
            const float rot_angle = rotation_dyn_.evaluate(input);

            pebble::math::vec2 offset_pos = pos;
            if (scatter_amt > 1e-4f) {
                const float s_angle = input.random * 6.2831853f;
                offset_pos[0] += std::cos(s_angle) * scatter_amt;
                offset_pos[1] += std::sin(s_angle) * scatter_amt;
            }

            stamps.push_back(BrushStamp{
                .pos = offset_pos,
                .radius = std::max(0.5f, evaluated_size * 0.5f),
                .opacity = std::clamp(evaluated_opacity, 0.0f, 1.0f),
                .color = color_,
                .pigment = pigment_,
                .angle = stamp_.angle + rot_angle,
                .roundness = stamp_.roundness,
                .hardness = stamp_.hardness
            });
        };

        if (count <= 0) {
            const float tilt_mag = std::sqrt(p1.tilt_x * p1.tilt_x + p1.tilt_y * p1.tilt_y);
            make_stamp(1.0f, p1.pos, p1.pressure, p1.velocity, tilt_mag);
            return stamps;
        }

        for (int i = 0; i <= count; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(count);
            const pebble::math::vec2 p(p0.pos[0] + dx * t, p0.pos[1] + dy * t);
            const float press = p0.pressure + (p1.pressure - p0.pressure) * t;
            const float vel = p0.velocity + (p1.velocity - p0.velocity) * t;
            const float tx = p0.tilt_x + (p1.tilt_x - p0.tilt_x) * t;
            const float ty = p0.tilt_y + (p1.tilt_y - p0.tilt_y) * t;
            const float tilt = std::sqrt(tx * tx + ty * ty);

            make_stamp(t, p, press, vel, tilt);
        }

        return stamps;
    }

    // Smudge / pigment pickup helper
    [[nodiscard]] spectral::SpectralColor smudge_sample(
        const spectral::SpectralColor& surface,
        const spectral::SpectralColor& brush_pigment,
        float pickup_rate) const noexcept {
        const float rate = std::clamp(pickup_rate * impasto_.smudge_rate, 0.0f, 1.0f);
        return brush_pigment.mix_km(surface, rate);
    }

private:
    float                   size_    = 10.0f;
    float                   spacing_ = 0.25f;
    Color                   color_   = colors::black();
    spectral::SpectralColor pigment_ = spectral::SpectralColor::from_color(colors::black());
    StampPolicy             stamp_{};
    deposit::DepositionParams deposit_{};
    WaterPhysicsParams      water_{};
    PigmentImpastoParams    impasto_{};

    DynamicsBinding flow_dyn_{.source = DynamicsSource::Pressure, .base = 1.0f, .lo = 0.0f, .hi = 1.0f};
    DynamicsBinding opacity_dyn_{.source = DynamicsSource::Constant, .base = 1.0f, .lo = 0.0f, .hi = 1.0f};
    DynamicsBinding scatter_dyn_{.source = DynamicsSource::Constant, .base = 0.0f, .lo = 0.0f, .hi = 1.0f};
    DynamicsBinding rotation_dyn_{.source = DynamicsSource::Constant, .base = 0.0f, .lo = 0.0f, .hi = 6.2831853f};
    DynamicsBinding size_dyn_{.source = DynamicsSource::Pressure, .base = 1.0f, .lo = 0.2f, .hi = 1.0f};
};

// Ready-to-use Spectral Brush Alias
using SpectralBrush = BrushPipeline<StampShape<StampPreset::Round>>;

} // namespace kalpana
