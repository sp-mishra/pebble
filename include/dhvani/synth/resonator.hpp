#pragma once
// dhvani/synth/resonator.hpp — Karplus-Strong string model and N-mode modal resonator.

#include "buffer.hpp"
#include <array>
#include <cmath>
#include <span>
#include <numbers>
#include <algorithm>

namespace pebble::dhvani::synth {
    // Karplus-Strong plucked-string resonator
    template <std::size_t MaxDelay = 4096>
    struct KarplusStrong {
        std::array<Sample, MaxDelay> buf{};
        std::size_t write_pos = 0;
        std::size_t period = 100;
        float damping = 0.996f;

        void excite(std::span<const Sample> noise) noexcept {
            const std::size_t n = std::min(noise.size(), period);
            for (std::size_t i = 0; i < n; ++i)
                buf[(write_pos + i) % period] = noise[i];
        }

        [[nodiscard]] Sample tick() noexcept {
            const std::size_t r1 = write_pos;
            const std::size_t r2 = (write_pos + 1) % period;
            const Sample out = (buf[r1] + buf[r2]) * 0.5f * damping;
            buf[write_pos] = out;
            write_pos = (write_pos + 1) % period;
            return out;
        }

        void set_frequency(float freq, uint32_t sr) noexcept {
            period = std::clamp<std::size_t>(
                static_cast<std::size_t>(static_cast<float>(sr) / std::max(freq, 1.f)),
                2, MaxDelay);
        }
    };

    // N parallel second-order modal resonators — used for metallic/bell/impact sounds
    template <std::size_t Modes = 8>
    struct ModalResonator {
        struct Mode {
            float freq = 440.f;
            float decay = 0.999f; // per-sample amplitude gain
            float amp = 1.f;
            float r = 0.f; // state y[n]
            float rp = 0.f; // state y[n-1]
        };

        std::array<Mode, Modes> modes{};

        void excite(float force) noexcept {
            for (auto& m : modes) {
                m.r = m.amp * force;
                m.rp = 0.f;
            }
        }

        [[nodiscard]] Sample tick(uint32_t sr) noexcept {
            Sample out = 0.f;
            const float inv_sr = 1.f / static_cast<float>(sr);
            for (auto& m : modes) {
                const float omega = 2.f * std::numbers::pi_v<float> * m.freq * inv_sr;
                const float cos_w = std::cos(omega);
                const float r_next = 2.f * m.decay * cos_w * m.r - m.decay * m.decay * m.rp;
                m.rp = m.r;
                m.r = r_next;
                out += r_next;
            }
            return out;
        }
    };
} // namespace pebble::dhvani::synth
