#pragma once
// gati/contact_cache.hpp — Persistent Manifold Cache with Warm-Started Impulse Reuse and Position Delta Checks.
#include "contact_constraint.hpp"
#include "akruti/narrowphase.hpp"
#include "akruti/math.hpp"
#include <containers/numeric/math_vector.hpp>
#include <cstdint>
#include <unordered_map>
#include <optional>

namespace gati {

struct ContactKey {
    std::uint32_t a{0};
    std::uint32_t b{0};

    constexpr bool operator==(const ContactKey& o) const noexcept {
        return a == o.a && b == o.b;
    }
};

struct ContactKeyHash {
    constexpr std::size_t operator()(const ContactKey& k) const noexcept {
        return (static_cast<std::size_t>(k.a) << 32) | static_cast<std::size_t>(k.b);
    }
};

struct CachedManifold {
    akruti::Manifold manifold{};
    pebble::math::vec2 pos_a{0.0f, 0.0f};
    pebble::math::vec2 pos_b{0.0f, 0.0f};
    float angle_a{0.0f};
    float angle_b{0.0f};
    float normal_impulse{0.0f};
    float tangent_impulse{0.0f};
    akruti::Vec separating_axis{1, 0};
    std::uint32_t age{0};
};

class ContactCache {
public:
    explicit ContactCache(std::size_t capacity = 1024) {
        cache_.reserve(capacity);
    }

    void clear() noexcept {
        cache_.clear();
    }

    [[nodiscard]] static ContactKey make_key(std::uint32_t a, std::uint32_t b) noexcept {
        return a < b ? ContactKey{a, b} : ContactKey{b, a};
    }

    // Check if resting bodies can skip narrowphase (delta pos < eps)
    [[nodiscard]] std::optional<CachedManifold> find(std::uint32_t a, std::uint32_t b) const noexcept {
        const auto key = make_key(a, b);
        auto it = cache_.find(key);
        if (it != cache_.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    void store(std::uint32_t a, std::uint32_t b, const akruti::Manifold& m,
               pebble::math::vec2 pos_a, pebble::math::vec2 pos_b,
               float angle_a, float angle_b,
               float normal_impulse = 0.0f, float tangent_impulse = 0.0f) {
        const auto key = make_key(a, b);
        CachedManifold cm;
        cm.manifold = m;
        cm.pos_a = pos_a;
        cm.pos_b = pos_b;
        cm.angle_a = angle_a;
        cm.angle_b = angle_b;
        cm.normal_impulse = normal_impulse;
        cm.tangent_impulse = tangent_impulse;
        cm.separating_axis = m.normal;
        cm.age = 0;
        cache_[key] = cm;
    }

    void update_impulses(std::uint32_t a, std::uint32_t b, float normal_imp, float tangent_imp) noexcept {
        const auto key = make_key(a, b);
        auto it = cache_.find(key);
        if (it != cache_.end()) {
            it->second.normal_impulse = normal_imp;
            it->second.tangent_impulse = tangent_imp;
        }
    }

    // Age out old contacts
    void tick() noexcept {
        for (auto it = cache_.begin(); it != cache_.end();) {
            if (++(it->second.age) > 2) {
                it = cache_.erase(it);
            } else {
                ++it;
            }
        }
    }

private:
    std::unordered_map<ContactKey, CachedManifold, ContactKeyHash> cache_;
};

} // namespace gati
