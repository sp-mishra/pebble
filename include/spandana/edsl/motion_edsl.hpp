#pragma once
// ============================================================================
// spandana/edsl/motion_edsl.hpp — Declarative Motion, Tween & Spring EDSL Directives
// ============================================================================
// Easing is a compile-time policy: each tween is templated on an EasingFunction
// functor stored via [[no_unique_address]], so the default (linear) costs zero
// bytes and every easing call inlines with no heap and no std::function.
// `.ease(fn)` rebinds the action to the new easing type, preserving state.
// ============================================================================

#include "../timeline.hpp"
#include "../easing.hpp"
#include "../spring.hpp"
#include "../concepts.hpp"
#include "containers/numeric/math_vector.hpp"
#include <cmath>
#include <utility>

namespace pebble::spandana::edsl {

// Tween Action targeting a float property.
template <EasingFunction E = ease::Linear>
class FloatTweenAction {
public:
    FloatTweenAction(float& prop, float target, float duration, ResourceKey key, E ease = {})
        : prop_(prop), start_(prop), target_(target), duration_(duration), key_(key), ease_(ease) {}

    // Rebind to a new easing policy, moving accumulated state across.
    template <EasingFunction E2>
    [[nodiscard]] FloatTweenAction<E2> ease(E2 e) const {
        return FloatTweenAction<E2>(prop_, target_, duration_, key_, e);
    }

    void on_start() noexcept { start_ = prop_; }

    void update(float progress, float) noexcept {
        prop_ = lerp(start_, target_, ease_(progress));
    }

    [[nodiscard]] float duration() const noexcept { return duration_; }
    [[nodiscard]] ResourceKey resource_key() const noexcept { return key_; }

private:
    float&                     prop_;
    float                      start_;
    float                      target_;
    float                      duration_;
    ResourceKey                key_;
    [[no_unique_address]] E    ease_{};
};

// Tween Action targeting a pebble::math::vec2 property.
template <EasingFunction E = ease::Linear>
class Vec2TweenAction {
public:
    Vec2TweenAction(pebble::math::vec2& prop, pebble::math::vec2 target, float duration, ResourceKey key, E ease = {})
        : prop_(prop), start_(prop), target_(target), duration_(duration), key_(key), ease_(ease) {}

    template <EasingFunction E2>
    [[nodiscard]] Vec2TweenAction<E2> ease(E2 e) const {
        return Vec2TweenAction<E2>(prop_, target_, duration_, key_, e);
    }

    void on_start() noexcept { start_ = prop_; }

    void update(float progress, float) noexcept {
        prop_ = pebble::math::lerp(start_, target_, ease_(progress));
    }

    [[nodiscard]] float duration() const noexcept { return duration_; }
    [[nodiscard]] ResourceKey resource_key() const noexcept { return key_; }

private:
    pebble::math::vec2&        prop_;
    pebble::math::vec2         start_;
    pebble::math::vec2         target_;
    float                      duration_;
    ResourceKey                key_;
    [[no_unique_address]] E    ease_{};
};

// Spring Action targeting a pebble::math::vec2 property.
class Vec2SpringAction {
public:
    Vec2SpringAction(pebble::math::vec2& prop, pebble::math::vec2 target, float stiffness, float damping, float duration, ResourceKey key)
        : prop_(prop), target_(target), spring_(stiffness, damping), duration_(duration), key_(key) {}

    void update(float, float dt) noexcept {
        auto [pos, vel] = spring_.step(prop_, vel_, target_, dt);
        prop_ = pos;
        vel_ = vel;
    }

    [[nodiscard]] float duration() const noexcept { return duration_; }
    [[nodiscard]] ResourceKey resource_key() const noexcept { return key_; }

private:
    pebble::math::vec2&        prop_;
    pebble::math::vec2         target_;
    pebble::math::vec2         vel_{};
    Vector2SpringDamper        spring_;
    float                      duration_;
    ResourceKey                key_;
};

// Callback Action — templated on the invocable so no std::function / heap.
template <typename Fn>
class CallbackAction {
public:
    explicit CallbackAction(Fn cb, ResourceKey key = kWorldResource)
        : cb_(std::move(cb)), key_(key) {}

