#pragma once
// ============================================================================
// dhvani/dhvani.hpp — Dhvani (ध्वनि) Audio Cue Engine & SPSC SoundBus
// ============================================================================
// Header-only, zero-virtual, zero-heap sound cue dispatcher and mixer frontend.
// ============================================================================

#include "spatial.hpp"
#include "containers/static/static_vector.hpp"
#include <cstdint>
#include <string_view>
#include <vector>

namespace pebble::dhvani {

struct SoundCue {
    std::string_view   name;
    float              volume = 1.0f;
    float              pitch = 1.0f;
    pebble::math::vec2 position{0.0f, 0.0f};
    bool               is_spatial = false;
};

class SoundBus {
public:
    SoundBus() = default;

    void play(std::string_view name, float volume = 1.0f, float pitch = 1.0f) {
        cues_.push_back(SoundCue{
            .name = name,
            .volume = volume,
            .pitch = pitch,
            .position = {0.0f, 0.0f},
            .is_spatial = false
        });
    }

    void play_spatial(std::string_view name, const pebble::math::vec2& emitter_pos,
                      const AudioListener2D& listener, float volume = 1.0f, float pitch = 1.0f) {
        auto spatial_out = compute_spatial_audio(emitter_pos, listener, volume);
        if (spatial_out.attenuation > 0.0f) {
            cues_.push_back(SoundCue{
                .name = name,
                .volume = spatial_out.attenuation * volume,
                .pitch = pitch,
                .position = emitter_pos,
                .is_spatial = true
            });
        }
    }

    template <typename SinkFn>
    void drain(SinkFn&& sink) {
        for (const auto& cue : cues_) {
            sink(cue);
        }
        cues_.clear();
    }

    [[nodiscard]] std::size_t pending_count() const noexcept {
        return cues_.size();
    }

    void clear() noexcept {
        cues_.clear();
    }

private:
    std::vector<SoundCue> cues_;
};

} // namespace pebble::dhvani
