#pragma once
// akruti/gradient.hpp — Analytical and finite-difference SDF gradients/normals for shapes.
#include "shape.hpp"
#include "primitives.hpp"
#include "transformed.hpp"
#include <cmath>
#include <algorithm>

namespace akruti {
    // ── Analytical outward normal / SDF gradient for primitive shapes ──────────

    [[nodiscard]] inline Vec sdf_gradient(const Circle& c, Vec p) noexcept {
        const Vec diff = p - c.center;
        const Scalar d = diff.len();
        return d > Scalar(1e-6) ? (diff / d) : Vec{0, 1};
    }

    [[nodiscard]] inline Vec sdf_gradient(const Box& b, Vec p) noexcept {
        const Vec d = Vec{
            std::fabs(p.x - b.center.x) - b.half.x,
            std::fabs(p.y - b.center.y) - b.half.y
        };
        const Vec sign = Vec{
            p.x >= b.center.x ? 1.0f : -1.0f,
            p.y >= b.center.y ? 1.0f : -1.0f
        };

        if (d.x > 0.0f || d.y > 0.0f) {
            const Vec clamped = Vec{std::max(d.x, 0.0f), std::max(d.y, 0.0f)};
            const Scalar len = clamped.len();
            return len > 1e-6f ? Vec{sign.x * clamped.x / len, sign.y * clamped.y / len} : sign;
        }
        // Inside box: normal along shallowest exit axis
        return (d.x > d.y) ? Vec{sign.x, 0.0f} : Vec{0.0f, sign.y};
    }

    [[nodiscard]] inline Vec sdf_gradient(const Capsule& cap, Vec p) noexcept {
        const Vec ab = cap.b - cap.a;
        const Vec ap = p - cap.a;
        const Scalar t = std::clamp(ap.dot(ab) / std::max(ab.len2(), Scalar(1e-12)), Scalar(0), Scalar(1));
        const Vec closest = cap.a + ab * t;
        const Vec diff = p - closest;
        const Scalar d = diff.len();
        return d > Scalar(1e-6) ? (diff / d) : Vec{0, 1};
    }

    [[nodiscard]] inline Vec sdf_gradient(const OrientedBox& obb, Vec p) noexcept {
        const Vec diff = p - obb.center;
        // Local coords
        const Vec local{
            obb.rot(0, 0) * diff.x + obb.rot(1, 0) * diff.y,
            obb.rot(0, 1) * diff.x + obb.rot(1, 1) * diff.y
        };
        const Box local_box{{0, 0}, obb.half};
        const Vec local_grad = sdf_gradient(local_box, local);
        // Rotate back to world: R * local_grad
        return obb.rot * local_grad;
    }

    [[nodiscard]] inline Vec sdf_gradient(const RoundedBox& rb, Vec p) noexcept {
        const Box inner{
            rb.center, Vec{
                std::max(0.0f, rb.half.x - rb.radius),
                std::max(0.0f, rb.half.y - rb.radius)
            }
        };
        return sdf_gradient(inner, p);
    }

    [[nodiscard]] inline Vec sdf_gradient(const Segment& s, Vec p) noexcept {
        const Vec ab = s.b - s.a;
        const Vec ap = p - s.a;
        const Scalar t = std::clamp(ap.dot(ab) / std::max(ab.len2(), Scalar(1e-12)), Scalar(0), Scalar(1));
        const Vec closest = s.a + ab * t;
        const Vec diff = p - closest;
        const Scalar d = diff.len();
        if (d > Scalar(1e-6)) return diff / d;
        // Perpendicular to segment
        const Vec normal{-ab.y, ab.x};
        return normal.normalized();
    }

    // ── Generic Fallback: Finite-Difference SDF Gradient ───────────────────────
    template <Shape S>
    [[nodiscard]] inline Vec sdf_gradient_fd(const S& shape, Vec p, Scalar h = 1e-3f) noexcept {
        const Scalar dx = shape.sdf(Vec{p.x + h, p.y}) - shape.sdf(Vec{p.x - h, p.y});
        const Scalar dy = shape.sdf(Vec{p.x, p.y + h}) - shape.sdf(Vec{p.x, p.y - h});
        const Vec g{dx, dy};
        const Scalar len = g.len();
        return len > 1e-6f ? (g / len) : Vec{0, 1};
    }

    template <Shape S>
    [[nodiscard]] inline Vec sdf_gradient(const S& shape, Vec p) noexcept {
        if constexpr (std::is_same_v<S, Circle> || std::is_same_v<S, Box> ||
            std::is_same_v<S, Capsule> || std::is_same_v<S, OrientedBox> ||
            std::is_same_v<S, RoundedBox> || std::is_same_v<S, Segment>) {
            return sdf_gradient(shape, p);
        }
        else {
            return sdf_gradient_fd(shape, p);
        }
    }

    template <Shape S>
    [[nodiscard]] inline Vec sdf_gradient(const TransformedShape<S>& ts, Vec p) noexcept {
        if (std::fabs(ts.angle) < Scalar(1e-7)) {
            return sdf_gradient(ts.shape, p - ts.position);
        }
        const Scalar c_inv = std::cos(-ts.angle);
        const Scalar s_inv = std::sin(-ts.angle);
        const Vec rel = p - ts.position;
        const Vec local{c_inv * rel.x - s_inv * rel.y, s_inv * rel.x + c_inv * rel.y};

        const Vec g_local = sdf_gradient(ts.shape, local);

        const Scalar c = std::cos(ts.angle);
        const Scalar s = std::sin(ts.angle);
        return Vec{c * g_local.x - s * g_local.y, s * g_local.x + c * g_local.y};
    }
} // namespace akruti
