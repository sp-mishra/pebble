#pragma once
// dhvani/physical/impact.hpp — Physically-based impact sound: force + material → modal synthesis.

#include "material.hpp"
#include "../synth/resonator.hpp"
#include "../synth/envelope.hpp"
#include <cmath>
#include <algorithm>

namespace pebble::dhvani::physical {
    struct ImpactParams {
        float force = 1.0f; // normalized [0..1]
        float contact_duration = 0.002f; // seconds
    };

    // Build a modal resonator whose mode frequencies and decays match the material
    template <std::size_t Modes = 8>
    [[nodiscard]] inline synth::ModalResonator<Modes> make_impact_resonator(
        const MaterialParams& mat, uint32_t sample_rate) noexcept {
        synth::ModalResonator < Modes > r{};

        // Fundamental: stiffness maps rubber(80Hz) → steel(2000Hz)
        const float f0 = 80.f + mat.stiffness * 1920.f;
        // Inharmonicity: low damping → strong metallic inharmonicity
        const float inharmonicity = 1.f + (1.f - mat.damping) * 1.5f;

        const float nyquist = static_cast<float>(sample_rate) * 0.45f;
        for (std::size_t i = 0; i < Modes; ++i) {
            const float ratio = std::pow(static_cast<float>(i + 1), inharmonicity);
            r.modes[i].freq = std::min(f0 * ratio, nyquist);
            // Per-sample decay derived from material damping + mode frequency
            // Higher freq modes decay faster in real materials
            const float decay_base = 1.f - mat.damping * 0.001f;
            r.modes[i].decay = std::clamp(
                std::pow(decay_base, r.modes[i].freq / f0), 0.9f, 0.99999f);
            r.modes[i].amp = 1.f / std::sqrt(static_cast<float>(i + 1));
        }
        return r;
    }

    template <std::size_t Modes = 8>
    struct ImpactVoice {
        synth::ModalResonator<Modes> resonator{};
        synth::EnvelopeState envelope{};
        bool active = false;

        void trigger(const MaterialParams& mat, const ImpactParams& imp, uint32_t sr) noexcept {
            resonator = make_impact_resonator<Modes>(mat, sr);
            resonator.excite(imp.force);

            envelope.params = {
                .attack = std::max(imp.contact_duration, 0.0001f),
                .decay = mat.damping * 0.5f + 0.05f,
                .sustain = 0.f,
                .release = (1.f - mat.damping) * 2.0f + 0.1f
            };
            envelope.sample_rate = sr;
            synth::trigger(envelope);
            active = true;
        }

        [[nodiscard]] synth::Sample tick() noexcept {
            if (!active) return 0.f;
            const float env = synth::tick(envelope);
            const float sig = resonator.tick(envelope.sample_rate);
            if (synth::done(envelope)) active = false;
            return sig * env;
        }

        [[nodiscard]] bool is_active() const noexcept { return active; }
    };
} // namespace pebble::dhvani::physical
