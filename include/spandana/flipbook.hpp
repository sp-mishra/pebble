#pragma once
// ============================================================================
// spandana/flipbook.hpp — 2D Sprite Flipbook Engine & Atlas Grid Slicing
// ============================================================================
// Frame rate control, loop modes (Once, Loop, PingPong), frame events.
// ============================================================================

#include "containers/static/static_vector.hpp"
#include "containers/numeric/math_vector.hpp"
#include <cstdint>
#include <string_view>
#include <cmath>

namespace pebble::spandana {

enum class LoopMode : std::uint8_t { Once, Loop, PingPong };

struct FrameEvent {
    std::uint32_t    frame_index = 0;
    std::string_view name;
};

// Represents a 2D Sprite Sheet Animation Clip
struct FlipbookClip {
    std::string_view name;
    std::uint32_t    start_frame = 0;
    std::uint32_t    frame_count = 1;
    float            fps = 12.0f;
    LoopMode         mode = LoopMode::Loop;

    containers::static_vector<FrameEvent, 8> events;

    [[nodiscard]] float duration() const noexcept {
        return frame_count > 0 ? static_cast<float>(frame_count) / fps : 0.0f;
    }
};

// Playback state component
struct SpriteAnimator {
    const FlipbookClip* current_clip = nullptr;
    float               time = 0.0f;
    float               speed = 1.0f;
    bool                playing = true;
    bool                finished = false;
    std::uint32_t       current_frame = 0;

    void play(const FlipbookClip* clip) noexcept {
        current_clip = clip;
        time = 0.0f;
        playing = true;
        finished = false;
        current_frame = clip ? clip->start_frame : 0;
    }

    void update(float dt) noexcept {
        if (!playing || !current_clip || current_clip->frame_count == 0) return;

        time += dt * speed;
        const float dur = current_clip->duration();
        if (dur <= 0.0f) return;

        switch (current_clip->mode) {
            case LoopMode::Once: {
                if (time >= dur) {
                    time = dur;
                    finished = true;
                    playing = false;
                    current_frame = current_clip->start_frame + current_clip->frame_count - 1;
                } else {
                    auto idx = static_cast<std::uint32_t>(time * current_clip->fps);
                    current_frame = current_clip->start_frame + std::min(idx, current_clip->frame_count - 1);
                }
                break;
            }
            case LoopMode::Loop: {
                float wrapped = time - dur * std::floor(time / dur);
                auto idx = static_cast<std::uint32_t>(wrapped * current_clip->fps);
                current_frame = current_clip->start_frame + (idx % current_clip->frame_count);
                break;
            }
            case LoopMode::PingPong: {
                float cycle = dur * 2.0f;
                float wrapped = time - cycle * std::floor(time / cycle);
                if (wrapped > dur) wrapped = cycle - wrapped;
                auto idx = static_cast<std::uint32_t>(wrapped * current_clip->fps);
                current_frame = current_clip->start_frame + std::min(idx, current_clip->frame_count - 1);
                break;
            }
        }
    }
};

} // namespace pebble::spandana
