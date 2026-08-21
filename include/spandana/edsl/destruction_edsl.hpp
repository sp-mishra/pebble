#pragma once
// ============================================================================
// spandana/edsl/destruction_edsl.hpp — Declarative Shatter & Destruction Directives
// ============================================================================

#include "../destruction.hpp"
#include "../timeline.hpp"

namespace pebble::spandana::edsl {

class ShatterEntityAction : public IAnimationAction {
public:
    ShatterEntityAction(pebble::ecs::World& world, pebble::ecs::Entity target,
                        pebble::math::vec2 impact_point, std::size_t shard_count,
                        float impulse_mag, ResourceKey key)
        : world_(world), target_(target), impact_(impact_point),
          shard_count_(shard_count), impulse_mag_(impulse_mag), key_(key) {}

    void on_start() override {
        DestructionEngine::shatter_entity_in_world(world_, target_, impact_, shard_count_, impulse_mag_);
    }

    void update(float, float) override {}
    [[nodiscard]] float duration() const noexcept override { return 0.0f; } // Instantaneous
    [[nodiscard]] ResourceKey resource_key() const noexcept override { return key_; }

private:
    pebble::ecs::World& world_;
    pebble::ecs::Entity target_;
    pebble::math::vec2  impact_;
    std::size_t         shard_count_;
    float               impulse_mag_;
    ResourceKey         key_;
};

class ShatterEntityBuilder {
public:
    ShatterEntityBuilder(pebble::ecs::World& world, pebble::ecs::Entity target, ResourceKey key = kWorldResource)
        : world_(world), target_(target), key_(key) {}

    ShatterEntityBuilder& at(pebble::math::vec2 impact_point) {
        impact_ = impact_point;
        return *this;
    }

    ShatterEntityBuilder& shards(std::size_t count) {
        shard_count_ = count;
        return *this;
    }

    [[nodiscard]] ShatterEntityAction impulse(float mag = 250.0f) const {
        return ShatterEntityAction(world_, target_, impact_, shard_count_, mag, key_);
    }

private:
    pebble::ecs::World& world_;
    pebble::ecs::Entity target_;
    pebble::math::vec2  impact_{0.0f, 0.0f};
    std::size_t         shard_count_ = 8;
    ResourceKey         key_;
};

inline ShatterEntityBuilder shatter_entity(pebble::ecs::World& world, pebble::ecs::Entity target, ResourceKey key = kWorldResource) {
    return ShatterEntityBuilder(world, target, key);
}

} // namespace pebble::spandana::edsl
