#pragma once
// dhvani/synth/waveform.hpp — Stateful oscillators: sine, saw, square, triangle, white noise.

#include "buffer.hpp"
#include <cmath>
#include <numbers>

namespace pebble::dhvani::synth {
    struct OscillatorState {
        float phase = 0.f;
        float frequency = 440.f;
        float amplitude = 1.f;
        uint32_t sample_rate = kDefaultSampleRate;
    };

    enum class WaveShape : uint8_t { Sine, Saw, Square, Triangle, WhiteNoise };

    [[nodiscard]] inline Sample tick(OscillatorState& s, WaveShape shape) noexcept {
        const float p = s.phase;
        const float dp = s.frequency / static_cast<float>(s.sample_rate);

        Sample out;
        switch (shape) {
        case WaveShape::Sine:
            out = std::sin(p * 2.f * std::numbers::pi_v<float>);
            break;
        case WaveShape::Saw:
            out = 2.f * p - 1.f;
            break;
        case WaveShape::Square:
            out = (p < 0.5f) ? 1.f : -1.f;
            break;
        case WaveShape::Triangle:
            out = (p < 0.5f) ? (4.f * p - 1.f) : (3.f - 4.f * p);
            break;
        case WaveShape::WhiteNoise: {
            // xorshift32 — deterministic, no stdlib random dependency
            static uint32_t seed = 123456789u;
            seed ^= seed << 13u;
            seed ^= seed >> 17u;
            seed ^= seed << 5u;
            out = (static_cast<float>(seed & 0xFFFFu) / 32767.5f) - 1.f;
            break;
        }
        default:
            out = 0.f;
        }
        s.phase = std::fmod(p + dp, 1.f);
        return out * s.amplitude;
    }

    template <std::size_t N>
    void fill_block(OscillatorState& s, WaveShape shape, SampleBlock<N>& block) noexcept {
        for (auto& frame : block) {
            const Sample v = tick(s, shape);
            frame.left = frame.right = v;
        }
    }
} // namespace pebble::dhvani::synth
