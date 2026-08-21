#pragma once
// akruti/primitives.hpp — analytic 2D shape primitives. Each satisfies the Shape concept
// (sdf / aabb / support). SDFs are exact signed distances (negative inside). Support
// functions enable GJK/EPA. constexpr where the math allows; no virtual, no macros.
#include "shape.hpp"
#include "containers/static/static_vector.hpp"
#include <cmath>
#include <algorithm>
#include <numbers>

namespace akruti {

using Vec = Vec2<Scalar>;
using Box2 = AABB<Scalar>;

// ── Circle ────────────────────────────────────────────────────────────────────
struct Circle {
    Vec    center{};
    Scalar radius{1};

    [[nodiscard]] Scalar sdf(Vec p) const noexcept { return (p - center).len() - radius; }
    [[nodiscard]] constexpr Box2 aabb() const noexcept {
        return {{center.x - radius, center.y - radius}, {center.x + radius, center.y + radius}};
    }
    [[nodiscard]] Vec support(Vec d) const noexcept { return center + d.normalized() * radius; }
};

// ── Segment (thick=0 line) ──────────────────────────────────────────────────────
struct Segment {
    Vec a{}, b{};

    [[nodiscard]] Scalar sdf(Vec p) const noexcept {
        const Vec ab = b - a, ap = p - a;
        const Scalar t = std::clamp(ap.dot(ab) / std::max(ab.len2(), Scalar(1e-12)), Scalar(0), Scalar(1));
        return (ap - ab * t).len();
    }
    [[nodiscard]] constexpr Box2 aabb() const noexcept {
        return {{std::min(a.x, b.x), std::min(a.y, b.y)}, {std::max(a.x, b.x), std::max(a.y, b.y)}};
    }
    [[nodiscard]] constexpr Vec support(Vec d) const noexcept { return d.dot(a) >= d.dot(b) ? a : b; }
};

// ── Capsule (segment inflated by radius) ─────────────────────────────────────────
struct Capsule {
    Vec    a{}, b{};
    Scalar radius{1};

    [[nodiscard]] Scalar sdf(Vec p) const noexcept { return Segment{a, b}.sdf(p) - radius; }
    [[nodiscard]] constexpr Box2 aabb() const noexcept {
        return {{std::min(a.x, b.x) - radius, std::min(a.y, b.y) - radius},
                {std::max(a.x, b.x) + radius, std::max(a.y, b.y) + radius}};
    }
    [[nodiscard]] Vec support(Vec d) const noexcept {
        const Vec base = d.dot(a) >= d.dot(b) ? a : b;
        return base + d.normalized() * radius;
    }
};

// ── Axis-aligned box (center + half-extents) ──────────────────────────────────────
struct Box {
    Vec center{};
    Vec half{1, 1};

    [[nodiscard]] Scalar sdf(Vec p) const noexcept {
        const Vec q{std::fabs(p.x - center.x) - half.x, std::fabs(p.y - center.y) - half.y};
        const Vec qp{std::max(q.x, Scalar(0)), std::max(q.y, Scalar(0))};
        return qp.len() + std::min(std::max(q.x, q.y), Scalar(0));
    }
    [[nodiscard]] constexpr Box2 aabb() const noexcept {
        return {{center.x - half.x, center.y - half.y}, {center.x + half.x, center.y + half.y}};
    }
    [[nodiscard]] constexpr Vec support(Vec d) const noexcept {
        return {center.x + (d.x >= 0 ? half.x : -half.x), center.y + (d.y >= 0 ? half.y : -half.y)};
    }
};

// ── Oriented Bounding Box (OBB: center + half + rotation) ─────────────────────────
struct OrientedBox {
    Vec         center{};
    Vec         half{1, 1};
    Mat2<Scalar> rot{1, 0, 0, 1}; // forward rotation matrix (local to world)

    [[nodiscard]] static OrientedBox from_angle(Vec center, Vec half, Scalar radians) noexcept {
        return {center, half, Mat2<Scalar>::rotation(radians)};
    }

    [[nodiscard]] Scalar sdf(Vec p) const noexcept {
        // Map p to local unrotated space: p_local = rot^T * (p - center)
        const Vec diff = p - center;
        const Vec local{rot.m00 * diff.x + rot.m10 * diff.y,
                        rot.m01 * diff.x + diff.y * rot.m11};
        const Vec q{std::fabs(local.x) - half.x, std::fabs(local.y) - half.y};
        const Vec qp{std::max(q.x, Scalar(0)), std::max(q.y, Scalar(0))};
        return qp.len() + std::min(std::max(q.x, q.y), Scalar(0));
    }

