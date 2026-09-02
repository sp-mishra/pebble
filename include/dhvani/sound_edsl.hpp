#pragma once
// dhvani/sound_edsl.hpp — Fluent EDSL for physically-based sound synthesis.
// Entry points: impact(), fracture(), friction(), tear(), metal_hit().
// Terminal operations: .build() → SoundEvent, .render<N>() → SampleBlock<N>.

#include "physical/material.hpp"
#include "physical/impact.hpp"
#include "physical/fracture.hpp"
#include "physical/surface.hpp"
#include "physical/metal.hpp"
#include "synth/envelope.hpp"

namespace pebble::dhvani {
    enum class SoundEventType : uint8_t { Impact, Fracture, Friction, Tear, MetalHit };

    struct SoundEvent {
        SoundEventType type = SoundEventType::Impact;
        physical::MaterialParams material{};
        float force = 1.f;
        float velocity = 0.f;
        physical::MetalType metal_type = physical::MetalType::Bell;
        float fundamental = 440.f;
        uint32_t sample_rate = synth::kDefaultSampleRate;
    };

    class SoundBuilder {
    public:
        explicit SoundBuilder(SoundEventType type) noexcept : event_{.type = type} {}

        SoundBuilder& material(const physical::MaterialParams& m) noexcept {
            event_.material = m;
            return *this;
        }

        template <physical::PhysicalMaterial M>
        SoundBuilder& material(const M& m) noexcept {
            event_.material = m.material_params();
            return *this;
        }

        SoundBuilder& force(float f) noexcept {
            event_.force = f;
            return *this;
        }

        SoundBuilder& velocity(float v) noexcept {
            event_.velocity = v;
            return *this;
        }

        SoundBuilder& metal(physical::MetalType t) noexcept {
            event_.metal_type = t;
            return *this;
        }

        SoundBuilder& fundamental(float hz) noexcept {
            event_.fundamental = hz;
            return *this;
        }

        SoundBuilder& sample_rate(uint32_t sr) noexcept {
            event_.sample_rate = sr;
            return *this;
        }

        [[nodiscard]] SoundEvent build() const noexcept { return event_; }

        // Render N frames of PCM directly — zero heap, stack-allocated SampleBlock
        template <std::size_t N = synth::kDefaultBlockSize>
        [[nodiscard]] synth::SampleBlock<N> render() const noexcept {
            synth::SampleBlock < N > block{};
            render_into(std::span{block});
            return block;
        }

    private:
        SoundEvent event_;

        void render_into(std::span<synth::SampleFrame> out) const noexcept {
            const uint32_t sr = event_.sample_rate;
            switch (event_.type) {
            case SoundEventType::Impact: {
                physical::ImpactVoice < 8 > v{};
                v.trigger(event_.material, {event_.force, 0.002f}, sr);
                for (auto& f : out) {
                    const float s = v.tick();
                    f = {s, s};
                }
                break;
            }
            case SoundEventType::Fracture: {
                physical::FractureVoice v{};
                v.trigger(event_.material, {event_.force, 3}, sr);
                for (auto& f : out) {
                    const float s = v.tick();
                    f = {s, s};
                }
                break;
            }
            case SoundEventType::Friction: {
                physical::FrictionVoice v{};
                v.configure(event_.material, {event_.velocity, event_.force}, sr);
                for (auto& f : out) {
                    const float s = v.tick();
                    f = {s, s};
                }
                break;
            }
            case SoundEventType::Tear: {
                physical::TearVoice v{};
                v.configure(event_.material, event_.velocity, sr);
                for (auto& f : out) {
                    const float s = v.tick();
                    f = {s, s};
                }
                break;
            }
            case SoundEventType::MetalHit: {
                auto res = physical::make_metal_resonator < 8 > (
                    event_.metal_type, event_.fundamental, event_.force, sr);
                synth::EnvelopeState env{
                    .params = {.attack = 0.001f, .decay = 0.5f, .sustain = 0.f, .release = 2.0f},
                    .sample_rate = sr
                };
                synth::trigger(env);
                for (auto& f : out) {
                    const float e = synth::tick(env);
                    const float s = res.tick(sr) * e;
                    f = {s, s};
                }
                break;
            }
            }
        }
    };

    // EDSL free-function entry points
    [[nodiscard]] inline SoundBuilder impact() noexcept {
        return SoundBuilder{SoundEventType::Impact};
    }

    [[nodiscard]] inline SoundBuilder fracture() noexcept {
        return SoundBuilder{SoundEventType::Fracture};
    }

    [[nodiscard]] inline SoundBuilder friction() noexcept {
        return SoundBuilder{SoundEventType::Friction};
    }

    [[nodiscard]] inline SoundBuilder tear() noexcept {
        return SoundBuilder{SoundEventType::Tear};
    }

    [[nodiscard]] inline SoundBuilder metal_hit(
        physical::MetalType t = physical::MetalType::Bell) noexcept {
        return SoundBuilder{SoundEventType::MetalHit}.metal(t);
    }
} // namespace pebble::dhvani
