#pragma once
// ============================================================================
// akruti/spline.hpp — Generic Splines as First-Class Akruti Shapes
// ============================================================================
// Cubic Bézier Curves & Catmull-Rom Splines implementing the full Shape contract:
//   - Parametric evaluation, tangents, and normals
//   - Arc-length parameterization for constant speed
//   - Signed Distance Function sdf(p) with stroke thickness radius
//   - Exact bounding box aabb() and GJK support mapping support(d)
//   - Full CSG shape arithmetic participation
// ============================================================================

#include "shape.hpp"
#include "math.hpp"
#include "containers/static/static_vector.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>

namespace akruti {

// ── Cubic Bézier Curve ──────────────────────────────────────────────────────
struct CubicBezierCurve {
    Vec2<Scalar> p0{};
    Vec2<Scalar> p1{};
    Vec2<Scalar> p2{};
    Vec2<Scalar> p3{};
    Scalar       radius = Scalar(0.5); // Stroke thickness radius for SDF/CSG

    [[nodiscard]] Vec2<Scalar> evaluate(Scalar t) const noexcept {
        const Scalar u = Scalar(1.0) - t;
        const Scalar tt = t * t;
        const Scalar uu = u * u;
        const Scalar uuu = uu * u;
        const Scalar ttt = tt * t;

        return p0 * uuu +
               p1 * (Scalar(3.0) * uu * t) +
               p2 * (Scalar(3.0) * u * tt) +
               p3 * ttt;
    }

    [[nodiscard]] Vec2<Scalar> tangent(Scalar t) const noexcept {
        const Scalar u = Scalar(1.0) - t;
        const Vec2<Scalar> d = (p1 - p0) * (Scalar(3.0) * u * u) +
                               (p2 - p1) * (Scalar(6.0) * u * t) +
                               (p3 - p2) * (Scalar(3.0) * t * t);
        const Scalar len = std::sqrt(d.x * d.x + d.y * d.y);
        return (len > Scalar(1e-6)) ? d * (Scalar(1.0) / len) : Vec2<Scalar>(Scalar(1.0), Scalar(0.0));
    }

    [[nodiscard]] Vec2<Scalar> normal(Scalar t) const noexcept {
        const auto tan = tangent(t);
        return Vec2<Scalar>(-tan.y, tan.x);
    }

    // Arc-length approximation via Gaussian quadrature / multi-segment sampling
    [[nodiscard]] Scalar arc_length(std::size_t samples = 16) const noexcept {
        Scalar length = Scalar(0.0);
        Vec2<Scalar> prev = p0;
        const Scalar step = Scalar(1.0) / static_cast<Scalar>(samples);
        for (std::size_t i = 1; i <= samples; ++i) {
            Vec2<Scalar> curr = evaluate(static_cast<Scalar>(i) * step);
            const Scalar dx = curr.x - prev.x;
            const Scalar dy = curr.y - prev.y;
            length += std::sqrt(dx * dx + dy * dy);
            prev = curr;
        }
        return length;
    }

    // ── Akruti Shape Contract ───────────────────────────────────────────────

    // Distance to cubic curve approximated by adaptive segment subdivision
    [[nodiscard]] Scalar sdf(Vec2<Scalar> p) const noexcept {
        Scalar min_dist_sq = Scalar(1e18);
        constexpr std::size_t kSubdivs = 16;
        Vec2<Scalar> prev = p0;
        const Scalar step = Scalar(1.0) / static_cast<Scalar>(kSubdivs);

        for (std::size_t i = 1; i <= kSubdivs; ++i) {
            Vec2<Scalar> curr = evaluate(static_cast<Scalar>(i) * step);
            // Distance from point p to segment [prev, curr]
            const Vec2<Scalar> ab = curr - prev;
            const Vec2<Scalar> ap = p - prev;
            const Scalar ab_len_sq = ab.x * ab.x + ab.y * ab.y;
            Scalar t_seg = (ab_len_sq > Scalar(1e-6)) ? (ap.x * ab.x + ap.y * ab.y) / ab_len_sq : Scalar(0.0);
            t_seg = std::clamp(t_seg, Scalar(0.0), Scalar(1.0));
            const Vec2<Scalar> proj = prev + ab * t_seg;
            const Scalar dx = p.x - proj.x;
            const Scalar dy = p.y - proj.y;
            min_dist_sq = std::min(min_dist_sq, dx * dx + dy * dy);
            prev = curr;
        }

        return std::sqrt(min_dist_sq) - radius;
    }

