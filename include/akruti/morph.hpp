#pragma once
// ============================================================================
// akruti/morph.hpp — Continuous SDF Shape Morphing & Topological Blending
// ============================================================================
// Blends any two Akruti Shapes by interpolating their signed distance fields:
//   sdf_morph(p, t) = (1 - t) * sdf_A(p) + t * sdf_B(p)
// Implements the full Shape concept contract.
// ============================================================================

#include "shape.hpp"
#include "math.hpp"
#include <algorithm>
#include <utility>

namespace akruti {
    template <Shape ShapeA, Shape ShapeB>
    struct ShapeMorph {
        ShapeA shape_a{};
        ShapeB shape_b{};
        Scalar t = static_cast<Scalar>(0.0); // Morph parameter in [0, 1]

        [[nodiscard]] Scalar sdf(Vec p) const noexcept {
            const Scalar da = shape_a.sdf(p);
            const Scalar db = shape_b.sdf(p);
            const Scalar factor = std::clamp(t, static_cast<Scalar>(0.0), static_cast<Scalar>(1.0));
            return da * (static_cast<Scalar>(1.0) - factor) + db * factor;
        }

        [[nodiscard]] Box2 aabb() const noexcept {
            const auto box_a = shape_a.aabb();
            const auto box_b = shape_b.aabb();
            const Scalar factor = std::clamp(t, static_cast<Scalar>(0.0), static_cast<Scalar>(1.0));
            const Vec lo = box_a.lo * (static_cast<Scalar>(1.0) - factor) + box_b.lo * factor;
            const Vec hi = box_a.hi * (static_cast<Scalar>(1.0) - factor) + box_b.hi * factor;
            return Box2{lo, hi};
        }

        [[nodiscard]] Vec support(Vec d) const noexcept {
            const auto s_a = shape_a.support(d);
            const auto s_b = shape_b.support(d);
            const Scalar factor = std::clamp(t, static_cast<Scalar>(0.0), static_cast<Scalar>(1.0));
            return s_a * (static_cast<Scalar>(1.0) - factor) + s_b * factor;
        }
    };

    template <Shape ShapeA, Shape ShapeB>
    [[nodiscard]] constexpr ShapeMorph<ShapeA, ShapeB> morph(ShapeA a, ShapeB b, Scalar t = static_cast<Scalar>(0.0)) noexcept {
        return ShapeMorph<ShapeA, ShapeB>{std::move(a), std::move(b), t};
    }
} // namespace akruti
