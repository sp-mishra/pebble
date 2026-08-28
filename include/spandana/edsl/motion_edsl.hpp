#pragma once
// ============================================================================
// spandana/edsl/motion_edsl.hpp — Declarative Motion, Tween & Spring EDSL Directives
// ============================================================================

#include "../timeline.hpp"
#include "../easing.hpp"
#include "../spring.hpp"
#include "containers/numeric/math_vector.hpp"

namespace pebble::spandana::edsl {

// Tween Action targeting a float property
class FloatTweenAction : public IAnimationAction {
public:
    FloatTweenAction(float& prop, float target, float duration, ResourceKey key)
        : prop_(prop), start_(prop), target_(target), duration_(duration), key_(key) {}

    template <EasingFunction E>
    FloatTweenAction& ease(E e) {
        ease_fn_ = [e](float t) { return e(t); };
        return *this;
    }

    void on_start() override {
        start_ = prop_;
    }

    void update(float progress, float) override {
        const float eased = ease_fn_ ? ease_fn_(progress) : progress;
        prop_ = lerp(start_, target_, eased);
    }

    [[nodiscard]] float duration() const noexcept override { return duration_; }
    [[nodiscard]] ResourceKey resource_key() const noexcept override { return key_; }

private:
    float&                       prop_;
    float                        start_;
    float                        target_;
    float                        duration_;
    ResourceKey                  key_;
    std::function<float(float)>  ease_fn_;
};

// Tween Action targeting a pebble::math::vec2 property
class Vec2TweenAction : public IAnimationAction {
public:
    Vec2TweenAction(pebble::math::vec2& prop, pebble::math::vec2 target, float duration, ResourceKey key)
        : prop_(prop), start_(prop), target_(target), duration_(duration), key_(key) {}

    template <EasingFunction E>
    Vec2TweenAction& ease(E e) {
        ease_fn_ = [e](float t) { return e(t); };
        return *this;
    }

    void on_start() override {
        start_ = prop_;
    }

    void update(float progress, float) override {
        const float eased = ease_fn_ ? ease_fn_(progress) : progress;
        prop_ = pebble::math::lerp(start_, target_, eased);
    }

    [[nodiscard]] float duration() const noexcept override { return duration_; }
    [[nodiscard]] ResourceKey resource_key() const noexcept override { return key_; }

private:
    pebble::math::vec2&          prop_;
    pebble::math::vec2           start_;
    pebble::math::vec2           target_;
    float                        duration_;
    ResourceKey                  key_;
    std::function<float(float)>  ease_fn_;
};

// Spring Action targeting a pebble::math::vec2 property
class Vec2SpringAction : public IAnimationAction {
public:
    Vec2SpringAction(pebble::math::vec2& prop, pebble::math::vec2 target, float stiffness, float damping, float duration, ResourceKey key)
        : prop_(prop), target_(target), spring_(stiffness, damping), duration_(duration), key_(key) {}

    void update(float, float dt) override {
        auto [pos, vel] = spring_.step(prop_, vel_, target_, dt);
        prop_ = pos;
        vel_ = vel;
    }

    [[nodiscard]] float duration() const noexcept override { return duration_; }
    [[nodiscard]] ResourceKey resource_key() const noexcept override { return key_; }

private:
    pebble::math::vec2&          prop_;
    pebble::math::vec2           target_;
    pebble::math::vec2           vel_{};
    Vector2SpringDamper          spring_;
    float                        duration_;
    ResourceKey                  key_;
};

// Callback Action
class CallbackAction : public IAnimationAction {
public:
    explicit CallbackAction(std::function<void()> cb, ResourceKey key = kWorldResource)
        : cb_(std::move(cb)), key_(key) {}

    void on_start() override {
        if (cb_) cb_();
    }
    void update(float, float) override {}
    [[nodiscard]] float duration() const noexcept override { return 0.0f; }
    [[nodiscard]] ResourceKey resource_key() const noexcept override { return key_; }

private:
    std::function<void()> cb_;
    ResourceKey key_;
};

// Builders
struct FloatTweenBuilder {
    float& prop;
    ResourceKey key;

    [[nodiscard]] FloatTweenAction to(float target, float duration = 0.3f) const {
        return FloatTweenAction(prop, target, duration, key);
    }
};

struct Vec2TweenBuilder {
    pebble::math::vec2& prop;
    ResourceKey key;

    [[nodiscard]] Vec2TweenAction to(pebble::math::vec2 target, float duration = 0.3f) const {
        return Vec2TweenAction(prop, target, duration, key);
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

// Follow Path Action for Akruti Splines
template <typename SplineT>
class SplinePathFollowAction : public IAnimationAction {
public:
    SplinePathFollowAction(pebble::math::vec2& prop, SplineT spline, float duration, ResourceKey key)
        : prop_(prop), spline_(std::move(spline)), duration_(duration), key_(key) {}

    template <EasingFunction E>
    SplinePathFollowAction& ease(E e) {
        ease_fn_ = [e](float t) { return e(t); };
        return *this;
    }

    SplinePathFollowAction& orient_to_tangent(float& out_rotation) {
        rot_ptr_ = &out_rotation;
        return *this;
    }

    void update(float progress, float) override {
        const float eased = ease_fn_ ? ease_fn_(progress) : progress;
        prop_ = spline_.evaluate(eased);
        if (rot_ptr_) {
            const auto tan = spline_.tangent(eased);
            *rot_ptr_ = std::atan2(tan.y, tan.x);
        }
    }

    [[nodiscard]] float duration() const noexcept override { return duration_; }
    [[nodiscard]] ResourceKey resource_key() const noexcept override { return key_; }

private:
    pebble::math::vec2&         prop_;
    SplineT                     spline_;
    float                       duration_;
    ResourceKey                 key_;
    float*                      rot_ptr_ = nullptr;
    std::function<float(float)> ease_fn_;
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

inline CallbackAction callback(std::function<void()> cb, ResourceKey key = kWorldResource) {
    return CallbackAction(std::move(cb), key);
}

} // namespace pebble::spandana::edsl