    [[nodiscard]] Box2 aabb() const noexcept {
        // Extents along world axes: e_x = |rot00|*hx + |rot01|*hy
        const Scalar ex = std::fabs(rot.m00) * half.x + std::fabs(rot.m01) * half.y;
        const Scalar ey = std::fabs(rot.m10) * half.x + std::fabs(rot.m11) * half.y;
        return {{center.x - ex, center.y - ey}, {center.x + ex, center.y + ey}};
    }

    [[nodiscard]] Vec support(Vec d) const noexcept {
        // Transform direction to local, find support on unrotated box, rotate back
        const Vec d_local{rot.m00 * d.x + rot.m10 * d.y,
                          rot.m01 * d.x + d.y * rot.m11};
        const Vec sup_local{d_local.x >= 0 ? half.x : -half.x,
                            d_local.y >= 0 ? half.y : -half.y};
        return center + rot * sup_local;
    }
};

// ── Triangle: exact 2D barycentric distance ───────────────────────────────────────
struct Triangle {
    Vec a{}, b{}, c{};

    [[nodiscard]] Scalar sdf(Vec p) const noexcept {
        const Vec e0 = b - a, e1 = c - b, e2 = a - c;
        const Vec v0 = p - a, v1 = p - b, v2 = p - c;

        const Vec pq0 = v0 - e0 * std::clamp(v0.dot(e0) / std::max(e0.len2(), Scalar(1e-12)), Scalar(0), Scalar(1));
        const Vec pq1 = v1 - e1 * std::clamp(v1.dot(e1) / std::max(e1.len2(), Scalar(1e-12)), Scalar(0), Scalar(1));
        const Vec pq2 = v2 - e2 * std::clamp(v2.dot(e2) / std::max(e2.len2(), Scalar(1e-12)), Scalar(0), Scalar(1));

        const Scalar s = cross(e0, e2); // CCW sign
        const Scalar d = std::min({pq0.len2(), pq1.len2(), pq2.len2()});

        // Inside if all on same side of edges
        const Scalar z0 = cross(e0, v0) * s;
        const Scalar z1 = cross(e1, v1) * s;
        const Scalar z2 = cross(e2, v2) * s;
        const bool inside = (z0 <= 0 && z1 <= 0 && z2 <= 0);

        return (inside ? -1.0f : 1.0f) * std::sqrt(d);
    }

    [[nodiscard]] constexpr Box2 aabb() const noexcept {
        return {{std::min({a.x, b.x, c.x}), std::min({a.y, b.y, c.y})},
                {std::max({a.x, b.x, c.x}), std::max({a.y, b.y, c.y})}};
    }

    [[nodiscard]] constexpr Vec support(Vec d) const noexcept {
        const Scalar da = d.dot(a), db = d.dot(b), dc = d.dot(c);
        if (da >= db && da >= dc) return a;
        if (db >= da && db >= dc) return b;
        return c;
    }
};

// ── Rounded Box (Box with corner radius) ──────────────────────────────────────────
struct RoundedBox {
    Vec    center{};
    Vec    half{1, 1};
    Scalar radius{0.1f};

    [[nodiscard]] Scalar sdf(Vec p) const noexcept {
        const Vec q{std::fabs(p.x - center.x) - half.x + radius,
                    std::fabs(p.y - center.y) - half.y + radius};
        const Vec qp{std::max(q.x, Scalar(0)), std::max(q.y, Scalar(0))};
        return qp.len() + std::min(std::max(q.x, q.y), Scalar(0)) - radius;
    }
    [[nodiscard]] constexpr Box2 aabb() const noexcept {
        return {{center.x - half.x, center.y - half.y}, {center.x + half.x, center.y + half.y}};
    }
    [[nodiscard]] Vec support(Vec d) const noexcept {
        const Vec inner_half{std::max(Scalar(0), half.x - radius), std::max(Scalar(0), half.y - radius)};
        const Vec b_sup{center.x + (d.x >= 0 ? inner_half.x : -inner_half.x),
                        center.y + (d.y >= 0 ? inner_half.y : -inner_half.y)};
        return b_sup + d.normalized() * radius;
    }
};

// ── Arc / Sector (FOV cone / radar sensor primitive) ──────────────────────────────
struct Sector {
    Vec    center{};
    Scalar radius{1};
    Scalar half_angle{0.785398f}; // half opening angle in radians (~45 deg)
    Mat2<Scalar> rot{1, 0, 0, 1}; // forward orientation matrix

    [[nodiscard]] static Sector from_direction(Vec center, Scalar r, Scalar half_ang, Vec dir) noexcept {
        const Vec nd = dir.normalized();
        // Rotation aligning +X with nd
        return {center, r, half_ang, Mat2<Scalar>{nd.x, -nd.y, nd.y, nd.x}};
    }

