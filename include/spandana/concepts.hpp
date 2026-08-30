#pragma once
// ============================================================================
// spandana/concepts.hpp — C++23 Concepts & Contracts for Spandana
// ============================================================================
// Zero virtual functions, static policy dispatch.
// ============================================================================

#include "resource_key.hpp"
#include "containers/numeric/math_vector.hpp"
#include <concepts>
#include <type_traits>
#include <utility>

namespace pebble::spandana {

// 1. Easing Function Contract: maps progress t in [0, 1] to eased value
template <typename E>
concept EasingFunction = requires(const E& e, float t) {
    { e(t) } noexcept -> std::convertible_to<float>;
};

// 2. Spring Solver Contract: closed-form or numerical second-order damper
template <typename S>
concept SpringSolver = requires(S s, float current, float velocity, float target, float dt) {
    { s.step(current, velocity, target, dt) } noexcept -> std::same_as<std::pair<float, float>>;
};

// 3. 2D Inverse Kinematics Contract
template <typename IK>
concept IKSolver2D = requires(IK ik, pebble::math::vec2 root, pebble::math::vec2 target) {
    { ik.solve(root, target) } noexcept;
};

// 4. Tweenable Type Contract: supports pebble::math::lerp
template <typename T>
concept Tweenable = requires(T a, T b, float t) {
    { pebble::math::lerp(a, b, t) } -> std::same_as<T>;
};

// Specialized float lerp overload
inline float lerp(float a, float b, float t) noexcept {
    return a + (b - a) * t;
}

// 5. Animation Action Contract — the timeline's unit of work.
//    Replaces the old virtual `IAnimationAction`. Any value type that models
//    this concept can be added to a Timeline; dispatch is fully static, so the
//    Timeline erases it into a fixed inline buffer (no heap, no vtable, no RTTI).
//
//    Required surface:
//      - update(elapsed, dt)      : advance the action; called once per frame
//      - duration()               : total playback length in seconds
//      - resource_key()           : the resource this action writes (drives
//                                   auto dependency/parallelism inference)
//    Optional surface (detected, defaulted to no-op when absent):
//      - on_start()               : fired once when the action first runs
//      - on_complete()            : fired once when elapsed >= duration()
template <typename A>
concept AnimationAction = requires(A& a, const A& ca, float elapsed, float dt) {
    { a.update(elapsed, dt) } noexcept;
    { ca.duration() } noexcept -> std::convertible_to<float>;
    { ca.resource_key() } noexcept -> std::convertible_to<ResourceKey>;
};

// 6. Action Builder Contract — a fluent builder that finalizes into an action.
//    `.add()` accepts either an AnimationAction directly or an ActionBuilder,
//    in which case `.build()` is invoked to obtain the action value.
template <typename B>
concept ActionBuilder = requires(B&& b) {
    { std::forward<B>(b).build() };
} && AnimationAction<std::remove_cvref_t<decltype(std::declval<B&&>().build())>>;

// Optional-method detection for the erased action vtable.
template <typename A>
concept HasOnStart = requires(A& a) { a.on_start(); };

template <typename A>
concept HasOnComplete = requires(A& a) { a.on_complete(); };

} // namespace pebble::spandana
