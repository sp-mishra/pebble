#pragma once
// dhvani/graph/builtin_nodes.hpp — Stock AudioNode implementations: oscillator, gain, filter, mixer.

#include "node.hpp"
#include "../synth/waveform.hpp"
#include "../synth/filter.hpp"
#include "../synth/envelope.hpp"
#include <span>
#include <algorithm>

namespace pebble::dhvani::graph {
    // Generates a waveform into output (ignores input)
    struct OscillatorNode {
        synth::OscillatorState state{};
        synth::WaveShape shape = synth::WaveShape::Sine;

        void process(std::span<synth::SampleFrame> out,
                     std::span<const synth::SampleFrame>,
                     uint32_t sr) noexcept {
            state.sample_rate = sr;
            for (auto& f : out) {
                const float s = synth::tick(state, shape);
                f.left += s;
                f.right += s;
            }
        }

        void reset() noexcept { state.phase = 0.f; }
    };

    static_assert(AudioNode<OscillatorNode>);

    // Multiplies all samples by a fixed gain
    struct GainNode {
        float gain = 1.f;

        void process(std::span<synth::SampleFrame> out,
                     std::span<const synth::SampleFrame>,
                     uint32_t) noexcept {
            for (auto& f : out) {
                f.left *= gain;
                f.right *= gain;
            }
        }

        void reset() noexcept {}
    };

    static_assert(AudioNode<GainNode>);

    // Biquad low-pass filter applied in-place
    struct LowPassNode {
        synth::BiquadCoeffs coeffs{};
        synth::BiquadState state_l{}, state_r{};

        LowPassNode(float cutoff_hz, float q, uint32_t sr) {
            coeffs = synth::make_biquad<synth::FilterTag_LowPass>(cutoff_hz, q, sr);
        }

        void process(std::span<synth::SampleFrame> out,
                     std::span<const synth::SampleFrame>,
                     uint32_t) noexcept {
            for (auto& f : out) {
                f.left = synth::process(state_l, coeffs, f.left);
                f.right = synth::process(state_r, coeffs, f.right);
            }
        }

        void reset() noexcept {
            state_l = {};
            state_r = {};
        }
    };

    static_assert(AudioNode<LowPassNode>);

    // Mixes input into output (add)
    struct MixerNode {
        float input_gain = 1.f;

        void process(std::span<synth::SampleFrame> out,
                     std::span<const synth::SampleFrame> in,
                     uint32_t) noexcept {
            const std::size_t n = std::min(out.size(), in.size());
            for (std::size_t i = 0; i < n; ++i) {
                out[i].left += in[i].left * input_gain;
                out[i].right += in[i].right * input_gain;
            }
        }

        void reset() noexcept {}
    };

    static_assert(AudioNode<MixerNode>);

    // Applies ADSR envelope to signal in-place
    struct EnvelopeNode {
        synth::EnvelopeState state{};

        void trigger() noexcept { synth::trigger(state); }

        void process(std::span<synth::SampleFrame> out,
                     std::span<const synth::SampleFrame>,
                     uint32_t sr) noexcept {
            state.sample_rate = sr;
            for (auto& f : out) {
                const float env = synth::tick(state);
                f.left *= env;
                f.right *= env;
            }
        }

        void reset() noexcept { state = {}; }
    };

    static_assert(AudioNode<EnvelopeNode>);
} // namespace pebble::dhvani::graph
