#pragma once
// akruti/transformed.hpp — Zero-cost world-space shape wrapper satisfying Shape concept.
#include "shape.hpp"
#include "math.hpp"
#include <cmath>

namespace akruti {
    template <Shape S>
    struct TransformedShape {
        const S& shape;
        Vec position{0, 0};
        Scalar angle{0}; // radians. 0 for axis-aligned (zero trig fast path)

        [[nodiscard]] constexpr Scalar sdf(const Vec p) const noexcept {
            const Vec rel = p - position;
            if (std::fabs(angle) < static_cast<Scalar>(1e-7)) {
                return shape.sdf(rel);
            }
            const Scalar c = std::cos(-angle);
            const Scalar s = std::sin(-angle);
            const Vec local{c * rel.x() - s * rel.y(), s * rel.x() + c * rel.y()};
            return shape.sdf(local);
        }

        [[nodiscard]] constexpr AABB aabb() const noexcept {
            const AABB local = shape.aabb();
            if (std::fabs(angle) < static_cast<Scalar>(1e-7)) {
                return AABB{
                    pebble::math::vec2(local.lo.x() + position.x(), local.lo.y() + position.y()),
                    pebble::math::vec2(local.hi.x() + position.x(), local.hi.y() + position.y())
                };
            }
            const Scalar c = std::abs(std::cos(angle));
            const Scalar s = std::abs(std::sin(angle));
            const auto half = Vec((local.hi.x() - local.lo.x()) * 0.5f,
                                  (local.hi.y() - local.lo.y()) * 0.5f);
            const auto center = Vec((local.lo.x() + local.hi.x()) * 0.5f,
                                    (local.lo.y() + local.hi.y()) * 0.5f);

            const Vec rotated_half{
                c * half.x() + s * half.y(),
                s * half.x() + c * half.y()
            };

            const Vec c_rot{
                std::cos(angle) * center.x() - std::sin(angle) * center.y(),
                std::sin(angle) * center.x() + std::cos(angle) * center.y()
            };

            const Vec world_center = position + c_rot;
            return AABB{
                pebble::math::vec2(world_center.x() - rotated_half.x(), world_center.y() - rotated_half.y()),
                pebble::math::vec2(world_center.x() + rotated_half.x(), world_center.y() + rotated_half.y())
            };
        }

        [[nodiscard]] constexpr Vec support(Vec d) const noexcept {
            if (std::fabs(angle) < static_cast<Scalar>(1e-7)) {
                return shape.support(d) + position;
            }
            // Rotate direction into local frame: d_local = R(-angle) * d
            const Scalar c_inv = std::cos(-angle);
            const Scalar s_inv = std::sin(-angle);
            const Vec d_local{c_inv * d.x() - s_inv * d.y(), s_inv * d.x() + c_inv * d.y()};

            const Vec sup_local = shape.support(d_local);

            // Rotate support point back to world frame: sup_world = R(angle) * sup_local + position
            const Scalar c = std::cos(angle);
            const Scalar s = std::sin(angle);
            const Vec sup_rot{c * sup_local.x() - s * sup_local.y(), s * sup_local.x() + c * sup_local.y()};
            return sup_rot + position;
        }

        [[nodiscard]] Vec centroid() const noexcept {
            const Vec local = shape.centroid();
            if (std::fabs(angle) < static_cast<Scalar>(1e-7)) {
                return local + position;
            }
            const Scalar c = std::cos(angle);
            const Scalar s = std::sin(angle);
            const Vec rot{c * local.x() - s * local.y(), s * local.x() + c * local.y()};
            return rot + position;
        }
    };

    template <Shape S>
    TransformedShape(const S&, Vec, Scalar) -> TransformedShape<S>;

    template <Shape S>
    [[nodiscard]] constexpr auto transform_shape(const S& s, Vec pos = {}, Scalar angle = 0) noexcept {
        return TransformedShape<S>{s, pos, angle};
    }
} // namespace akruti
