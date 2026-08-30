#pragma once
// ============================================================================
// spandana/edsl/particle_edsl.hpp — Procedural Particle Burst EDSL Directives
// ============================================================================
// The action does NOT own the particle buffer — it writes into caller-provided
// storage (a `static_vector<Particle, N>&`), mirroring how tweens target an
// external property by reference. This keeps the erased action small enough for
// the Timeline's inline buffer (no heap, no hidden multi-KB payload) and lets
// the caller choose any capacity `N`. The RNG seed is configurable (`.seed()`)
// rather than hardcoded, so bursts can be deterministic per-emitter or varied.
// ============================================================================

#include "../timeline.hpp"
#include "containers/numeric/math_vector.hpp"
#include "containers/static/static_vector.hpp"
#include <cmath>
#include <cstdint>
#include <random>

namespace pebble::spandana::edsl {

struct Particle {
    pebble::math::vec2 position{};
    pebble::math::vec2 velocity{};
    float              lifetime = 1.0f;
    float              age = 0.0f;
    float              size = 4.0f;
};

// Particle Burst Action — templated on the caller buffer's capacity so no
// fixed limit is baked in and no silent truncation occurs beyond the buffer the
// caller actually supplied.
template <std::size_t MaxParticles>
class ParticleBurstAction {
public:
    using buffer_type = containers::static_vector<Particle, MaxParticles>;

    ParticleBurstAction(buffer_type& out, pebble::math::vec2 origin, std::uint32_t count,
                        float min_speed, float max_speed, float lifetime,
                        std::uint32_t seed, ResourceKey key)
        : out_(&out), origin_(origin), count_(count), min_speed_(min_speed),
          max_speed_(max_speed), lifetime_(lifetime), seed_(seed), key_(key) {}

    void on_start() {
        out_->clear();
        std::mt19937 rng(seed_);
        std::uniform_real_distribution<float> angle_dist(0.0f, 6.2831853f);
        std::uniform_real_distribution<float> speed_dist(min_speed_, max_speed_);

        // Emit up to min(requested, buffer capacity). If the caller asked for
        // more than the buffer holds, out_->overflow() reports the shortfall —
        // no silent drop past a magic constant.
        for (std::uint32_t i = 0; i < count_; ++i) {
            const float angle = angle_dist(rng);
            const float speed = speed_dist(rng);
            const bool ok = out_->push_back(Particle{
                .position = origin_,
                .velocity = pebble::math::vec2(std::cos(angle) * speed, std::sin(angle) * speed),
                .lifetime = lifetime_,
                .age = 0.0f,
                .size = 4.0f
            });
            if (!ok) break;
        }
    }

    void update(float, float dt) noexcept {
        for (auto& p : *out_) {
            p.position = p.position + p.velocity * dt;
            p.age += dt;
        }
    }

    [[nodiscard]] float duration() const noexcept { return lifetime_; }
    [[nodiscard]] ResourceKey resource_key() const noexcept { return key_; }

private:
    buffer_type*       out_;
    pebble::math::vec2 origin_;
    std::uint32_t      count_;
    float              min_speed_;
    float              max_speed_;
    float              lifetime_;
    std::uint32_t      seed_;
    ResourceKey        key_;
};

template <std::size_t MaxParticles>
class ParticleBurstBuilder {
public:
    using buffer_type = containers::static_vector<Particle, MaxParticles>;

    ParticleBurstBuilder(buffer_type& out, ResourceKey key)
        : out_(&out), key_(key) {}

    ParticleBurstBuilder& at(pebble::math::vec2 pos) { origin_ = pos; return *this; }
    ParticleBurstBuilder& count(std::uint32_t n)     { count_ = n; return *this; }
    ParticleBurstBuilder& speed(float lo, float hi)  { min_speed_ = lo; max_speed_ = hi; return *this; }
    ParticleBurstBuilder& seed(std::uint32_t s)      { seed_ = s; return *this; }

    [[nodiscard]] ParticleBurstAction<MaxParticles> lifetime(float dur = 0.5f) {
        return ParticleBurstAction<MaxParticles>(*out_, origin_, count_, min_speed_, max_speed_, dur, seed_, key_);
    }

private:
    buffer_type*       out_;
    pebble::math::vec2 origin_{};
    std::uint32_t      count_ = 32;
    float              min_speed_ = 50.0f;
    float              max_speed_ = 150.0f;
    std::uint32_t      seed_ = 1337u;
    ResourceKey        key_;
};

// Emit a burst into caller-owned `buffer`. Capacity is deduced from the buffer,
// so the caller controls the particle budget with zero heap allocation.
template <std::size_t MaxParticles>
inline ParticleBurstBuilder<MaxParticles>
particle_burst(containers::static_vector<Particle, MaxParticles>& buffer,
               ResourceKey key = kWorldResource) {
    return ParticleBurstBuilder<MaxParticles>(buffer, key);
}

} // namespace pebble::spandana::edsl
