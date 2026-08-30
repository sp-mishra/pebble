#pragma once
// ============================================================================
// dhvani/edsl.hpp — Dhvani Sound Cue Directives for Spandana Timelines
// ============================================================================

#include "dhvani.hpp"
#include "spandana/timeline.hpp"
#include <string_view>

namespace pebble::dhvani::edsl {

class AudioCueAction {
public:
    AudioCueAction(SoundBus& bus, std::string_view name, float volume, float pitch, spandana::ResourceKey key)
        : bus_(bus), name_(name), volume_(volume), pitch_(pitch), key_(key) {}

    void on_start() {
        bus_.play(name_, volume_, pitch_);
    }

    void update(float, float) noexcept {}
    [[nodiscard]] float duration() const noexcept { return 0.0f; } // Instantaneous cue trigger
    [[nodiscard]] spandana::ResourceKey resource_key() const noexcept { return key_; }

private:
    SoundBus&                bus_;
    std::string_view         name_;
    float                    volume_;
    float                    pitch_;
    spandana::ResourceKey    key_;
};

class AudioCueBuilder {
public:
    AudioCueBuilder(SoundBus& bus, std::string_view name, spandana::ResourceKey key = spandana::kWorldResource)
        : bus_(bus), name_(name), key_(key) {}

    AudioCueBuilder& volume(float vol) {
        volume_ = vol;
        return *this;
    }

    AudioCueBuilder& pitch(float p) {
        pitch_ = p;
        return *this;
    }

    [[nodiscard]] AudioCueAction build() const {
        return AudioCueAction(bus_, name_, volume_, pitch_, key_);
    }

    // Implicit conversion to Action for direct use in timeline.add()
    operator AudioCueAction() const {
        return build();
    }

private:
    SoundBus&             bus_;
    std::string_view      name_;
    float                 volume_ = 1.0f;
    float                 pitch_ = 1.0f;
    spandana::ResourceKey key_;
};

inline AudioCueBuilder audio_cue(SoundBus& bus, std::string_view name, spandana::ResourceKey key = spandana::kWorldResource) {
    return AudioCueBuilder(bus, name, key);
}

} // namespace pebble::dhvani::edsl
