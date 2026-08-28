#pragma once
// ============================================================================
// spandana/concepts.hpp — C++23 Concepts & Contracts for Spandana
// ============================================================================
// Zero virtual functions, static policy dispatch.
// ============================================================================

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

} // namespace pebble::spandana
