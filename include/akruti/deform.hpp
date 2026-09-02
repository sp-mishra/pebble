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

        [[nodiscard]] Scalar sdf(Vec p) const noexcept {
            if (std::abs(curvature) < static_cast<Scalar>(1e-6)) {
                return shape.sdf(p);
            }
            // Coordinate transformation for circular bend
            const Scalar c = std::cos(curvature * x(p));
            const Scalar s = std::sin(curvature * x(p));
            const Mat2<Scalar> m{c, -s, s, c};
            const Vec q = m * p;
            return shape.sdf(q);
        }

        [[nodiscard]] Box2 aabb() const noexcept {
            auto box = shape.aabb();
            const Scalar pad = std::abs(curvature) * (x(box.hi) - x(box.lo)) * static_cast<Scalar>(5.0);
            return Box2{
                Vec{x(box.lo) - pad, y(box.lo) - pad},
                Vec{x(box.hi) + pad, y(box.hi) + pad}
            };
        }

        [[nodiscard]] Vec support(Vec d) const noexcept {
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

        [[nodiscard]] Scalar sdf(const Vec p) const noexcept {
            const Scalar scale = std::max(static_cast<Scalar>(1e-4), static_cast<Scalar>(1.0) + k * y(p));
            const Vec q{x(p) / scale, y(p)};
            return shape.sdf(q) * scale;
        }

        [[nodiscard]] Box2 aabb() const noexcept {
            auto box = shape.aabb();
            const Scalar max_scale = std::max(static_cast<Scalar>(1.0) + k * y(box.lo),
                                              static_cast<Scalar>(1.0) + k * y(box.hi));
            return Box2{
                Vec{x(box.lo) * max_scale, y(box.lo)},
                Vec{x(box.hi) * max_scale, y(box.hi)}
            };
        }

        [[nodiscard]] Vec support(Vec d) const noexcept {
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

        [[nodiscard]] Scalar sdf(const Vec p) const noexcept {
            const Scalar sx = std::max(static_cast<Scalar>(1e-4), factor);
            const Scalar sy = static_cast<Scalar>(1.0) / sx; // Preserves 2D area (det = 1.0)
            const Vec q{x(p) / sx, y(p) / sy};
            return shape.sdf(q) * std::min(sx, sy);
        }

        [[nodiscard]] Box2 aabb() const noexcept {
            auto box = shape.aabb();
            const Scalar sx = factor;
            const Scalar sy = static_cast<Scalar>(1.0) / sx;
            return Box2{
                Vec{x(box.lo) * sx, y(box.lo) * sy},
                Vec{x(box.hi) * sx, y(box.hi) * sy}
            };
        }

        [[nodiscard]] Vec support(const Vec d) const noexcept {
            const Scalar sx = factor;
            const Scalar sy = static_cast<Scalar>(1.0) / sx;
            auto s = shape.support(Vec{x(d) * sx, y(d) * sy});
            return Vec{x(s) * sx, y(s) * sy};
        }
    };

    template <Shape S>
    [[nodiscard]] constexpr SquashStretchShape<S> squash_stretch(
        S shape, Scalar factor = static_cast<Scalar>(1.0)) noexcept {
        return SquashStretchShape<S>{std::move(shape), factor};
    }
} // namespace akruti::deform