    void on_start() { cb_(); }
    void update(float, float) noexcept {}
    [[nodiscard]] float duration() const noexcept { return 0.0f; }
    [[nodiscard]] ResourceKey resource_key() const noexcept { return key_; }

private:
    [[no_unique_address]] Fn cb_;
    ResourceKey              key_;
};

// Builders
struct FloatTweenBuilder {
    float& prop;
    ResourceKey key;

    [[nodiscard]] FloatTweenAction<> to(float target, float duration = 0.3f) const {
        return FloatTweenAction<>(prop, target, duration, key);
    }
};

struct Vec2TweenBuilder {
    pebble::math::vec2& prop;
    ResourceKey key;

    [[nodiscard]] Vec2TweenAction<> to(pebble::math::vec2 target, float duration = 0.3f) const {
        return Vec2TweenAction<>(prop, target, duration, key);
    }
};

struct Vec2SpringBuilder {
    pebble::math::vec2& prop;
    ResourceKey key;

    [[nodiscard]] Vec2SpringAction target(pebble::math::vec2 target, float stiffness = 180.0f, float damping = 12.0f, float duration = 0.5f) const {
        return Vec2SpringAction(prop, target, stiffness, damping, duration, key);
    }
};

inline FloatTweenBuilder tween(float& prop, ResourceKey key = kWorldResource) {
    return FloatTweenBuilder{prop, key};
}

inline Vec2TweenBuilder tween(pebble::math::vec2& prop, ResourceKey key = kWorldResource) {
    return Vec2TweenBuilder{prop, key};
}

inline Vec2SpringBuilder spring(pebble::math::vec2& prop, ResourceKey key = kWorldResource) {
    return Vec2SpringBuilder{prop, key};
}

// Follow Path Action for Akruti Splines.
template <typename SplineT, EasingFunction E = ease::Linear>
class SplinePathFollowAction {
public:
    SplinePathFollowAction(pebble::math::vec2& prop, SplineT spline, float duration, ResourceKey key, float* rot = nullptr, E ease = {})
        : prop_(prop), spline_(std::move(spline)), duration_(duration), key_(key), rot_ptr_(rot), ease_(ease) {}

    template <EasingFunction E2>
    [[nodiscard]] SplinePathFollowAction<SplineT, E2> ease(E2 e) const {
        return SplinePathFollowAction<SplineT, E2>(prop_, spline_, duration_, key_, rot_ptr_, e);
    }

    [[nodiscard]] SplinePathFollowAction orient_to_tangent(float& out_rotation) const {
        return SplinePathFollowAction(prop_, spline_, duration_, key_, &out_rotation, ease_);
    }

    void update(float progress, float) noexcept {
        const float eased = ease_(progress);
        prop_ = spline_.evaluate(eased);
        if (rot_ptr_) {
            const auto tan = spline_.tangent(eased);
            *rot_ptr_ = std::atan2(tan.y, tan.x);
        }
    }

    [[nodiscard]] float duration() const noexcept { return duration_; }
    [[nodiscard]] ResourceKey resource_key() const noexcept { return key_; }

private:
    pebble::math::vec2&     prop_;
    SplineT                 spline_;
    float                   duration_;
    ResourceKey             key_;
    float*                  rot_ptr_ = nullptr;
    [[no_unique_address]] E ease_{};
};

template <typename SplineT>
struct FollowPathBuilder {
    pebble::math::vec2& prop;
    SplineT             spline;
    ResourceKey         key;

    [[nodiscard]] SplinePathFollowAction<SplineT> duration(float d = 1.0f) const {
        return SplinePathFollowAction<SplineT>(prop, spline, d, key);
    }
};

template <typename SplineT>
inline FollowPathBuilder<SplineT> follow_path(pebble::math::vec2& prop, SplineT spline, ResourceKey key = kWorldResource) {
    return FollowPathBuilder<SplineT>{prop, std::move(spline), key};
}

template <typename Fn>
inline CallbackAction<std::decay_t<Fn>> callback(Fn cb, ResourceKey key = kWorldResource) {
    return CallbackAction<std::decay_t<Fn>>(std::move(cb), key);
}

} // namespace pebble::spandana::edsl
