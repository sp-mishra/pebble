#pragma once
// dhvani/backend/miniaudio.hpp — Hardware playback backend via miniaudio.
// Gated on DHVANI_USE_MINIAUDIO. Falls back to a no-op stub when not defined.

#include "backend.hpp"

#ifdef DHVANI_USE_MINIAUDIO

// miniaudio single-file impl — include exactly once in a .cpp translation unit
// by defining MINIAUDIO_IMPLEMENTATION before including this header.
// Here we include the header-only declarations only.
#ifndef MINIAUDIO_IMPLEMENTATION
#define MA_NO_DECODING
#define MA_NO_ENCODING
#endif

#if __has_include(<miniaudio.h>)
#include <miniaudio.h>
#elif __has_include("miniaudio.h")
#include "miniaudio.h"
#endif

#endif // DHVANI_USE_MINIAUDIO

namespace pebble::dhvani::backend {
#ifdef DHVANI_USE_MINIAUDIO

    class MiniAudioBackend {
    public:
        explicit MiniAudioBackend(uint32_t sr = synth::kDefaultSampleRate) noexcept
            : sample_rate_(sr) {}

        ~MiniAudioBackend() { stop(); }

        [[nodiscard]] uint32_t sample_rate() const noexcept { return sample_rate_; }
        [[nodiscard]] bool is_running() const noexcept { return running_; }

        bool start(std::function<void(std::span<synth::SampleFrame>)> callback) {
            callback_ = std::move(callback);

            ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
            cfg.playback.format = ma_format_f32;
            cfg.playback.channels = 2;
            cfg.sampleRate = sample_rate_;
            cfg.dataCallback = &MiniAudioBackend::data_callback;
            cfg.pUserData = this;

            if (ma_device_init(nullptr, &cfg, &device_) != MA_SUCCESS)
                return false;
            if (ma_device_start(&device_) != MA_SUCCESS) {
                ma_device_uninit(&device_);
                return false;
            }
            running_ = true;
            return true;
        }

        void stop() noexcept {
            if (running_) {
                ma_device_stop(&device_);
                ma_device_uninit(&device_);
                running_ = false;
            }
        }

    private:
        ma_device device_{};
        std::function<void(std::span<synth::SampleFrame>)> callback_;
        uint32_t sample_rate_;
        bool running_ = false;

        static void data_callback(ma_device* dev, void* out, const void*, ma_uint32 frame_count) noexcept {
            auto* self = static_cast<MiniAudioBackend*>(dev->pUserData);
            auto frames = std::span{static_cast<synth::SampleFrame*>(out), frame_count};
            if (self->callback_) self->callback_(frames);
        }
    };

    static_assert(AudioBackend<MiniAudioBackend>);

#else // !DHVANI_USE_MINIAUDIO

    // No-op stub — satisfies concept without any hardware interaction
    struct MiniAudioBackend {
        [[nodiscard]] uint32_t sample_rate() const noexcept { return synth::kDefaultSampleRate; }
        [[nodiscard]] bool is_running() const noexcept { return false; }
        bool start(std::function<void(std::span<synth::SampleFrame>)>) noexcept { return false; }
        void stop() noexcept {}
    };

    static_assert(AudioBackend<MiniAudioBackend>);

#endif // DHVANI_USE_MINIAUDIO
} // namespace pebble::dhvani::backend
