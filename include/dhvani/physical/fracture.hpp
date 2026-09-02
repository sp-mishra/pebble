#pragma once
// dhvani/physical/fracture.hpp — Crack/break/shatter burst model.
// Brittleness → HP cutoff frequency; burst decays to silence.

#include "material.hpp"
#include "../synth/waveform.hpp"
#include "../synth/filter.hpp"
#include "../synth/envelope.hpp"

namespace pebble::dhvani::physical {
    struct FractureParams {
        float force = 1.0f;
        uint32_t num_shards = 3; // cosmetic: reserved for multi-shard extensions
    };

    struct FractureVoice {
        synth::OscillatorState noise{};
        synth::BiquadCoeffs hp_coeff{};
        synth::BiquadState hp_state{};
        synth::EnvelopeState env{};
        bool active = false;

        void trigger(const MaterialParams& mat, const FractureParams& fp, uint32_t sr) noexcept {
            noise = {.phase = 0.f, .frequency = 1.f, .amplitude = fp.force, .sample_rate = sr};

            // brittleness maps [0..1] → cutoff [100..8100] Hz
            const float cutoff = 100.f + mat.brittleness * 8000.f;
            hp_coeff = synth::make_biquad<synth::FilterTag_HighPass>(cutoff, 0.7f, sr);
            hp_state = {};

            // Fracture is an impulsive crack: energy is front-loaded and the burst
            // must collapse to silence within a few hundred samples (a shard snap,
            // not a sustained ring). Keep the decay short and damping-scaled so the
            // tail dies well inside the onset window.
            env.params = {
                .attack = 0.0005f,
                .decay = 0.005f + mat.damping * 0.05f,
                .sustain = 0.f,
                .release = mat.brittleness * 0.05f + 0.01f
            };
            env.sample_rate = sr;
            synth::trigger(env);
            active = true;
        }

        [[nodiscard]] synth::Sample tick() noexcept {
            if (!active) return 0.f;
            const float e = synth::tick(env);
            const float raw = synth::tick(noise, synth::WaveShape::WhiteNoise);
            const float sig = synth::process(hp_state, hp_coeff, raw);
            if (synth::done(env)) active = false;
            return sig * e;
        }

        [[nodiscard]] bool is_active() const noexcept { return active; }
    };
} // namespace pebble::dhvani::physical
