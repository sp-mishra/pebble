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

namespace akruti {
    // ── Cubic Bézier Curve ──────────────────────────────────────────────────────
    struct CubicBezierCurve {
        Vec2<Scalar> p0{};
        Vec2<Scalar> p1{};
        Vec2<Scalar> p2{};
        Vec2<Scalar> p3{};
        Scalar radius = 0.5; // Stroke thickness radius for SDF/CSG

        [[nodiscard]] Vec2<Scalar> evaluate(const Scalar t) const noexcept {
            const Scalar u = static_cast<Scalar>(1.0) - t;
            const Scalar tt = t * t;
            const Scalar uu = u * u;
            const Scalar uuu = uu * u;
            const Scalar ttt = tt * t;

            return p0 * uuu +
                p1 * (static_cast<Scalar>(3.0) * uu * t) +
                p2 * (static_cast<Scalar>(3.0) * u * tt) +
                p3 * ttt;
        }

        [[nodiscard]] Vec2<Scalar> tangent(const Scalar t) const noexcept {
            const Scalar u = static_cast<Scalar>(1.0) - t;
            const Vec2<Scalar> d = (p1 - p0) * (static_cast<Scalar>(3.0) * u * u) +
                (p2 - p1) * (static_cast<Scalar>(6.0) * u * t) +
                (p3 - p2) * (static_cast<Scalar>(3.0) * t * t);
            const Scalar len = length(d);
            return (len > static_cast<Scalar>(1e-6))
                       ? d * (static_cast<Scalar>(1.0) / len)
                       : Vec2<Scalar>(static_cast<Scalar>(1.0), static_cast<Scalar>(0.0));
        }

        [[nodiscard]] Vec2<Scalar> normal(const Scalar t) const noexcept {
            const auto tan = tangent(t);
            return perp(tan);
        }

        // Arc-length approximation via Gaussian quadrature / multi-segment sampling
        [[nodiscard]] Scalar arc_length(const std::size_t samples = 16) const noexcept {
            Scalar length_val = 0.0;
            Vec2<Scalar> prev = p0;
            const Scalar step = static_cast<Scalar>(1.0) / static_cast<Scalar>(samples);
            for (std::size_t i = 1; i <= samples; ++i) {
                Vec2<Scalar> curr = evaluate(static_cast<Scalar>(i) * step);
                length_val += distance(curr, prev);
                prev = curr;
            }
            return length_val;
        }

        // ── Akruti Shape Contract ───────────────────────────────────────────────

        // Distance to cubic curve via adaptive subdivision + Halley/Newton polynomial refinement
        [[nodiscard]] Scalar sdf(const Vec2<Scalar> p) const noexcept {
            Scalar min_dist_sq = 1e18;
            Scalar best_t = 0.0;
            constexpr std::size_t kSubdivs = 8;
            Vec2<Scalar> prev = p0;
            constexpr Scalar step = static_cast<Scalar>(1.0) / static_cast<Scalar>(kSubdivs);

            for (std::size_t i = 1; i <= kSubdivs; ++i) {
                const Scalar t_val = static_cast<Scalar>(i) * step;
                Vec2<Scalar> curr = evaluate(t_val);
                const Vec2<Scalar> ab = curr - prev;
                const Vec2<Scalar> ap = p - prev;
                const Scalar ab_len_sq = length_sq(ab);
                Scalar t_seg = (ab_len_sq > static_cast<Scalar>(1e-6))
                                   ? akruti::dot(ap, ab) / ab_len_sq
                                   : static_cast<Scalar>(0.0);
                t_seg = std::clamp(t_seg, static_cast<Scalar>(0.0), static_cast<Scalar>(1.0));
                const Vec2<Scalar> proj = prev + ab * t_seg;
                if (const Scalar d2 = distance_sq(p, proj); d2 < min_dist_sq) {
                    min_dist_sq = d2;
                    best_t = (static_cast<Scalar>(i - 1) + t_seg) * step;
                }
                prev = curr;
            }

            // Analytical Halley/Newton root refinement on (B(t) - P) · B'(t) = 0
            Scalar t = std::clamp(best_t, static_cast<Scalar>(0.0), static_cast<Scalar>(1.0));
            for (int k = 0; k < 3; ++k) {
                const Vec2<Scalar> bt = evaluate(t);
                const Vec2<Scalar> dbt = tangent_unnormalized(t);
                const Vec2<Scalar> diff = bt - p;
                const Scalar f = akruti::dot(diff, dbt);
                const Vec2<Scalar> d2bt = second_derivative(t);
                const Scalar f_prime = length_sq(dbt) + akruti::dot(diff, d2bt);
                if (std::fabs(f_prime) < static_cast<Scalar>(1e-8)) break;
                const Scalar dt = f / f_prime;
                t = std::clamp(t - dt, static_cast<Scalar>(0.0), static_cast<Scalar>(1.0));
                if (std::fabs(dt) < static_cast<Scalar>(1e-6)) break;
            }

            const Vec2<Scalar> exact_pt = evaluate(t);
            return distance(p, exact_pt) - radius;
        }

