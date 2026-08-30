#pragma once
// dhvani/synth/envelope.hpp — ADSR amplitude envelope state machine.

#include "buffer.hpp"
#include <algorithm>

namespace pebble::dhvani::synth {

enum class EnvelopeStage : uint8_t { Idle, Attack, Decay, Sustain, Release, Done };

struct ADSRParams {
    float attack  = 0.01f;  // seconds
    float decay   = 0.10f;  // seconds
    float sustain = 0.70f;  // level [0..1]
    float release = 0.30f;  // seconds
};

struct EnvelopeState {
    ADSRParams    params{};
    EnvelopeStage stage       = EnvelopeStage::Idle;
    float         value       = 0.f;
    float         time        = 0.f;
    uint32_t      sample_rate = kDefaultSampleRate;
};

inline void trigger(EnvelopeState& e) noexcept {
    e.stage = EnvelopeStage::Attack;
    e.time  = 0.f;
}

inline void release_note(EnvelopeState& e) noexcept {
    if (e.stage != EnvelopeStage::Done) {
        e.stage = EnvelopeStage::Release;
        e.time  = 0.f;
    }
}

[[nodiscard]] inline bool done(const EnvelopeState& e) noexcept {
    return e.stage == EnvelopeStage::Done;
}

[[nodiscard]] inline Sample tick(EnvelopeState& e) noexcept {
    const float dt = 1.f / static_cast<float>(e.sample_rate);
    switch (e.stage) {
        case EnvelopeStage::Attack:
            e.value += dt / std::max(e.params.attack, 1e-6f);
            if (e.value >= 1.f) {
                e.value = 1.f;
                e.stage = EnvelopeStage::Decay;
                e.time  = 0.f;
            }
            break;
        case EnvelopeStage::Decay:
            e.value -= dt * (1.f - e.params.sustain) / std::max(e.params.decay, 1e-6f);
            if (e.value <= e.params.sustain) {
                e.value = e.params.sustain;
                e.stage = (e.params.sustain <= 0.f) ? EnvelopeStage::Done : EnvelopeStage::Sustain;
            }
            break;
        case EnvelopeStage::Sustain:
            break;
        case EnvelopeStage::Release:
            e.value -= dt * e.params.sustain / std::max(e.params.release, 1e-6f);
            if (e.value <= 0.f) {
                e.value = 0.f;
                e.stage = EnvelopeStage::Done;
            }
            break;
        default:
            e.value = 0.f;
            break;
    }
    return e.value;
}

} // namespace pebble::dhvani::synth
