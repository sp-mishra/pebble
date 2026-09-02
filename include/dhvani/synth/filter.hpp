#pragma once
// dhvani/synth/filter.hpp — RBJ cookbook biquad filters. Tag dispatch selects type at compile time.

#include "buffer.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>
#include <type_traits>

namespace pebble::dhvani::synth {
    struct FilterTag_LowPass {};

    struct FilterTag_HighPass {};

    struct FilterTag_BandPass {};

    struct FilterTag_Notch {};

    struct BiquadCoeffs {
        float b0, b1, b2, a1, a2;
    };

    struct BiquadState {
        float x1 = 0.f, x2 = 0.f, y1 = 0.f, y2 = 0.f;
    };

    template <typename Tag>
    [[nodiscard]] inline BiquadCoeffs make_biquad(float freq, float q, uint32_t sr) noexcept {
        const float w0 = 2.f * std::numbers::pi_v<float> * freq / static_cast<float>(sr);
        const float cos_w0 = std::cos(w0);
        const float sin_w0 = std::sin(w0);
        const float alpha = sin_w0 / (2.f * std::max(q, 1e-4f));

        float b0, b1, b2, a0, a1, a2;

        if constexpr (std::is_same_v<Tag, FilterTag_LowPass>) {
            b0 = (1.f - cos_w0) * 0.5f;
            b1 = 1.f - cos_w0;
            b2 = (1.f - cos_w0) * 0.5f;
            a0 = 1.f + alpha;
            a1 = -2.f * cos_w0;
            a2 = 1.f - alpha;
        }
        else if constexpr (std::is_same_v<Tag, FilterTag_HighPass>) {
            b0 = (1.f + cos_w0) * 0.5f;
            b1 = -(1.f + cos_w0);
            b2 = (1.f + cos_w0) * 0.5f;
            a0 = 1.f + alpha;
            a1 = -2.f * cos_w0;
            a2 = 1.f - alpha;
        }
        else if constexpr (std::is_same_v<Tag, FilterTag_BandPass>) {
            b0 = sin_w0 * 0.5f;
            b1 = 0.f;
            b2 = -sin_w0 * 0.5f;
            a0 = 1.f + alpha;
            a1 = -2.f * cos_w0;
            a2 = 1.f - alpha;
        }
        else { // Notch
            b0 = 1.f;
            b1 = -2.f * cos_w0;
            b2 = 1.f;
            a0 = 1.f + alpha;
            a1 = -2.f * cos_w0;
            a2 = 1.f - alpha;
        }

        const float inv_a0 = 1.f / a0;
        return {b0 * inv_a0, b1 * inv_a0, b2 * inv_a0, a1 * inv_a0, a2 * inv_a0};
    }

    [[nodiscard]] inline Sample process(BiquadState& s, const BiquadCoeffs& c, Sample x) noexcept {
        const float y = c.b0 * x + c.b1 * s.x1 + c.b2 * s.x2 - c.a1 * s.y1 - c.a2 * s.y2;
        s.x2 = s.x1;
        s.x1 = x;
        s.y2 = s.y1;
        s.y1 = y;
        return y;
    }
} // namespace pebble::dhvani::synth
