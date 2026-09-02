#pragma once
// dhvani/physical/metal.hpp — Modal synthesis for metallic sounds: bell, cymbal, plate, pipe, spring.
// Mode frequency ratios sourced from physical measurements of real metallic objects.

#include "../synth/resonator.hpp"
#include <algorithm>
#include <cmath>

namespace pebble::dhvani::physical {
    enum class MetalType : uint8_t { Bell, Cymbal, Plate, Pipe, Spring };

    template <std::size_t Modes = 8>
    [[nodiscard]] inline synth::ModalResonator<Modes> make_metal_resonator(
        MetalType type, float fundamental, float force, uint32_t sr) noexcept {
        // Inharmonic ratios measured from physical metallic object spectra
        static constexpr float bell_ratios[] = {1.f, 2.76f, 5.40f, 8.93f, 13.34f, 18.64f, 24.77f, 31.87f};
        static constexpr float cymbal_ratios[] = {1.f, 1.49f, 1.96f, 2.59f, 3.26f, 4.04f, 5.01f, 6.12f};
        static constexpr float plate_ratios[] = {1.f, 2.07f, 4.04f, 6.73f, 10.32f, 14.66f, 19.96f, 26.11f};
        static constexpr float pipe_ratios[] = {1.f, 2.00f, 3.00f, 4.00f, 5.00f, 6.00f, 7.00f, 8.00f};
        static constexpr float spring_ratios[] = {1.f, 1.30f, 1.78f, 2.40f, 3.10f, 4.00f, 5.10f, 6.40f};

        const float* ratios = [type]() -> const float* {
            switch (type) {
            case MetalType::Bell: return bell_ratios;
            case MetalType::Cymbal: return cymbal_ratios;
            case MetalType::Plate: return plate_ratios;
            case MetalType::Pipe: return pipe_ratios;
            case MetalType::Spring: return spring_ratios;
            }
            return bell_ratios;
        }();

        // Cymbal has longer sustain (thinner metal, less internal damping)
        const float base_decay = (type == MetalType::Cymbal) ? 0.99995f : 0.9998f;
        const float nyquist = static_cast<float>(sr) * 0.45f;

        synth::ModalResonator < Modes > r{};
        for (std::size_t i = 0; i < Modes; ++i) {
            r.modes[i].freq = std::min(fundamental * ratios[i], nyquist);
            // Higher modes decay faster — physically accurate
            r.modes[i].decay = std::pow(base_decay, ratios[i]);
            r.modes[i].amp = 1.f / (1.f + static_cast<float>(i) * 0.3f);
        }
        r.excite(force);
        return r;
    }
} // namespace pebble::dhvani::physical
