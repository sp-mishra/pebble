#pragma once
// dhvani/physical/surface.hpp — Continuous surface interaction sounds: friction and tearing.

#include "material.hpp"
#include "../synth/waveform.hpp"
#include "../synth/filter.hpp"
#include "../synth/envelope.hpp"
#include <algorithm>

namespace pebble::dhvani::physical {

struct FrictionParams {
    float velocity     = 1.0f;  // normalized relative surface speed [0..1]
    float normal_force = 0.5f;  // normalized [0..1]
};

// Band-pass filtered noise; amplitude and center frequency follow velocity
struct FrictionVoice {
    synth::OscillatorState noise{};
    synth::BiquadCoeffs    bp_coeff{};
    synth::BiquadState     bp_state{};

    void configure(const MaterialParams& mat, const FrictionParams& fp, uint32_t sr) noexcept {
        noise.sample_rate = sr;
        noise.amplitude   = fp.velocity * fp.normal_force;
        const float center = 200.f + mat.roughness * fp.velocity * 3000.f;
        const float q      = 1.f + mat.roughness * 4.f;
        bp_coeff = synth::make_biquad<synth::FilterTag_BandPass>(center, q, sr);
        bp_state = {};
    }

    [[nodiscard]] synth::Sample tick() noexcept {
        const float raw = synth::tick(noise, synth::WaveShape::WhiteNoise);
        return synth::process(bp_state, bp_coeff, raw);
    }

    // Modulate velocity at runtime without full reconfigure
    void set_velocity(float vel, const MaterialParams& mat, uint32_t sr) noexcept {
        noise.amplitude  = vel;
        const float center = 200.f + mat.roughness * vel * 3000.f;
        bp_coeff = synth::make_biquad<synth::FilterTag_BandPass>(
            center, 1.f + mat.roughness * 4.f, sr);
        bp_state = {};
    }
};

// Periodic bursts of HP-filtered noise, rate controlled by tear speed
struct TearVoice {
    synth::OscillatorState noise{};
    synth::BiquadCoeffs    hp_coeff{};
    synth::BiquadState     hp_state{};
    synth::EnvelopeState   env{};
    uint32_t               burst_counter    = 0;
    uint32_t               burst_period_smp = 2000;

    void configure(const MaterialParams& mat, float speed, uint32_t sr) noexcept {
        noise.sample_rate = sr;
        noise.amplitude   = std::clamp(speed, 0.f, 1.f);
        const float cutoff = 500.f + (1.f - mat.roughness) * 4000.f;
        hp_coeff = synth::make_biquad<synth::FilterTag_HighPass>(cutoff, 0.6f, sr);
        hp_state = {};
        // Higher speed → faster bursts; minimum 50 samples apart
        burst_period_smp = std::max(50u,
            static_cast<uint32_t>(static_cast<float>(sr) * 0.02f / (speed + 0.01f)));
        env.sample_rate  = sr;
        env.params = {.attack=0.001f, .decay=0.005f, .sustain=0.f, .release=0.01f};
        burst_counter = 0;
    }

    [[nodiscard]] synth::Sample tick() noexcept {
        if (++burst_counter >= burst_period_smp) {
            burst_counter = 0;
            synth::trigger(env);
        }
        const float e   = synth::tick(env);
        const float raw = synth::tick(noise, synth::WaveShape::WhiteNoise);
        return synth::process(hp_state, hp_coeff, raw) * e;
    }
};

} // namespace pebble::dhvani::physical