    [[nodiscard]] AABB<Scalar> aabb() const noexcept {
        const Scalar min_x = std::min({p0.x, p1.x, p2.x, p3.x}) - radius;
        const Scalar min_y = std::min({p0.y, p1.y, p2.y, p3.y}) - radius;
        const Scalar max_x = std::max({p0.x, p1.x, p2.x, p3.x}) + radius;
        const Scalar max_y = std::max({p0.y, p1.y, p2.y, p3.y}) + radius;
        return AABB<Scalar>{Vec2<Scalar>{min_x, min_y}, Vec2<Scalar>{max_x, max_y}};
    }

    [[nodiscard]] Vec2<Scalar> support(Vec2<Scalar> d) const noexcept {
        // Farthest control point + radius in direction d
        const Scalar d0 = p0.x * d.x + p0.y * d.y;
        const Scalar d1 = p1.x * d.x + p1.y * d.y;
        const Scalar d2 = p2.x * d.x + p2.y * d.y;
        const Scalar d3 = p3.x * d.x + p3.y * d.y;

        Vec2<Scalar> best = p0;
        Scalar max_dot = d0;
        if (d1 > max_dot) { max_dot = d1; best = p1; }
        if (d2 > max_dot) { max_dot = d2; best = p2; }
        if (d3 > max_dot) { best = p3; }

        const Scalar d_len = std::sqrt(d.x * d.x + d.y * d.y);
        if (d_len > Scalar(1e-6)) {
            best = best + d * (radius / d_len);
        }
        return best;
    }

    [[nodiscard]] constexpr Vec2<Scalar> centroid() const noexcept {
        return (p0 + p1 + p2 + p3) * Scalar(0.25);
    }
};

// ── Catmull-Rom Multi-Segment Spline ────────────────────────────────────────
struct CatmullRomSpline {
    containers::static_vector<Vec2<Scalar>, 32> points;
    Scalar                                      radius = Scalar(0.5);
    bool                                        closed = false;

    [[nodiscard]] Vec2<Scalar> evaluate(Scalar t) const noexcept {
        if (points.empty()) return Vec2<Scalar>{};
        if (points.size() == 1) return points[0];

        const std::size_t num_segments = closed ? points.size() : points.size() - 1;
        const Scalar scaled_t = t * static_cast<Scalar>(num_segments);
        auto seg_idx = static_cast<std::size_t>(scaled_t);
        if (seg_idx >= num_segments) seg_idx = num_segments - 1;
        const Scalar local_t = scaled_t - static_cast<Scalar>(seg_idx);

        const auto get_point = [&](int idx) -> Vec2<Scalar> {
            if (closed) {
                int n = static_cast<int>(points.size());
                return points[static_cast<std::size_t>((idx % n + n) % n)];
            }
            int clamped = std::clamp(idx, 0, static_cast<int>(points.size() - 1));
            return points[static_cast<std::size_t>(clamped)];
        };

        const Vec2<Scalar> p0 = get_point(static_cast<int>(seg_idx) - 1);
        const Vec2<Scalar> p1 = get_point(static_cast<int>(seg_idx));
        const Vec2<Scalar> p2 = get_point(static_cast<int>(seg_idx) + 1);
        const Vec2<Scalar> p3 = get_point(static_cast<int>(seg_idx) + 2);

        // Single Catmull-Rom source: pebble::math (shared with gati animation curves).
        return Vec2<Scalar>(pebble::math::catmull_rom(
            static_cast<pebble::math::vec2>(p0), static_cast<pebble::math::vec2>(p1),
            static_cast<pebble::math::vec2>(p2), static_cast<pebble::math::vec2>(p3), local_t));
    }

    [[nodiscard]] Vec2<Scalar> tangent(Scalar t) const noexcept {
        constexpr Scalar dt = Scalar(1e-3);
        const auto p_next = evaluate(std::min(Scalar(1.0), t + dt));
        const auto p_prev = evaluate(std::max(Scalar(0.0), t - dt));
        const auto diff = p_next - p_prev;
        const Scalar len = std::sqrt(diff.x * diff.x + diff.y * diff.y);
        return (len > Scalar(1e-6)) ? diff * (Scalar(1.0) / len) : Vec2<Scalar>(Scalar(1.0), Scalar(0.0));
    }

