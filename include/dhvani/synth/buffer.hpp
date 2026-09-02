#pragma once
// dhvani/synth/buffer.hpp — PCM sample types and fixed-capacity block primitives.

#include <array>
#include <cstdint>

namespace pebble::dhvani::synth {
    using Sample = float;
    static constexpr uint32_t kDefaultSampleRate = 44100;
    static constexpr uint32_t kDefaultBlockSize = 512;

    struct SampleFrame {
        Sample left = 0.f;
        Sample right = 0.f;
    };

    // Zero-heap fixed-capacity PCM block
    template <std::size_t N = kDefaultBlockSize>
    using SampleBlock = std::array<SampleFrame, N>;
} // namespace pebble::dhvani::synth
