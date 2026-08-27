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
        // diff points from clamped (box surface) toward circle center (a.center)
        // therefore (clamped - a.center) / d = -diff / d points from circle into box.
        const Vec n = -(diff / d);
        const Scalar depth = a.radius - d;
        const Vec cp = clamped;

        Manifold m;
        m.hit = true;
        m.normal = n; // from circle into box
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
            // Circle center is inside box, normal points from circle towards box center
            n = Vec{a.center.x >= b.center.x ? -1.0f : 1.0f, 0.0f};
            depth = dx + a.radius;
        } else {
            n = Vec{0.0f, a.center.y >= b.center.y ? -1.0f : 1.0f};
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

[[nodiscard]] inline Manifold collide_capsule_capsule(const Capsule& a, const Capsule& b) noexcept {
    // 1. Find closest points between segment A (a.a -> a.b) and segment B (b.a -> b.b)
    const Vec d1 = a.b - a.a;
    const Vec d2 = b.b - b.a;
    const Vec r = a.a - b.a;

    const Scalar a_len2 = d1.len2();
    const Scalar b_len2 = d2.len2();
    const Scalar f = d2.dot(r);

    Scalar s = 0.0f, t = 0.0f;
    if (a_len2 <= 1e-6f && b_len2 <= 1e-6f) {
        s = t = 0.0f;
    } else if (a_len2 <= 1e-6f) {
        s = 0.0f;
        t = std::clamp(f / b_len2, 0.0f, 1.0f);
    } else {
        const Scalar c = d1.dot(r);
        if (b_len2 <= 1e-6f) {
            t = 0.0f;
            s = std::clamp(-c / a_len2, 0.0f, 1.0f);
        } else {
            const Scalar b_dot = d1.dot(d2);
            const Scalar denom = a_len2 * b_len2 - b_dot * b_dot;
            if (denom > 1e-6f) {
                s = std::clamp((b_dot * f - c * b_len2) / denom, 0.0f, 1.0f);
            } else {
                s = 0.0f;
            }
            t = (b_dot * s + f) / b_len2;
            if (t < 0.0f) {
                t = 0.0f;
                s = std::clamp(-c / a_len2, 0.0f, 1.0f);
            } else if (t > 1.0f) {
                t = 1.0f;
                s = std::clamp((b_dot - c) / a_len2, 0.0f, 1.0f);
            }
        }
    }

    const Vec pA = a.a + d1 * s;
    const Vec pB = b.a + d2 * t;
    const Vec diff = pB - pA;
    const Scalar dist2 = diff.len2();
    const Scalar r_sum = a.radius + b.radius;

    if (dist2 >= r_sum * r_sum) return Manifold{};

    const Scalar dist = std::sqrt(std::max(dist2, 0.0f));
    Vec n = (dist > 1e-6f) ? (diff / dist) : Vec{0, 1};
    const Scalar depth = r_sum - dist;

    Manifold m;
    m.hit = true;
    m.normal = n;
    m.depth = depth;

    // Check if segments are roughly parallel for a 2-point manifold
    const Scalar parallel_tol = 0.98f;
    const bool parallel = (std::fabs(d1.normalized().dot(d2.normalized())) > parallel_tol) &&
                          (a_len2 > 1e-4f) && (b_len2 > 1e-4f);

    if (parallel) {
        // Project endpoints of A onto B
        const Vec d2_norm = d2.normalized();
        const Scalar t_a = std::clamp((a.a - b.a).dot(d2_norm) / std::sqrt(b_len2), 0.0f, 1.0f);
        const Vec proj_a = b.a + d2 * t_a;
        const Scalar d_a = (proj_a - a.a).len();

        const Scalar t_b = std::clamp((a.b - b.a).dot(d2_norm) / std::sqrt(b_len2), 0.0f, 1.0f);
        const Vec proj_b = b.a + d2 * t_b;
        const Scalar d_b = (proj_b - a.b).len();

        if (d_a < r_sum) {
            (void)m.points.push_back(ContactPoint{a.a + n * (a.radius - (r_sum - d_a) * 0.5f), r_sum - d_a});
        }
        if (d_b < r_sum && m.points.size() < 2) {
            (void)m.points.push_back(ContactPoint{a.b + n * (a.radius - (r_sum - d_b) * 0.5f), r_sum - d_b});
        }
    }

    if (m.points.empty()) {
        const Vec cp = pA + n * (a.radius - depth * 0.5f);
        (void)m.points.push_back(ContactPoint{cp, depth});
    }

    return m;
}

[[nodiscard]] inline Manifold collide_capsule_obb(const Capsule& a, const OrientedBox& b) noexcept {
    // Transform capsule endpoints to local box coordinates
    const Vec diff_a = a.a - b.center;
    const Vec local_a{b.rot.m00 * diff_a.x + b.rot.m10 * diff_a.y,
                      b.rot.m01 * diff_a.x + diff_a.y * b.rot.m11};

    const Vec diff_b = a.b - b.center;
    const Vec local_b{b.rot.m00 * diff_b.x + b.rot.m10 * diff_b.y,
                      b.rot.m01 * diff_b.x + diff_b.y * b.rot.m11};

    // Test both endpoints against local AABB
    const Circle c_a{a.a, a.radius};
    const Circle c_b{a.b, a.radius};
    const Box local_box{{0, 0}, b.half};

    const Circle local_ca{local_a, a.radius};
    const Circle local_cb{local_b, a.radius};

    const Manifold m_a = collide_circle_box(local_ca, local_box);
    const Manifold m_b = collide_circle_box(local_cb, local_box);

    if (!m_a.hit && !m_b.hit) {
        // Also check capsule midpoint for long capsules straddling box corners
        const Vec local_mid = (local_a + local_b) * 0.5f;
        const Circle local_cm{local_mid, a.radius};
        const Manifold m_m = collide_circle_box(local_cm, local_box);
        if (!m_m.hit) return Manifold{};

        Manifold world_m;
        world_m.hit = true;
        world_m.normal = b.rot * m_m.normal;
        world_m.depth = m_m.depth;
        (void)world_m.points.push_back(ContactPoint{b.center + b.rot * m_m.points[0].point, m_m.depth});
        return world_m;
    }

    Manifold world_m;
    world_m.hit = true;
    Scalar max_depth = 0.0f;

    if (m_a.hit) {
        world_m.normal = b.rot * m_a.normal;
        world_m.depth = m_a.depth;
        max_depth = m_a.depth;
        (void)world_m.points.push_back(ContactPoint{b.center + b.rot * m_a.points[0].point, m_a.depth});
    }

    if (m_b.hit) {
        const Vec norm_b = b.rot * m_b.normal;
        if (world_m.points.empty()) {
            world_m.normal = norm_b;
            world_m.depth = m_b.depth;
        } else {
            // Average normal if both collide
            world_m.normal = (world_m.normal + norm_b).normalized();
            world_m.depth = std::max(world_m.depth, m_b.depth);
        }
        (void)world_m.points.push_back(ContactPoint{b.center + b.rot * m_b.points[0].point, m_b.depth});
    }

    return world_m;
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

    // Ensure best_axis points from A toward B
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

    // Find reference face on A (outward normal closest to best_axis)
    int ref_idx = 0;
    Scalar max_dot = -1e18f;
    for (int i = 0; i < 4; ++i) {
        const Vec v_curr = a_verts[static_cast<std::size_t>(i)];
        const Vec v_next = a_verts[static_cast<std::size_t>((i + 1) % 4)];
        const Vec e = v_next - v_curr;
        // In CCW order, outward normal is (e.y, -e.x)
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
    const Vec ref_normal = Vec{ref_tangent.y, -ref_tangent.x}.normalized();

    // Find incident face on B (outward normal most anti-parallel to ref_normal)
    int inc_idx = 0;
    Scalar min_inc_dot = 1e18f;
    for (int i = 0; i < 4; ++i) {
        const Vec v_curr = b_verts[static_cast<std::size_t>(i)];
        const Vec v_next = b_verts[static_cast<std::size_t>((i + 1) % 4)];
        const Vec e = v_next - v_curr;
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

// ── 4. Direct Static Narrowphase Matrix Dispatch Table ───────────────────────────

enum class ShapeType : std::uint8_t {
    Circle = 0,
    Box,
    Capsule,
    OrientedBox,
    Triangle,
    RoundedBox,
    Sector,
    Segment,
    ConvexPoly,
    RoundedPoly,
    Count
};

inline constexpr std::size_t kShapeTypeCount = static_cast<std::size_t>(ShapeType::Count);

using NarrowphaseFn = Manifold (*)(const void*, const void*, SimplexCache*) noexcept;

namespace detail {

template<typename A, typename B>
inline Manifold dispatch_narrow(const void* a_ptr, const void* b_ptr, SimplexCache* cache) noexcept {
    const auto& a = *static_cast<const A*>(a_ptr);
    const auto& b = *static_cast<const B*>(b_ptr);

    if constexpr (std::is_same_v<A, Circle> && std::is_same_v<B, Circle>) {
        return collide_circle_circle(a, b);
    } else if constexpr (std::is_same_v<A, Circle> && std::is_same_v<B, Capsule>) {
        return collide_circle_capsule(a, b);
    } else if constexpr (std::is_same_v<A, Capsule> && std::is_same_v<B, Circle>) {
        auto m = collide_circle_capsule(b, a);
        if (m.hit) m.normal = -m.normal;
        return m;
    } else if constexpr (std::is_same_v<A, Circle> && std::is_same_v<B, Box>) {
        return collide_circle_box(a, b);
    } else if constexpr (std::is_same_v<A, Box> && std::is_same_v<B, Circle>) {
        auto m = collide_circle_box(b, a);
        if (m.hit) m.normal = -m.normal;
        return m;
    } else if constexpr (std::is_same_v<A, Box> && std::is_same_v<B, Box>) {
        return collide_box_box(a, b);
    } else if constexpr (std::is_same_v<A, OrientedBox> && std::is_same_v<B, OrientedBox>) {
        return collide_obb_obb(a, b);
    } else if constexpr (std::is_same_v<A, Capsule> && std::is_same_v<B, Capsule>) {
        return collide_capsule_capsule(a, b);
    } else if constexpr (std::is_same_v<A, Capsule> && std::is_same_v<B, OrientedBox>) {
        return collide_capsule_obb(a, b);
    } else if constexpr (std::is_same_v<A, OrientedBox> && std::is_same_v<B, Capsule>) {
        auto m = collide_capsule_obb(b, a);
        if (m.hit) m.normal = -m.normal;
        return m;
    } else {
        return collide_gjk_warm_started(a, b, cache);
    }
}

template<std::size_t PolyVerts = 8>
struct NarrowphaseMatrixTable {
    NarrowphaseFn table[kShapeTypeCount][kShapeTypeCount]{};

    constexpr NarrowphaseMatrixTable() {
        populate<Circle, 0>();
        populate<Box, 1>();
        populate<Capsule, 2>();
        populate<OrientedBox, 3>();
        populate<Triangle, 4>();
        populate<RoundedBox, 5>();
        populate<Sector, 6>();
        populate<Segment, 7>();
        populate<ConvexPoly<PolyVerts>, 8>();
        populate<RoundedPoly<PolyVerts>, 9>();
    }

private:
    template<typename A, std::size_t Row>
    constexpr void populate() {
        populate_col<A, Circle, Row, 0>();
        populate_col<A, Box, Row, 1>();
        populate_col<A, Capsule, Row, 2>();
        populate_col<A, OrientedBox, Row, 3>();
        populate_col<A, Triangle, Row, 4>();
        populate_col<A, RoundedBox, Row, 5>();
        populate_col<A, Sector, Row, 6>();
        populate_col<A, Segment, Row, 7>();
        populate_col<A, ConvexPoly<PolyVerts>, Row, 8>();
        populate_col<A, RoundedPoly<PolyVerts>, Row, 9>();
    }

    template<typename A, typename B, std::size_t Row, std::size_t Col>
    constexpr void populate_col() {
        table[Row][Col] = &dispatch_narrow<A, B>;
    }
};

} // namespace detail

template<std::size_t PolyVerts = 8>
inline const detail::NarrowphaseMatrixTable<PolyVerts>& narrowphase_matrix() noexcept {
    static const detail::NarrowphaseMatrixTable<PolyVerts> kTable{};
    return kTable;
}

// O(1) Matrix Dispatch narrowphase with warm-started GJK/EPA simplex caching
template<std::size_t PolyVerts = 8>
[[nodiscard]] inline Manifold collide_matrix(ShapeType type_a, const void* shape_a,
                                             ShapeType type_b, const void* shape_b,
                                             SimplexCache* cache = nullptr) noexcept {
    const auto idx_a = static_cast<std::size_t>(type_a);
    const auto idx_b = static_cast<std::size_t>(type_b);
    if (idx_a >= kShapeTypeCount || idx_b >= kShapeTypeCount) return Manifold{};
    return narrowphase_matrix<PolyVerts>().table[idx_a][idx_b](shape_a, shape_b, cache);
}

} // namespace akruti