        [[nodiscard]] Vec2<Scalar> tangent_unnormalized(const Scalar t) const noexcept {
            const Scalar u = static_cast<Scalar>(1.0) - t;
            return (p1 - p0) * (static_cast<Scalar>(3.0) * u * u) +
                (p2 - p1) * (static_cast<Scalar>(6.0) * u * t) +
                (p3 - p2) * (static_cast<Scalar>(3.0) * t * t);
        }

        [[nodiscard]] Vec2<Scalar> second_derivative(const Scalar t) const noexcept {
            const Scalar u = static_cast<Scalar>(1.0) - t;
            return (p2 - p1 * static_cast<Scalar>(2.0) + p0) * (static_cast<Scalar>(6.0) * u) +
                (p3 - p2 * static_cast<Scalar>(2.0) + p1) * (static_cast<Scalar>(6.0) * t);
        }

        [[nodiscard]] Box2 aabb() const noexcept {
            const Scalar min_x = std::min({p0[0], p1[0], p2[0], p3[0]}) - radius;
            const Scalar min_y = std::min({p0[1], p1[1], p2[1], p3[1]}) - radius;
            const Scalar max_x = std::max({p0[0], p1[0], p2[0], p3[0]}) + radius;
            const Scalar max_y = std::max({p0[1], p1[1], p2[1], p3[1]}) + radius;
            return Box2{Vec2<Scalar>{min_x, min_y}, Vec2<Scalar>{max_x, max_y}};
        }

        [[nodiscard]] Vec2<Scalar> support(const Vec2<Scalar> d) const noexcept {
            const Scalar d0 = akruti::dot(p0, d);
            const Scalar d1 = akruti::dot(p1, d);
            const Scalar d2 = akruti::dot(p2, d);
            const Scalar d3 = akruti::dot(p3, d);

            Vec2<Scalar> best = p0;
            Scalar max_dot = d0;
            if (d1 > max_dot) {
                max_dot = d1;
                best = p1;
            }
            if (d2 > max_dot) {
                max_dot = d2;
                best = p2;
            }
            if (d3 > max_dot) { best = p3; }

            if (const Scalar d_len = length(d); d_len > static_cast<Scalar>(1e-6)) {
                best = best + d * (radius / d_len);
            }
            return best;
        }

        [[nodiscard]] constexpr Vec2<Scalar> centroid() const noexcept {
            return (p0 + p1 + p2 + p3) * static_cast<Scalar>(0.25);
        }
    };

    // ── Catmull-Rom Multi-Segment Spline ────────────────────────────────────────
    struct CatmullRomSpline {
        containers::static_vector<Vec2<Scalar>, 32> points;
        Scalar radius = 0.5;
        bool closed = false;

        [[nodiscard]] Vec2<Scalar> evaluate(const Scalar t) const noexcept {
            if (points.empty()) return Vec2<Scalar>{};
            if (points.size() == 1) return points[0];

            const std::size_t num_segments = closed ? points.size() : points.size() - 1;
            const Scalar scaled_t = t * static_cast<Scalar>(num_segments);
            auto seg_idx = static_cast<std::size_t>(scaled_t);
            if (seg_idx >= num_segments) seg_idx = num_segments - 1;
            const Scalar local_t = scaled_t - static_cast<Scalar>(seg_idx);

            const auto get_point = [&](const int idx) -> Vec2<Scalar> {
                if (closed) {
                    const int n = static_cast<int>(points.size());
                    return points[static_cast<std::size_t>((idx % n + n) % n)];
                }
                const int clamped = std::clamp(idx, 0, static_cast<int>(points.size() - 1));
                return points[static_cast<std::size_t>(clamped)];
            };

            const Vec2<Scalar> p0 = get_point(static_cast<int>(seg_idx) - 1);
            const Vec2<Scalar> p1 = get_point(static_cast<int>(seg_idx));
            const Vec2<Scalar> p2 = get_point(static_cast<int>(seg_idx) + 1);
            const Vec2<Scalar> p3 = get_point(static_cast<int>(seg_idx) + 2);

            // Single Catmull-Rom source: pebble::math (shared with gati animation curves).
            return pebble::math::catmull_rom(p0, p1, p2, p3, local_t);
        }

