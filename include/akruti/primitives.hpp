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
    // ── Circle ────────────────────────────────────────────────────────────────────
    struct Circle {
        Vec center{};
        Scalar radius{1};

        [[nodiscard]] Scalar sdf(Vec p) const noexcept { return akruti::distance(p, center) - radius; }

        [[nodiscard]] constexpr Box2 aabb() const noexcept {
            return {{center[0] - radius, center[1] - radius}, {center[0] + radius, center[1] + radius}};
        }

        [[nodiscard]] Vec support(Vec d) const noexcept { return center + akruti::normalize(d) * radius; }
        [[nodiscard]] constexpr Vec centroid() const noexcept { return center; }
    };

    // ── Segment (thick=0 line) ──────────────────────────────────────────────────────
    struct Segment {
        Vec a{}, b{};

        [[nodiscard]] Scalar sdf(Vec p) const noexcept {
            const Vec ab = b - a, ap = p - a;
            const Scalar ab_len_sq = akruti::length_sq(ab);
            const Scalar t = std::clamp(akruti::dot(ap, ab) / std::max(ab_len_sq, Scalar(1e-12)), Scalar(0), Scalar(1));
            return akruti::length(ap - ab * t);
        }

        [[nodiscard]] constexpr Box2 aabb() const noexcept {
            return {{std::min(a[0], b[0]), std::min(a[1], b[1])}, {std::max(a[0], b[0]), std::max(a[1], b[1])}};
        }

        [[nodiscard]] constexpr Vec support(Vec d) const noexcept { return akruti::dot(d, a) >= akruti::dot(d, b) ? a : b; }
        [[nodiscard]] constexpr Vec centroid() const noexcept { return (a + b) * Scalar(0.5); }
    };

    // ── Capsule (segment inflated by radius) ─────────────────────────────────────────
    struct Capsule {
        Vec a{}, b{};
        Scalar radius{1};

        [[nodiscard]] Scalar sdf(Vec p) const noexcept { return Segment{a, b}.sdf(p) - radius; }

        [[nodiscard]] constexpr Box2 aabb() const noexcept {
            return {
                {std::min(a[0], b[0]) - radius, std::min(a[1], b[1]) - radius},
                {std::max(a[0], b[0]) + radius, std::max(a[1], b[1]) + radius}
            };
        }

        [[nodiscard]] Vec support(Vec d) const noexcept {
            const Vec base = akruti::dot(d, a) >= akruti::dot(d, b) ? a : b;
            return base + akruti::normalize(d) * radius;
        }

        [[nodiscard]] constexpr Vec centroid() const noexcept { return (a + b) * Scalar(0.5); }
    };

    // ── Axis-aligned box (center + half-extents) ──────────────────────────────────────
    struct Box {
        Vec center{};
        Vec half{1, 1};

        [[nodiscard]] Scalar sdf(Vec p) const noexcept {
            const Vec q{std::fabs(p[0] - center[0]) - half[0], std::fabs(p[1] - center[1]) - half[1]};
            const Vec qp{std::max(q[0], Scalar(0)), std::max(q[1], Scalar(0))};
            return akruti::length(qp) + std::min(std::max(q[0], q[1]), Scalar(0));
        }

        [[nodiscard]] constexpr Box2 aabb() const noexcept {
            return {{center[0] - half[0], center[1] - half[1]}, {center[0] + half[0], center[1] + half[1]}};
        }

        [[nodiscard]] constexpr Vec support(Vec d) const noexcept {
            return {center[0] + (d[0] >= 0 ? half[0] : -half[0]), center[1] + (d[1] >= 0 ? half[1] : -half[1])};
        }

        [[nodiscard]] constexpr Vec centroid() const noexcept { return center; }
    };

    // ── Oriented Bounding Box (OBB: center + half + rotation) ─────────────────────────
    struct OrientedBox {
        Vec center{};
        Vec half{1, 1};
        Mat2<Scalar> rot{1, 0, 0, 1}; // forward rotation matrix (local to world)

        [[nodiscard]] static OrientedBox from_angle(Vec center, Vec half, Scalar radians) noexcept {
            return {center, half, akruti::make_rotation2d<Scalar>(radians)};
        }

        [[nodiscard]] Scalar sdf(Vec p) const noexcept {
            // Map p to local unrotated space: p_local = rot^T * (p - center)
            const Vec diff = p - center;
            const Vec local{
                rot(0, 0) * diff[0] + rot(1, 0) * diff[1],
                rot(0, 1) * diff[0] + diff[1] * rot(1, 1)
            };
            const Vec q{std::fabs(local[0]) - half[0], std::fabs(local[1]) - half[1]};
            const Vec qp{std::max(q[0], Scalar(0)), std::max(q[1], Scalar(0))};
            return akruti::length(qp) + std::min(std::max(q[0], q[1]), Scalar(0));
        }

        [[nodiscard]] Box2 aabb() const noexcept {
            // Extents along world axes: e_x = |rot00|*hx + |rot01|*hy
            const Scalar ex = std::fabs(rot(0, 0)) * half[0] + std::fabs(rot(0, 1)) * half[1];
            const Scalar ey = std::fabs(rot(1, 0)) * half[0] + std::fabs(rot(1, 1)) * half[1];
            return {{center[0] - ex, center[1] - ey}, {center[0] + ex, center[1] + ey}};
        }

        [[nodiscard]] Vec support(Vec d) const noexcept {
            // Transform direction to local, find support on unrotated box, rotate back
            const Vec d_local{
                rot(0, 0) * d[0] + rot(1, 0) * d[1],
                rot(0, 1) * d[0] + d[1] * rot(1, 1)
            };
            const Vec sup_local{
                d_local[0] >= 0 ? half[0] : -half[0],
                d_local[1] >= 0 ? half[1] : -half[1]
            };
            return center + rot * sup_local;
        }

        [[nodiscard]] constexpr Vec centroid() const noexcept { return center; }
    };

    // ── Triangle: exact 2D barycentric distance ───────────────────────────────────────
    struct Triangle {
        Vec a{}, b{}, c{};

        [[nodiscard]] Scalar sdf(Vec p) const noexcept {
            const Vec e0 = b - a, e1 = c - b, e2 = a - c;
            const Vec v0 = p - a, v1 = p - b, v2 = p - c;

            const Vec pq0 = v0 - e0 * std::clamp(akruti::dot(v0, e0) / std::max(akruti::length_sq(e0), Scalar(1e-12)), Scalar(0), Scalar(1));
            const Vec pq1 = v1 - e1 * std::clamp(akruti::dot(v1, e1) / std::max(akruti::length_sq(e1), Scalar(1e-12)), Scalar(0), Scalar(1));
            const Vec pq2 = v2 - e2 * std::clamp(akruti::dot(v2, e2) / std::max(akruti::length_sq(e2), Scalar(1e-12)), Scalar(0), Scalar(1));

            const Scalar s = akruti::cross(e0, -e2); // Area / orientation: positive if a->b->c is CCW
            const Scalar d = std::min({akruti::length_sq(pq0), akruti::length_sq(pq1), akruti::length_sq(pq2)});

            // Inside if all on same side of edges
            const Scalar z0 = akruti::cross(e0, v0) * s;
            const Scalar z1 = akruti::cross(e1, v1) * s;
            const Scalar z2 = akruti::cross(e2, v2) * s;
            const bool inside = (z0 >= 0 && z1 >= 0 && z2 >= 0);

            return (inside ? -1.0f : 1.0f) * std::sqrt(d);
        }

        [[nodiscard]] constexpr Box2 aabb() const noexcept {
            return {
                {std::min({a[0], b[0], c[0]}), std::min({a[1], b[1], c[1]})},
                {std::max({a[0], b[0], c[0]}), std::max({a[1], b[1], c[1]})}
            };
        }

        [[nodiscard]] constexpr Vec support(Vec d) const noexcept {
            const Scalar da = akruti::dot(d, a), db = akruti::dot(d, b), dc = akruti::dot(d, c);
            if (da >= db && da >= dc) return a;
            if (db >= da && db >= dc) return b;
            return c;
        }

        [[nodiscard]] constexpr Vec centroid() const noexcept { return (a + b + c) * (Scalar(1) / Scalar(3)); }
    };

    // ── Rounded Box (Box with corner radius) ──────────────────────────────────────────
    struct RoundedBox {
        Vec center{};
        Vec half{1, 1};
        Scalar radius{0.1f};

        [[nodiscard]] Scalar sdf(Vec p) const noexcept {
            const Vec q{
                std::fabs(p[0] - center[0]) - half[0] + radius,
                std::fabs(p[1] - center[1]) - half[1] + radius
            };
            const Vec qp{std::max(q[0], Scalar(0)), std::max(q[1], Scalar(0))};
            return akruti::length(qp) + std::min(std::max(q[0], q[1]), Scalar(0)) - radius;
        }

        [[nodiscard]] constexpr Box2 aabb() const noexcept {
            return {{center[0] - half[0], center[1] - half[1]}, {center[0] + half[0], center[1] + half[1]}};
        }

        [[nodiscard]] Vec support(Vec d) const noexcept {
            const Vec inner_half{std::max(Scalar(0), half[0] - radius), std::max(Scalar(0), half[1] - radius)};
            const Vec b_sup{
                center[0] + (d[0] >= 0 ? inner_half[0] : -inner_half[0]),
                center[1] + (d[1] >= 0 ? inner_half[1] : -inner_half[1])
            };
            return b_sup + akruti::normalize(d) * radius;
        }

        [[nodiscard]] constexpr Vec centroid() const noexcept { return center; }
    };

    // ── Arc / Sector (FOV cone / radar sensor primitive) ──────────────────────────────
    struct Sector {
        Vec center{};
        Scalar radius{1};
        Scalar half_angle{0.785398f}; // half opening angle in radians (~45 deg)
        Mat2<Scalar> rot{1, 0, 0, 1}; // forward orientation matrix

        [[nodiscard]] static Sector from_direction(Vec center, Scalar r, Scalar half_ang, Vec dir) noexcept {
            const Vec nd = akruti::normalize(dir);
            // Rotation aligning +X with nd
            return {center, r, half_ang, Mat2<Scalar>{nd[0], -nd[1], nd[1], nd[0]}};
        }

        [[nodiscard]] Scalar sdf(Vec p) const noexcept {
            const Vec diff = p - center;
            // Map to local unrotated space where cone axis is +X
            const Vec q{
                rot(0, 0) * diff[0] + rot(1, 0) * diff[1],
                rot(0, 1) * diff[0] + diff[1] * rot(1, 1)
            };
            const Scalar len = akruti::length(q);
            const Scalar sin_a = std::sin(half_angle), cos_a = std::cos(half_angle);
            const Vec cs{cos_a, sin_a};

            const Vec p_rot{std::fabs(q[1]), q[0]};
            const Scalar z = p_rot[0] * cs[0] - p_rot[1] * cs[1];
            if (z <= 0) {
                return len - radius;
            }
            const Vec edge = cs * std::clamp(akruti::dot(q, Vec{cs[0], cs[1]}), Scalar(0), radius);
            return akruti::length(q - Vec{edge[0], std::copysign(edge[1], q[1])});
        }

        [[nodiscard]] Box2 aabb() const noexcept {
            // Conservative circle bound
            return {{center[0] - radius, center[1] - radius}, {center[0] + radius, center[1] + radius}};
        }

        [[nodiscard]] Vec support(Vec d) const noexcept {
            // Transform direction d into local coordinate system of the sector
            const Vec local_d{
                rot(0, 0) * d.x() + rot(1, 0) * d.y(),
                rot(0, 1) * d.x() + d.y() * rot(1, 1)
            };

            // Candidate 1: Apex (center)
            Vec best_p{0, 0};
            Scalar best_dot = 0.0f; // Apex dot local_d is 0

            // Candidate 2: Arc extremes at -half_angle and +half_angle
            const Scalar cos_h = std::cos(half_angle);
            const Scalar sin_h = std::sin(half_angle);
            const Vec p_left{radius * cos_h, radius * -sin_h};
            const Vec p_right{radius * cos_h, radius * sin_h};

            const Scalar dot_l = akruti::dot(p_left, local_d);
            const Scalar dot_r = akruti::dot(p_right, local_d);
            if (dot_l > best_dot) {
                best_dot = dot_l;
                best_p = p_left;
            }
            if (dot_r > best_dot) {
                best_dot = dot_r;
                best_p = p_right;
            }

            // Candidate 3: Continuous arc boundary if local_d falls within [-half_angle, half_angle]
            if (local_d.x() > 0.0f) {
                const Scalar ang = std::atan2(local_d.y(), local_d.x());
                if (std::fabs(ang) <= half_angle) {
                    const Vec p_arc = akruti::normalize(local_d) * radius;
                    const Scalar dot_arc = akruti::dot(p_arc, local_d);
                    if (dot_arc > best_dot) {
                        best_dot = dot_arc;
                        best_p = p_arc;
                    }
                }
            }

            // Transform best point back to world coordinates
            return center + Vec{
                rot(0, 0) * best_p.x() + rot(0, 1) * best_p.y(),
                rot(1, 0) * best_p.x() + rot(1, 1) * best_p.y()
            };
        }

        [[nodiscard]] constexpr Vec centroid() const noexcept { return center; }
    };

    // ── Half-plane: signed distance to a line through `point` with unit outward `normal`.
    struct HalfPlane {
        Vec normal{0, 1}; // points OUT of the solid (solid is the negative-distance side)
        Vec point{};

        [[nodiscard]] Scalar sdf(Vec p) const noexcept { return akruti::dot(akruti::normalize(normal), p - point); }

        [[nodiscard]] constexpr Box2 aabb() const noexcept {
            constexpr Scalar big = Scalar(1e18);
            return {{-big, -big}, {big, big}};
        }

        [[nodiscard]] Vec support(Vec d) const noexcept {
            // Unbounded; project a far point against the ray direction.
            return point - akruti::normalize(normal) * (akruti::dot(d, normal) > 0 ? Scalar(0) : Scalar(1e18));
        }

        [[nodiscard]] constexpr Vec centroid() const noexcept { return point; }
    };

    // ── Convex polygon (CCW winding). Verts in a fixed-capacity buffer (no heap). ─────
    template <std::size_t N = 8>
    struct ConvexPoly {
        containers::static_vector<Vec, N> verts;

        [[nodiscard]] Scalar sdf(Vec p) const noexcept {
            const std::size_t n = verts.size();
            if (n == 0) return Scalar(1e18);
            Scalar d = akruti::length_sq(p - verts[0]);
            Scalar s = 1; // sign: +1 outside, -1 inside
            for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
                const Vec e = verts[j] - verts[i];
                const Vec w = p - verts[i];
                const Scalar t = std::clamp(akruti::dot(w, e) / std::max(akruti::length_sq(e), Scalar(1e-12)), Scalar(0), Scalar(1));
                const Vec proj = w - e * t;
                d = std::min(d, akruti::length_sq(proj));
                // winding sign test (point-in-convex via edge crossings)
                const bool c1 = p.y() >= verts[i].y(), c2 = p.y() < verts[j].y();
                const bool c3 = akruti::cross(e, w) > 0;
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
            Scalar bestDot = akruti::dot(d, verts[0]);
            for (std::size_t i = 1; i < verts.size(); ++i) {
                const Scalar dp = akruti::dot(d, verts[i]);
                if (dp > bestDot) {
                    bestDot = dp;
                    best = i;
                }
            }
            return verts[best];
        }

        [[nodiscard]] Vec centroid() const noexcept {
            // Shoelace centroid for CCW polygon
            if (verts.empty()) return Vec{};
            if (verts.size() == 1) return verts[0];
            Scalar area = 0, cx = 0, cy = 0;
            const std::size_t n = verts.size();
            for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
                const Scalar cross_val = verts[j].x() * verts[i].y() - verts[i].x() * verts[j].y();
                area += cross_val;
                cx += (verts[j].x() + verts[i].x()) * cross_val;
                cy += (verts[j].y() + verts[i].y()) * cross_val;
            }
            area *= Scalar(0.5);
            if (std::fabs(area) < Scalar(1e-12)) {
                Vec sum{};
                for (const auto& v : verts) sum = sum + v;
                return sum * (Scalar(1) / Scalar(n));
            }
            const Scalar inv = Scalar(1) / (Scalar(6) * area);
            return {cx * inv, cy * inv};
        }

        [[nodiscard]] static ConvexPoly<4> from_aabb(Box2 box) noexcept {
            ConvexPoly<4> p;
            (void)p.verts.push_back({box.lo.x(), box.lo.y()});
            (void)p.verts.push_back({box.hi.x(), box.lo.y()});
            (void)p.verts.push_back({box.hi.x(), box.hi.y()});
            (void)p.verts.push_back({box.lo.x(), box.hi.y()});
            return p;
        }
    };

    // ── Rounded Convex Polygon ────────────────────────────────────────────────────────
    template <std::size_t N = 8>
    struct RoundedPoly {
        ConvexPoly<N> base{};
        Scalar radius{0.1f};

        [[nodiscard]] Scalar sdf(Vec p) const noexcept { return base.sdf(p) - radius; }
        [[nodiscard]] Box2 aabb() const noexcept { return base.aabb().fattened(radius); }

        [[nodiscard]] Vec support(Vec d) const noexcept {
            return base.support(d) + akruti::normalize(d) * radius;
        }

        [[nodiscard]] Vec centroid() const noexcept { return base.centroid(); }
    };

    // ── Chain / Polyline (with ghost vertices to prevent edge snagging) ───────────────
    template <std::size_t N = 16>
    struct ChainShape {
        containers::static_vector<Vec, N> verts{};
        Vec prev_ghost{}; // Ghost vertex before verts[0] (for smooth boundary normal)
        Vec next_ghost{}; // Ghost vertex after verts.back()
        bool has_prev_ghost{false};
        bool has_next_ghost{false};
        bool is_loop{false}; // If true, verts.back() connects to verts[0]
        Scalar radius{0.0f}; // Optional stroke radius

        [[nodiscard]] Scalar sdf(Vec p) const noexcept {
            const std::size_t n = verts.size();
            if (n < 2) return Scalar(1e18);

            Scalar min_d2 = Scalar(1e18);
            const std::size_t seg_count = is_loop ? n : (n - 1);

            for (std::size_t i = 0; i < seg_count; ++i) {
                const Vec v0 = verts[i];
                const Vec v1 = verts[(i + 1) % n];
                const Vec e = v1 - v0;
                const Vec w = p - v0;
                const Scalar t = std::clamp(akruti::dot(w, e) / std::max(akruti::length_sq(e), Scalar(1e-12)), Scalar(0), Scalar(1));
                const Vec proj = w - e * t;
                min_d2 = std::min(min_d2, akruti::length_sq(proj));
            }
            return std::sqrt(min_d2) - radius;
        }

        [[nodiscard]] Box2 aabb() const noexcept {
            if (verts.empty()) return Box2{};
            Box2 b{verts[0], verts[0]};
            for (std::size_t i = 1; i < verts.size(); ++i) b.expand(verts[i]);
            return radius > 0 ? b.fattened(radius) : b;
        }

        [[nodiscard]] Vec support(Vec d) const noexcept {
            if (verts.empty()) return Vec{};
            std::size_t best = 0;
            Scalar bestDot = akruti::dot(d, verts[0]);
            for (std::size_t i = 1; i < verts.size(); ++i) {
                const Scalar dp = akruti::dot(d, verts[i]);
                if (dp > bestDot) {
                    bestDot = dp;
                    best = i;
                }
            }
            const Vec sup = verts[best];
            return (radius > 0) ? (sup + akruti::normalize(d) * radius) : sup;
        }

        // Ghost-vertex corrected edge normal for edge index `i` (from verts[i] to verts[(i+1)%n])
        [[nodiscard]] Vec edge_normal(std::size_t i) const noexcept {
            const std::size_t n = verts.size();
            if (n < 2 || i >= (is_loop ? n : (n - 1))) return Vec{0, 1};
            const Vec v0 = verts[i];
            const Vec v1 = verts[(i + 1) % n];
            const Vec e = v1 - v0;
            return akruti::normalize(akruti::perp(e)); // Outward CCW 90-degree normal
        }

        [[nodiscard]] Vec centroid() const noexcept {
            if (verts.empty()) return Vec{};
            Vec sum{};
            for (const auto& v : verts) sum = sum + v;
            return sum * (Scalar(1) / Scalar(verts.size()));
        }
    };

    // ── GridSDF / Discrete 2D Texture Signed Distance Field ──────────────────────────
    template <std::size_t Width = 16, std::size_t Height = 16>
    struct GridSDF {
        Box2 bounds{}; // World-space domain bounding box
        std::array<Scalar, Width * Height> grid{}; // Row-major discrete distance values

        [[nodiscard]] Scalar sample(std::size_t x, std::size_t y) const noexcept {
            return grid[y * Width + x];
        }

        [[nodiscard]] Scalar sdf(Vec p) const noexcept {
            const Vec extent = bounds.extent();
            const Vec center = bounds.center();
            if (extent.x() <= 1e-6f || extent.y() <= 1e-6f || Width < 2 || Height < 2) {
                return akruti::distance(p, center);
            }

            // Normalized [0, 1] UV
            const Scalar u = (p.x() - bounds.lo.x()) / extent.x();
            const Scalar v = (p.y() - bounds.lo.y()) / extent.y();

            // Grid coordinates
            const Scalar gx = u * static_cast<Scalar>(Width - 1);
            const Scalar gy = v * static_cast<Scalar>(Height - 1);

            const auto x0 = static_cast<std::size_t>(std::clamp(std::floor(gx), 0.0f, static_cast<Scalar>(Width - 2)));
            const auto y0 = static_cast<std::size_t>(std::clamp(std::floor(gy), 0.0f, static_cast<Scalar>(Height - 2)));
            const std::size_t x1 = x0 + 1;
            const std::size_t y1 = y0 + 1;

            const Scalar tx = std::clamp(gx - static_cast<Scalar>(x0), 0.0f, 1.0f);
            const Scalar ty = std::clamp(gy - static_cast<Scalar>(y0), 0.0f, 1.0f);

            // Bilinear interpolation
            const Scalar d00 = sample(x0, y0);
            const Scalar d10 = sample(x1, y0);
            const Scalar d01 = sample(x0, y1);
            const Scalar d11 = sample(x1, y1);

            const Scalar d0 = d00 * (1.0f - tx) + d10 * tx;
            const Scalar d1 = d01 * (1.0f - tx) + d11 * tx;
            const Scalar val = d0 * (1.0f - ty) + d1 * ty;

            // Add distance to bounding box if point is outside domain
            const Box box{center, extent * 0.5f};
            const Scalar box_dist = box.sdf(p);
            return (box_dist > 0.0f) ? (box_dist + std::max(0.0f, val)) : val;
        }

        [[nodiscard]] Box2 aabb() const noexcept { return bounds; }

        [[nodiscard]] Vec support(Vec d) const noexcept {
            const Box box{bounds.center(), bounds.extent() * 0.5f};
            return box.support(d);
        }

        [[nodiscard]] Vec centroid() const noexcept { return bounds.center(); }
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
    static_assert(Shape<ChainShape<8>>);
    static_assert(Shape<GridSDF<4, 4>>);
} // namespace akruti
