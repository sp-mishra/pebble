#pragma once
// ============================================================================
// dhvani/spatial.hpp — 2D Spatial Audio Attenuation & Stereo Panning
// ============================================================================
// Zero-virtual, constexpr-enabled, header-only 2D spatial acoustic calculations.
// ============================================================================

#include "containers/numeric/math_vector.hpp"
#include <algorithm>
#include <cmath>

namespace pebble::dhvani {

struct AudioListener2D {
    pebble::math::vec2 position{0.0f, 0.0f};
    pebble::math::vec2 forward{0.0f, 1.0f}; // Heading direction
    float              max_distance = 600.0f;
    float              ref_distance = 50.0f;
    float              rolloff = 1.0f;
};

struct SpatialAudioOutput {
    float volume_left = 1.0f;
    float volume_right = 1.0f;
    float attenuation = 1.0f;
    float pan = 0.0f; // -1.0 (full left) to +1.0 (full right)
};

// Calculate 2D distance attenuation & stereo panning
[[nodiscard]] inline SpatialAudioOutput compute_spatial_audio(
    const pebble::math::vec2& emitter_pos,
    const AudioListener2D& listener,
    float base_volume = 1.0f) noexcept {

    const float dx = emitter_pos[0] - listener.position[0];
    const float dy = emitter_pos[1] - listener.position[1];
    const float dist = std::sqrt(dx * dx + dy * dy);

    if (dist >= listener.max_distance) {
        return SpatialAudioOutput{0.0f, 0.0f, 0.0f, 0.0f};
    }

    // 1. Logarithmic / Clamped Linear Distance Attenuation
    float atten = 1.0f;
    if (dist > listener.ref_distance) {
        const float dist_range = listener.max_distance - listener.ref_distance;
        const float d = dist - listener.ref_distance;
        atten = std::clamp(1.0f - (d / dist_range) * listener.rolloff, 0.0f, 1.0f);
    }

    // 2. Stereo Panning based on perpendicular offset to forward vector
    // Listener right vector = (forward.y, -forward.x)
    const float right_x = listener.forward[1];
    const float right_y = -listener.forward[0];

    float pan = 0.0f;
    if (dist > 1e-4f) {
        const float dir_x = dx / dist;
        const float dir_y = dy / dist;
        pan = std::clamp(dir_x * right_x + dir_y * right_y, -1.0f, 1.0f);
    }

    // Equal-power stereo panning law
    // Left = cos((pan + 1) * pi / 4), Right = sin((pan + 1) * pi / 4)
    constexpr float kPiOver4 = 0.785398163f;
    const float angle = (pan + 1.0f) * kPiOver4;
    const float left_gain = std::cos(angle);
    const float right_gain = std::sin(angle);

    const float final_vol = base_volume * atten;

    return SpatialAudioOutput{
        .volume_left = final_vol * left_gain,
        .volume_right = final_vol * right_gain,
        .attenuation = atten,
        .pan = pan
    };
}

} // namespace pebble::dhvani
