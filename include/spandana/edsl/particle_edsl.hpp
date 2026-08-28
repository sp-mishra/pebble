#pragma once
// ============================================================================
// spandana/edsl/particle_edsl.hpp — Procedural Particle Burst EDSL Directives
// ============================================================================

#include "../timeline.hpp"
#include "containers/numeric/math_vector.hpp"
#include "containers/static/static_vector.hpp"
#include <cmath>
#include <random>

namespace pebble::spandana::edsl {

struct Particle {
    pebble::math::vec2 position{};
    pebble::math::vec2 velocity{};
    float              lifetime = 1.0f;
    float              age = 0.0f;
    float              size = 4.0f;
};

// Particle Burst Action
class ParticleBurstAction : public IAnimationAction {
public:
    ParticleBurstAction(pebble::math::vec2 origin, std::uint32_t count,
                        float min_speed, float max_speed, float lifetime,
                        ResourceKey key)
        : origin_(origin), count_(count), min_speed_(min_speed),
          max_speed_(max_speed), lifetime_(lifetime), key_(key) {}

    void on_start() override {
        particles_.clear();
        std::mt19937 rng(1337);
        std::uniform_real_distribution<float> angle_dist(0.0f, 6.2831853f);
        std::uniform_real_distribution<float> speed_dist(min_speed_, max_speed_);

        for (std::uint32_t i = 0; i < count_ && i < 64; ++i) {
            const float angle = angle_dist(rng);
            const float speed = speed_dist(rng);
            (void)particles_.push_back(Particle{
                .position = origin_,
                .velocity = pebble::math::vec2(std::cos(angle) * speed, std::sin(angle) * speed),
                .lifetime = lifetime_,
                .age = 0.0f,
                .size = 4.0f
            });
        }
    }

    void update(float, float dt) override {
        for (auto& p : particles_) {
            p.position = p.position + p.velocity * dt;
            p.age += dt;
        }
    }

    [[nodiscard]] float duration() const noexcept override { return lifetime_; }
    [[nodiscard]] ResourceKey resource_key() const noexcept override { return key_; }

    [[nodiscard]] const containers::static_vector<Particle, 64>& particles() const noexcept {
        return particles_;
    }

private:
    pebble::math::vec2                    origin_;
    std::uint32_t                         count_;
    float                                 min_speed_;
    float                                 max_speed_;
    float                                 lifetime_;
    ResourceKey                           key_;
    containers::static_vector<Particle, 64> particles_;
};

class ParticleBurstBuilder {
public:
    explicit ParticleBurstBuilder(ResourceKey key = kWorldResource)
        : key_(key) {}

    ParticleBurstBuilder& at(pebble::math::vec2 pos) {
        origin_ = pos;
        return *this;
    }

    ParticleBurstBuilder& count(std::uint32_t n) {
        count_ = n;
        return *this;
    }

    ParticleBurstBuilder& speed(float min_spd, float max_spd) {
        min_speed_ = min_spd;
        max_speed_ = max_spd;
        return *this;
    }

    ParticleBurstAction lifetime(float dur = 0.5f) {
        return ParticleBurstAction(origin_, count_, min_speed_, max_speed_, dur, key_);
    }

private:
    pebble::math::vec2 origin_{};
    std::uint32_t      count_ = 32;
    float              min_speed_ = 50.0f;
    float              max_speed_ = 150.0f;
    ResourceKey        key_;
};

inline ParticleBurstBuilder particle_burst(ResourceKey key = kWorldResource) {
    return ParticleBurstBuilder(key);
}

} // namespace pebble::spandana::edsl
