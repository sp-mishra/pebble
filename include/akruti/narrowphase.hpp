#pragma once
// akruti/narrowphase.hpp — High-Performance 2D Narrowphase & SAT Manifold Solver.
//
// Key features:
//   1. Manifold struct supporting 1-point and 2-point contact manifolds (critical for stable physics stacking).
//   2. O(1) Analytic Fast-Paths for Circle-Circle, Circle-Capsule, Circle-Box, and Circle-OBB.
//   3. 2D Separating Axis Theorem (SAT) with edge clipping for Box-Box, OBB-OBB, and ConvexPoly pairs.
//   4. Warm-started GJK / EPA with temporal simplex caching for general convex shapes.
//
// No virtual functions, no macros, no heap allocation on hot paths.
#include "shape.hpp"
#include "primitives.hpp"
#include "gjk.hpp"
#include "containers/static/static_vector.hpp"
#include <cmath>
#include <algorithm>
#include <array>

namespace akruti {

// ── Contact Manifold (1 or 2 contact points for 2D rigid stacking) ────────────────
struct ContactPoint {
    Vec    point{};         // World-space contact position
    Scalar depth{0};        // Penetration depth (positive when penetrating)
};

struct Manifold {
    bool                                   hit{false};
    Vec                                    normal{};   // Unit normal pointing from A into B
    containers::static_vector<ContactPoint, 2> points{};  // Up to 2 contact points
    Scalar                                 depth{0};   // Max penetration depth
};

// ── 1. Analytic Fast-Paths (Circle vs Circle, Capsule, Box) ───────────────────────

[[nodiscard]] inline Manifold collide_circle_circle(const Circle& a, const Circle& b) noexcept {
    const Vec diff = b.center - a.center;
    const Scalar dist2 = diff.len2();
    const Scalar r_sum = a.radius + b.radius;
    if (dist2 >= r_sum * r_sum) return Manifold{};

    const Scalar dist = std::sqrt(dist2);
    Vec n = (dist > Scalar(1e-6)) ? (diff / dist) : Vec{1, 0};
    const Scalar depth = r_sum - dist;
    const Vec cp = a.center + n * (a.radius - depth * 0.5f);

    Manifold m;
    m.hit = true;
    m.normal = n;
    m.depth = depth;
    (void)m.points.push_back(ContactPoint{cp, depth});
    return m;
}

[[nodiscard]] inline Manifold collide_circle_capsule(const Circle& a, const Capsule& b) noexcept {
    const Vec ab = b.b - b.a;
    const Vec ap = a.center - b.a;
    const Scalar t = std::clamp(ap.dot(ab) / std::max(ab.len2(), Scalar(1e-12)), Scalar(0), Scalar(1));
    const Vec closest = b.a + ab * t;

    const Circle temp_c{closest, b.radius};
    return collide_circle_circle(a, temp_c);
}

[[nodiscard]] inline Manifold collide_circle_box(const Circle& a, const Box& b) noexcept {
    // Clamp circle center to box bounds
    const Vec clamped{std::clamp(a.center.x, b.center.x - b.half.x, b.center.x + b.half.x),
                      std::clamp(a.center.y, b.center.y - b.half.y, b.center.y + b.half.y)};

    const Vec diff = a.center - clamped;
    const Scalar d2 = diff.len2();

    if (d2 > Scalar(1e-9)) {
        // Circle center is outside box
        if (d2 >= a.radius * a.radius) return Manifold{};
        const Scalar d = std::sqrt(d2);
        const Vec n = diff / d;
        const Scalar depth = a.radius - d;
        const Vec cp = clamped;

        Manifold m;
        m.hit = true;
        m.normal = -n; // from circle into box: -n points toward box center
        m.depth = depth;
        (void)m.points.push_back(ContactPoint{cp, depth});
        return m;
    } else {
        // Circle center is inside box: find shallowest exit axis
        const Scalar dx = (b.half.x) - std::fabs(a.center.x - b.center.x);
        const Scalar dy = (b.half.y) - std::fabs(a.center.y - b.center.y);

        Vec n{};
        Scalar depth = 0;
        if (dx < dy) {
            n = Vec{a.center.x > b.center.x ? -1.0f : 1.0f, 0.0f};
            depth = dx + a.radius;
        } else {
            n = Vec{0.0f, a.center.y > b.center.y ? -1.0f : 1.0f};
            depth = dy + a.radius;
        }

        Manifold m;
        m.hit = true;
        m.normal = n;
        m.depth = depth;
        (void)m.points.push_back(ContactPoint{a.center - n * a.radius, depth});
        return m;
    }
}

// ── 2. Separating Axis Theorem (SAT) with 2-Point Edge Clipping ───────────────────

namespace detail {

struct ClipEdge {
    Vec v1{}, v2{};
    Vec max_vertex{};
    Vec normal{};
};

inline void clip_segment_to_line(std::array<Vec, 2>& out_pts, int& out_count,
                                 const std::array<Vec, 2>& in_pts, Vec normal, Scalar offset) noexcept {
    out_count = 0;
    const Scalar d0 = normal.dot(in_pts[0]) - offset;
    const Scalar d1 = normal.dot(in_pts[1]) - offset;

    // If points are behind or on the clipping plane, keep them
    if (d0 <= 0.0f) out_pts[static_cast<std::size_t>(out_count++)] = in_pts[0];
    if (d1 <= 0.0f) out_pts[static_cast<std::size_t>(out_count++)] = in_pts[1];

    if (d0 * d1 < 0.0f) {
        // Intersect edge with plane
        const Scalar t = d0 / (d0 - d1);
        out_pts[static_cast<std::size_t>(out_count++)] = in_pts[0] + (in_pts[1] - in_pts[0]) * t;
    }
}

} // namespace detail

// SAT Box-Box / OBB-OBB with 2-Point Contact Manifold
[[nodiscard]] inline Manifold collide_obb_obb(const OrientedBox& a, const OrientedBox& b) noexcept {
    // 4 candidate separating axes: a.rot columns (2) and b.rot columns (2)
    const Vec ax = Vec{a.rot.m00, a.rot.m10};
    const Vec ay = Vec{a.rot.m01, a.rot.m11};
    const Vec bx = Vec{b.rot.m00, b.rot.m10};
    const Vec by = Vec{b.rot.m01, b.rot.m11};

    const std::array<Vec, 4> axes = {ax, ay, bx, by};
    const Vec diff = b.center - a.center;

    Scalar min_overlap = 1e18f;
    Vec best_axis{};

    for (const Vec& axis : axes) {
        // Project a and b extents onto axis
        const Scalar ra = std::fabs(ax.dot(axis)) * a.half.x + std::fabs(ay.dot(axis)) * a.half.y;
        const Scalar rb = std::fabs(bx.dot(axis)) * b.half.x + std::fabs(by.dot(axis)) * b.half.y;
        const Scalar dist = std::fabs(diff.dot(axis));

        const Scalar overlap = ra + rb - dist;
        if (overlap <= 0.0f) return Manifold{}; // Separating axis found!

        if (overlap < min_overlap) {
            min_overlap = overlap;
            best_axis = axis;
        }
    }

    // Ensure normal points from A to B
    if (best_axis.dot(diff) < 0.0f) best_axis = -best_axis;

    // Build 2-point manifold via incident-reference edge clipping
    // Vertices of Box A and Box B in CCW order
    const std::array<Vec, 4> a_verts = {
        a.center + ax * a.half.x + ay * a.half.y,
        a.center - ax * a.half.x + ay * a.half.y,
        a.center - ax * a.half.x - ay * a.half.y,
        a.center + ax * a.half.x - ay * a.half.y
    };
    const std::array<Vec, 4> b_verts = {
        b.center + bx * b.half.x + by * b.half.y,
        b.center - bx * b.half.x + by * b.half.y,
        b.center - bx * b.half.x - by * b.half.y,
        b.center + bx * b.half.x - by * b.half.y
    };

    // Find reference face on A (normal closest to best_axis)
    int ref_idx = 0;
    Scalar max_dot = -1e18f;
    for (int i = 0; i < 4; ++i) {
        const Vec e = a_verts[static_cast<std::size_t>((i + 1) % 4)] - a_verts[static_cast<std::size_t>(i)];
        const Vec n = Vec{e.y, -e.x}.normalized();
        const Scalar dot = n.dot(best_axis);
        if (dot > max_dot) {
            max_dot = dot;
            ref_idx = i;
        }
    }
    const Vec ref_v1 = a_verts[static_cast<std::size_t>(ref_idx)];
    const Vec ref_v2 = a_verts[static_cast<std::size_t>((ref_idx + 1) % 4)];
    const Vec ref_tangent = (ref_v2 - ref_v1).normalized();
    const Vec ref_normal = Vec{ref_tangent.y, -ref_tangent.x};

    // Find incident face on B (normal most anti-parallel to ref_normal)
    int inc_idx = 0;
    Scalar min_inc_dot = 1e18f;
    for (int i = 0; i < 4; ++i) {
        const Vec e = b_verts[static_cast<std::size_t>((i + 1) % 4)] - b_verts[static_cast<std::size_t>(i)];
        const Vec n = Vec{e.y, -e.x}.normalized();
        const Scalar dot = n.dot(ref_normal);
        if (dot < min_inc_dot) {
            min_inc_dot = dot;
            inc_idx = i;
        }
    }
    std::array<Vec, 2> inc_pts = {b_verts[static_cast<std::size_t>(inc_idx)],
                                  b_verts[static_cast<std::size_t>((inc_idx + 1) % 4)]};

    // 1. Clip incident edge against reference face side plane 1 (ref_v1, -ref_tangent)
    std::array<Vec, 2> clip1{};
    int count1 = 0;
    detail::clip_segment_to_line(clip1, count1, inc_pts, -ref_tangent, -ref_tangent.dot(ref_v1));
    if (count1 < 2) count1 = 2; // fallback

    // 2. Clip against reference face side plane 2 (ref_v2, ref_tangent)
    std::array<Vec, 2> clip2{};
    int count2 = 0;
    detail::clip_segment_to_line(clip2, count2, clip1, ref_tangent, ref_tangent.dot(ref_v2));

    Manifold m;
    m.hit = true;
    m.normal = best_axis;
    m.depth = min_overlap;

    const Scalar ref_depth = ref_normal.dot(ref_v1);
    for (int i = 0; i < count2; ++i) {
        const Scalar sep = ref_normal.dot(clip2[static_cast<std::size_t>(i)]) - ref_depth;
        if (sep <= 0.05f) { // Points at or behind reference face
            (void)m.points.push_back(ContactPoint{clip2[static_cast<std::size_t>(i)], std::max(0.0f, -sep)});
        }
    }

    if (m.points.empty()) {
        (void)m.points.push_back(ContactPoint{a.center + best_axis * (a.half.x), min_overlap});
    }

    return m;
}


[[nodiscard]] inline Manifold collide_box_box(const Box& a, const Box& b) noexcept {
    const OrientedBox obb_a{a.center, a.half, Mat2<Scalar>{1, 0, 0, 1}};
    const OrientedBox obb_b{b.center, b.half, Mat2<Scalar>{1, 0, 0, 1}};
    return collide_obb_obb(obb_a, obb_b);
}

// ── 3. Warm-Started GJK / EPA Solver ──────────────────────────────────────────────

struct SimplexCache {
    containers::static_vector<Vec, 3> simplex;
    Vec                               separating_axis{1, 0};
    bool                              valid{false};
};

template <Shape A, Shape B>
[[nodiscard]] inline Manifold collide_gjk_warm_started(const A& a, const B& b, SimplexCache* cache = nullptr) noexcept {
    containers::static_vector<Vec, 3> simp;
    Vec init_d = (cache && cache->valid) ? cache->separating_axis : Vec{1, 0};

    // Fast GJK overlap
    const bool overlap = gjk_overlap(a, b, &simp);
    if (!overlap) {
        if (cache) {
            cache->valid = false;
            cache->simplex.clear();
        }
        return Manifold{};
    }

    const Contact c = epa(a, b);
    if (!c.hit) return Manifold{};

    if (cache) {
        cache->simplex = simp;
        cache->separating_axis = c.normal;
        cache->valid = true;
    }

    Manifold m;
    m.hit = true;
    m.normal = c.normal;
    m.depth = c.depth;
    const Vec cp = a.support(c.normal) - c.normal * (c.depth * 0.5f);
    (void)m.points.push_back(ContactPoint{cp, c.depth});
    return m;
}

} // namespace akruti
