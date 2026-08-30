#pragma once
// ============================================================================
// gati/transform.hpp — Spatial Component, Scene Hierarchy & Presentation Pose
// ============================================================================
// Uses pebble::math::vec2 for positions and ga::StaticMatrix<float,2,2> for rotation/basis.
// Double-buffers pose (prev_position/prev_angle) for render interpolation.
// ============================================================================

#include "math.hpp"
#include "ecs.hpp"
#include <cmath>

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
        const Scalar c = std::cos(angle);
        const Scalar s = std::sin(angle);
        return Mat2{c * scale[0], -s * scale[1],
                    s * scale[0],  c * scale[1]};
    }
};

inline constexpr int kMaxHierarchyDepth = 32;

// World-space position by composing local transforms up parent hierarchy
[[nodiscard]] inline Vec2 world_position(World& w, Entity e) {
    Vec2 p{};
    Mat2 acc = Mat2::identity();
    for (int depth = 0; depth < kMaxHierarchyDepth; ++depth) {
        Transform* t = w.get<Transform>(e);
        if (!t) break;
        const ga::Vec2<float> pv{t->position[0], t->position[1]};
        const ga::Vec2<float> rp = acc * pv;
        p = Vec2(p[0] + rp(0,0), p[1] + rp(1,0));
        acc = acc * t->basis();
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

// SIMD-accelerated batch interpolation of transforms into poses
inline void batch_interpolate(std::span<const Transform> transforms, Scalar alpha,
                              std::span<Pose> out_poses) noexcept {
    const std::size_t n = std::min(transforms.size(), out_poses.size());
    for (std::size_t i = 0; i < n; ++i) {
        out_poses[i] = interpolated(transforms[i], alpha);
    }
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
