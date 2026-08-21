#pragma once
// ============================================================================
// gati/transform.hpp — Spatial Component, Scene Hierarchy & Presentation Pose
// ============================================================================
// Uses pebble::math::vec2 and pebble::math::mat2 directly.
// Double-buffers pose (prev_position/prev_angle) for render interpolation.
// ============================================================================

#include "math.hpp"
#include "ecs.hpp"

namespace gati {

struct Transform {
    Vec2   position{};
    Scalar angle = 0.0f;               // radians
    Vec2   scale{1.0f, 1.0f};
    Vec2   prev_position{};            // pose at previous fixed step (for interpolation)
    Scalar prev_angle = 0.0f;
    Entity parent = null_entity;

    constexpr void checkpoint() noexcept {
        prev_position = position;
        prev_angle = angle;
    }

    // Local 2x2 basis (rotation * scale)
    [[nodiscard]] Mat2 basis() const noexcept {
        Mat2 r = pebble::math::rotation2d(angle);
        return Mat2(
            r[0, 0] * scale[0], r[0, 1] * scale[1],
            r[1, 0] * scale[0], r[1, 1] * scale[1]
        );
    }
};

inline constexpr int kMaxHierarchyDepth = 32;

// World-space position by composing local transforms up parent hierarchy
[[nodiscard]] inline Vec2 world_position(World& w, Entity e) {
    Vec2 p{};
    Mat2 acc = Mat2(1.0f, 0.0f, 0.0f, 1.0f);
    for (int depth = 0; depth < kMaxHierarchyDepth; ++depth) {
        Transform* t = w.get<Transform>(e);
        if (!t) break;
        p = pebble::math::mul(acc, t->position) + p;
        acc = pebble::math::mul(acc, t->basis());
        if (t->parent.is_null()) break;
        e = t->parent;
    }
    return p;
}

// Interpolated pose for rendering: lerp(prev, cur, alpha)
struct Pose {
    Vec2   position;
    Scalar angle;
};

[[nodiscard]] inline Pose interpolated(const Transform& t, Scalar alpha) noexcept {
    return {
        lerp(t.prev_position, t.position, alpha),
        angle_lerp(t.prev_angle, t.angle, alpha)
    };
}

// Reparent with cycle rejection
inline bool set_parent(World& w, Entity e, Entity parent) {
    Entity cur = parent;
    for (int d = 0; d < kMaxHierarchyDepth && !cur.is_null(); ++d) {
        if (cur == e) return false; // Cycle detected
        Transform* t = w.get<Transform>(cur);
        cur = t ? t->parent : null_entity;
    }
    if (Transform* t = w.get<Transform>(e)) {
        t->parent = parent;
        return true;
    }
    return false;
}

} // namespace gati