    [[nodiscard]] Scalar sdf(Vec p) const noexcept {
        const Vec diff = p - center;
        // Map to local unrotated space where cone axis is +X
        const Vec q{rot.m00 * diff.x + rot.m10 * diff.y,
                    rot.m01 * diff.x + diff.y * rot.m11};
        const Scalar len = q.len();
        const Scalar sin_a = std::sin(half_angle), cos_a = std::cos(half_angle);
        const Vec cs{cos_a, sin_a};

        const Vec p_rot{std::fabs(q.y), q.x};
        const Scalar z = p_rot.x * cs.x - p_rot.y * cs.y;
        if (z <= 0) {
            return len - radius;
        }
        const Vec edge = cs * std::clamp(q.dot(Vec{cs.x, cs.y}), Scalar(0), radius);
        return (q - Vec{edge.x, std::copysign(edge.y, q.y)}).len();
    }

    [[nodiscard]] Box2 aabb() const noexcept {
        // Conservative circle bound
        return {{center.x - radius, center.y - radius}, {center.x + radius, center.y + radius}};
    }

    [[nodiscard]] Vec support(Vec d) const noexcept {
        const Vec nd = d.normalized();
        return center + nd * radius;
    }
};

// ── Half-plane: signed distance to a line through `point` with unit outward `normal`.
struct HalfPlane {
    Vec normal{0, 1}; // points OUT of the solid (solid is the negative-distance side)
    Vec point{};

    [[nodiscard]] Scalar sdf(Vec p) const noexcept { return normal.normalized().dot(p - point); }
    [[nodiscard]] constexpr Box2 aabb() const noexcept {
        constexpr Scalar big = Scalar(1e18);
        return {{-big, -big}, {big, big}};
    }
    [[nodiscard]] Vec support(Vec d) const noexcept {
        // Unbounded; project a far point against the ray direction.
        return point - normal.normalized() * (d.dot(normal) > 0 ? Scalar(0) : Scalar(1e18));
    }
};

// ── Convex polygon (CCW winding). Verts in a fixed-capacity buffer (no heap). ─────
template <std::size_t N = 8>
struct ConvexPoly {
    containers::static_vector<Vec, N> verts;

    [[nodiscard]] Scalar sdf(Vec p) const noexcept {
        const std::size_t n = verts.size();
        if (n == 0) return Scalar(1e18);
        Scalar d = (p - verts[0]).len2();
        Scalar s = 1; // sign: +1 outside, -1 inside
        for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
            const Vec e = verts[j] - verts[i];
            const Vec w = p - verts[i];
            const Scalar t = std::clamp(w.dot(e) / std::max(e.len2(), Scalar(1e-12)), Scalar(0), Scalar(1));
            const Vec proj = w - e * t;
            d = std::min(d, proj.len2());
            // winding sign test (point-in-convex via edge crossings)
            const bool c1 = p.y >= verts[i].y, c2 = p.y < verts[j].y;
            const bool c3 = cross(e, w) > 0;
            if ((c1 && c2 && c3) || (!c1 && !c2 && !c3)) s = -s;
        }
        return s * std::sqrt(d);
    }
    [[nodiscard]] Box2 aabb() const noexcept {
        if (verts.empty()) return Box2{};
        Box2 b{verts[0], verts[0]};
        for (std::size_t i = 1; i < verts.size(); ++i) b.expand(verts[i]);
        return b;
    }
    [[nodiscard]] Vec support(Vec d) const noexcept {
        if (verts.empty()) return Vec{};
        std::size_t best = 0;
        Scalar bestDot = d.dot(verts[0]);
        for (std::size_t i = 1; i < verts.size(); ++i) {
            const Scalar dp = d.dot(verts[i]);
            if (dp > bestDot) { bestDot = dp; best = i; }
        }
        return verts[best];
    }
};

// ── Rounded Convex Polygon ────────────────────────────────────────────────────────
template <std::size_t N = 8>
struct RoundedPoly {
    ConvexPoly<N> base{};
    Scalar        radius{0.1f};

    [[nodiscard]] Scalar sdf(Vec p) const noexcept { return base.sdf(p) - radius; }
    [[nodiscard]] Box2 aabb() const noexcept { return base.aabb().fattened(radius); }
    [[nodiscard]] Vec support(Vec d) const noexcept {
        return base.support(d) + d.normalized() * radius;
    }
};

static_assert(Shape<Circle>);
static_assert(Shape<Segment>);
static_assert(Shape<Capsule>);
static_assert(Shape<Box>);
static_assert(Shape<OrientedBox>);
static_assert(Shape<Triangle>);
static_assert(Shape<RoundedBox>);
static_assert(Shape<Sector>);
static_assert(Shape<HalfPlane>);
static_assert(Shape<ConvexPoly<8>>);
static_assert(Shape<RoundedPoly<8>>);

} // namespace akruti
