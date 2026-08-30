#pragma once
// dhvani/backend/null_backend.hpp — PCM-capture backend for tests and offline export.
// Satisfies AudioBackend concept; stores rendered frames in a heap vector.

#include "backend.hpp"
#include <vector>

namespace pebble::dhvani::backend {

class NullBackend {
public:
    explicit NullBackend(uint32_t sr = synth::kDefaultSampleRate,
                         std::size_t capture_frames = synth::kDefaultSampleRate)
        : sample_rate_(sr), capture_(capture_frames, synth::SampleFrame{}) {}

    [[nodiscard]] uint32_t sample_rate() const noexcept { return sample_rate_; }
    [[nodiscard]] bool     is_running()  const noexcept { return running_; }

    bool start(std::function<void(std::span<synth::SampleFrame>)> cb) {
        cb_ = std::move(cb);
        cb_(std::span{capture_});
        running_ = true;
        return true;
    }

    void stop() noexcept { running_ = false; }

    [[nodiscard]] std::span<const synth::SampleFrame> captured() const noexcept {
        return capture_;
    }

private:
    uint32_t                                           sample_rate_;
    std::vector<synth::SampleFrame>                    capture_;
    std::function<void(std::span<synth::SampleFrame>)> cb_;
    bool                                               running_ = false;
};

static_assert(AudioBackend<NullBackend>);

} // namespace pebble::dhvani::backend