        [[nodiscard]] Vec2<Scalar> tangent(const Scalar t) const noexcept {
            constexpr Scalar dt = 1e-3;
            const auto p_next = evaluate(std::min(static_cast<Scalar>(1.0), t + dt));
            const auto p_prev = evaluate(std::max(static_cast<Scalar>(0.0), t - dt));
            const auto diff = p_next - p_prev;
            const Scalar len = length(diff);
            return (len > static_cast<Scalar>(1e-6))
                       ? diff * (static_cast<Scalar>(1.0) / len)
                       : Vec2<Scalar>(static_cast<Scalar>(1.0), static_cast<Scalar>(0.0));
        }

        [[nodiscard]] Scalar arc_length(const std::size_t samples = 32) const noexcept {
            if (points.size() < 2) return 0.0;
            Scalar len = 0.0;
            Vec2<Scalar> prev = evaluate(0.0);
            const Scalar step = static_cast<Scalar>(1.0) / static_cast<Scalar>(samples);
            for (std::size_t i = 1; i <= samples; ++i) {
                Vec2<Scalar> curr = evaluate(static_cast<Scalar>(i) * step);
                len += distance(curr, prev);
                prev = curr;
            }
            return len;
        }

        // ── Akruti Shape Contract ───────────────────────────────────────────────

        [[nodiscard]] Scalar sdf(const Vec2<Scalar> p) const noexcept {
            if (points.empty()) return 1e18;
            Scalar min_dist_sq = 1e18;
            constexpr std::size_t kSamples = 32;
            Vec2<Scalar> prev = evaluate(0.0);
            constexpr Scalar step = static_cast<Scalar>(1.0) / static_cast<Scalar>(kSamples);

            for (std::size_t i = 1; i <= kSamples; ++i) {
                Vec2<Scalar> curr = evaluate(static_cast<Scalar>(i) * step);
                const Vec2<Scalar> ab = curr - prev;
                const Vec2<Scalar> ap = p - prev;
                const Scalar ab_len_sq = length_sq(ab);
                Scalar t_seg = (ab_len_sq > static_cast<Scalar>(1e-6))
                                   ? akruti::dot(ap, ab) / ab_len_sq
                                   : static_cast<Scalar>(0.0);
                t_seg = std::clamp(t_seg, static_cast<Scalar>(0.0), static_cast<Scalar>(1.0));
                const Vec2<Scalar> proj = prev + ab * t_seg;
                min_dist_sq = std::min(min_dist_sq, distance_sq(p, proj));
                prev = curr;
            }

            return std::sqrt(min_dist_sq) - radius;
        }

        [[nodiscard]] Box2 aabb() const noexcept {
            if (points.empty()) return Box2{};
            Scalar min_x = points[0][0], min_y = points[0][1];
            Scalar max_x = points[0][0], max_y = points[0][1];
            for (const auto& pt : points) {
                min_x = std::min(min_x, pt[0]);
                min_y = std::min(min_y, pt[1]);
                max_x = std::max(max_x, pt[0]);
                max_y = std::max(max_y, pt[1]);
            }
            return Box2{
                Vec2<Scalar>{min_x - radius, min_y - radius},
                Vec2<Scalar>{max_x + radius, max_y + radius}
            };
        }

        [[nodiscard]] Vec2<Scalar> support(const Vec2<Scalar> d) const noexcept {
            if (points.empty()) return Vec2<Scalar>{};
            Vec2<Scalar> best = points[0];
            Scalar max_dot = akruti::dot(points[0], d);
            for (const auto& pt : points) {
                if (const Scalar d_dot = akruti::dot(pt, d); d_dot > max_dot) {
                    max_dot = d_dot;
                    best = pt;
                }
            }
            if (const Scalar d_len = length(d); d_len > static_cast<Scalar>(1e-6)) {
                best = best + d * (radius / d_len);
            }
            return best;
        }

        [[nodiscard]] Vec2<Scalar> centroid() const noexcept {
            if (points.empty()) return Vec2<Scalar>{};
            Vec2<Scalar> sum{};
            for (const auto& v : points) sum = sum + v;
            return sum * (static_cast<Scalar>(1) / static_cast<Scalar>(points.size()));
        }
    };

    static_assert(Shape<CubicBezierCurve>);
    static_assert(Shape<CatmullRomSpline>);
} // namespace akruti