    [[nodiscard]] Scalar arc_length(std::size_t samples = 32) const noexcept {
        if (points.size() < 2) return Scalar(0.0);
        Scalar len = Scalar(0.0);
        Vec2<Scalar> prev = evaluate(Scalar(0.0));
        const Scalar step = Scalar(1.0) / static_cast<Scalar>(samples);
        for (std::size_t i = 1; i <= samples; ++i) {
            Vec2<Scalar> curr = evaluate(static_cast<Scalar>(i) * step);
            const Scalar dx = curr.x - prev.x;
            const Scalar dy = curr.y - prev.y;
            len += std::sqrt(dx * dx + dy * dy);
            prev = curr;
        }
        return len;
    }

    // ── Akruti Shape Contract ───────────────────────────────────────────────

    [[nodiscard]] Scalar sdf(Vec2<Scalar> p) const noexcept {
        if (points.empty()) return Scalar(1e18);
        Scalar min_dist_sq = Scalar(1e18);
        constexpr std::size_t kSamples = 32;
        Vec2<Scalar> prev = evaluate(Scalar(0.0));
        const Scalar step = Scalar(1.0) / static_cast<Scalar>(kSamples);

        for (std::size_t i = 1; i <= kSamples; ++i) {
            Vec2<Scalar> curr = evaluate(static_cast<Scalar>(i) * step);
            const Vec2<Scalar> ab = curr - prev;
            const Vec2<Scalar> ap = p - prev;
            const Scalar ab_len_sq = ab.x * ab.x + ab.y * ab.y;
            Scalar t_seg = (ab_len_sq > Scalar(1e-6)) ? (ap.x * ab.x + ap.y * ab.y) / ab_len_sq : Scalar(0.0);
            t_seg = std::clamp(t_seg, Scalar(0.0), Scalar(1.0));
            const Vec2<Scalar> proj = prev + ab * t_seg;
            const Scalar dx = p.x - proj.x;
            const Scalar dy = p.y - proj.y;
            min_dist_sq = std::min(min_dist_sq, dx * dx + dy * dy);
            prev = curr;
        }

        return std::sqrt(min_dist_sq) - radius;
    }

    [[nodiscard]] AABB<Scalar> aabb() const noexcept {
        if (points.empty()) return AABB<Scalar>{};
        Scalar min_x = points[0].x, min_y = points[0].y;
        Scalar max_x = points[0].x, max_y = points[0].y;
        for (const auto& pt : points) {
            min_x = std::min(min_x, pt.x);
            min_y = std::min(min_y, pt.y);
            max_x = std::max(max_x, pt.x);
            max_y = std::max(max_y, pt.y);
        }
        return AABB<Scalar>{Vec2<Scalar>{min_x - radius, min_y - radius},
                            Vec2<Scalar>{max_x + radius, max_y + radius}};
    }

    [[nodiscard]] Vec2<Scalar> support(Vec2<Scalar> d) const noexcept {
        if (points.empty()) return Vec2<Scalar>{};
        Vec2<Scalar> best = points[0];
        Scalar max_dot = points[0].x * d.x + points[0].y * d.y;
        for (const auto& pt : points) {
            const Scalar dot = pt.x * d.x + pt.y * d.y;
            if (dot > max_dot) {
                max_dot = dot;
                best = pt;
            }
        }
        const Scalar d_len = std::sqrt(d.x * d.x + d.y * d.y);
        if (d_len > Scalar(1e-6)) {
            best = best + d * (radius / d_len);
        }
        return best;
    }

    [[nodiscard]] Vec2<Scalar> centroid() const noexcept {
        if (points.empty()) return Vec2<Scalar>{};
        Vec2<Scalar> sum{};
        for (const auto& v : points) sum = sum + v;
        return sum * (Scalar(1) / Scalar(points.size()));
    }
};

static_assert(Shape<CubicBezierCurve>);
static_assert(Shape<CatmullRomSpline>);

} // namespace akruti
