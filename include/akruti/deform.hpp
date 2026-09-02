#pragma once
// ============================================================================
// akruti/deform.hpp — Non-Linear Geometric Space Deformations & Modifiers
// ============================================================================
// Applies spatial coordinate transformations to signed distance fields:
//   - bend(shape, curvature)
//   - taper(shape, k)
//   - squash_stretch(shape, factor)
// ============================================================================

#include "shape.hpp"
#include "math.hpp"
#include <algorithm>
#include <cmath>

namespace akruti::deform {
    // ── Bent Shape Modifier ─────────────────────────────────────────────────────
    template <Shape S>
    struct BentShape {
        S shape{};
        Scalar curvature = static_cast<Scalar>(0.02); // k = 1 / R

        [[nodiscard]] Scalar sdf(Vec2<Scalar> p) const noexcept {
            if (std::abs(curvature) < static_cast<Scalar>(1e-6)) {
                return shape.sdf(p);
            }
            // Coordinate transformation for circular bend
            const Scalar c = std::cos(curvature * p.x);
            const Scalar s = std::sin(curvature * p.x);
            const Mat2<Scalar> m{c, -s, s, c};
            const Vec2<Scalar> q = m * p;
            return shape.sdf(q);
        }

        [[nodiscard]] AABB<Scalar> aabb() const noexcept {
            auto box = shape.aabb();
            const Scalar pad = std::abs(curvature) * (box.hi[0] - box.lo[0]) * static_cast<Scalar>(5.0);
            return AABB<Scalar>{
                pebble::math::vec2(box.lo[0] - pad, box.lo[1] - pad),
                pebble::math::vec2(box.hi[0] + pad, box.hi[1] + pad)
            };
        }

        [[nodiscard]] Vec2<Scalar> support(Vec2<Scalar> d) const noexcept {
            return shape.support(d);
        }
    };

    template <Shape S>
    [[nodiscard]] constexpr BentShape<S> bend(S shape, Scalar curvature = static_cast<Scalar>(0.02)) noexcept {
        return BentShape<S>{std::move(shape), curvature};
    }

    // ── Tapered Shape Modifier ──────────────────────────────────────────────────
    template <Shape S>
    struct TaperedShape {
        S shape{};
        Scalar k = static_cast<Scalar>(0.1); // Taper slope per unit Y

        [[nodiscard]] Scalar sdf(const Vec2<Scalar> p) const noexcept {
            const Scalar scale = std::max(static_cast<Scalar>(1e-4), static_cast<Scalar>(1.0) + k * p.y);
            const Vec2<Scalar> q{p.x / scale, p.y};
            return shape.sdf(q) * scale;
        }

        [[nodiscard]] AABB<Scalar> aabb() const noexcept {
            auto box = shape.aabb();
            const Scalar max_scale = std::max(static_cast<Scalar>(1.0) + k * box.lo[1],
                                              static_cast<Scalar>(1.0) + k * box.hi[1]);
            return AABB<Scalar>{
                pebble::math::vec2(box.lo[0] * max_scale, box.lo[1]),
                pebble::math::vec2(box.hi[0] * max_scale, box.hi[1])
            };
        }

        [[nodiscard]] Vec2<Scalar> support(Vec2<Scalar> d) const noexcept {
            return shape.support(d);
        }
    };

    template <Shape S>
    [[nodiscard]] constexpr TaperedShape<S> taper(S shape, Scalar k = static_cast<Scalar>(0.1)) noexcept {
        return TaperedShape<S>{std::move(shape), k};
    }

    // ── Volume-Preserving Squash and Stretch ─────────────────────────────────────
    template <Shape S>
    struct SquashStretchShape {
        S shape{};
        Scalar factor = static_cast<Scalar>(1.0); // > 1: horizontal stretch, < 1: vertical stretch

        [[nodiscard]] Scalar sdf(const Vec2<Scalar> p) const noexcept {
            const Scalar sx = std::max(static_cast<Scalar>(1e-4), factor);
            const Scalar sy = static_cast<Scalar>(1.0) / sx; // Preserves 2D area (det = 1.0)
            const Vec2<Scalar> q{p.x / sx, p.y / sy};
            return shape.sdf(q) * std::min(sx, sy);
        }

        [[nodiscard]] AABB<Scalar> aabb() const noexcept {
            auto box = shape.aabb();
            const Scalar sx = factor;
            const Scalar sy = static_cast<Scalar>(1.0) / sx;
            return AABB<Scalar>{
                pebble::math::vec2(box.lo[0] * sx, box.lo[1] * sy),
                pebble::math::vec2(box.hi[0] * sx, box.hi[1] * sy)
            };
        }

        [[nodiscard]] Vec2<Scalar> support(const Vec2<Scalar> d) const noexcept {
            const Scalar sx = factor;
            const Scalar sy = static_cast<Scalar>(1.0) / sx;
            auto s = shape.support(Vec2<Scalar>{d.x * sx, d.y * sy});
            return Vec2<Scalar>{s.x * sx, s.y * sy};
        }
    };

    template <Shape S>
    [[nodiscard]] constexpr SquashStretchShape<S> squash_stretch(
        S shape, Scalar factor = static_cast<Scalar>(1.0)) noexcept {
        return SquashStretchShape<S>{std::move(shape), factor};
    }
} // namespace akruti::deform
