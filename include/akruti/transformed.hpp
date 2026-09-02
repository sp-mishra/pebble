#pragma once
// akruti/transformed.hpp — Zero-cost world-space shape wrapper satisfying Shape concept.
#include "shape.hpp"
#include "math.hpp"
#include <cmath>

namespace akruti {
    template <Shape S>
    struct TransformedShape {
        const S& shape;
        Vec2<Scalar> position{0, 0};
        Scalar angle{0}; // radians. 0 for axis-aligned (zero trig fast path)

        [[nodiscard]] constexpr Scalar sdf(Vec2<Scalar> p) const noexcept {
            const Vec2<Scalar> rel = p - position;
            if (std::fabs(angle) < Scalar(1e-7)) {
                return shape.sdf(rel);
            }
            const Scalar c = std::cos(-angle);
            const Scalar s = std::sin(-angle);
            const Vec2<Scalar> local{c * rel.x - s * rel.y, s * rel.x + c * rel.y};
            return shape.sdf(local);
        }

        [[nodiscard]] constexpr AABB<Scalar> aabb() const noexcept {
            const AABB<Scalar> local = shape.aabb();
            if (std::fabs(angle) < Scalar(1e-7)) {
                const Vec2<Scalar> pos_vec = position;
                return AABB<Scalar>{
                    pebble::math::vec2(local.lo[0] + pos_vec.x, local.lo[1] + pos_vec.y),
                    pebble::math::vec2(local.hi[0] + pos_vec.x, local.hi[1] + pos_vec.y)
                };
            }
            const Scalar c = std::abs(std::cos(angle));
            const Scalar s = std::abs(std::sin(angle));
            const Vec2<Scalar> half = Vec2<Scalar>((local.hi[0] - local.lo[0]) * 0.5f,
                                                   (local.hi[1] - local.lo[1]) * 0.5f);
            const Vec2<Scalar> center = Vec2<Scalar>((local.lo[0] + local.hi[0]) * 0.5f,
                                                     (local.lo[1] + local.hi[1]) * 0.5f);

            const Vec2<Scalar> rotated_half{
                c * half.x + s * half.y,
                s * half.x + c * half.y
            };

            const Vec2<Scalar> c_rot{
                std::cos(angle) * center.x - std::sin(angle) * center.y,
                std::sin(angle) * center.x + std::cos(angle) * center.y
            };

            const Vec2<Scalar> world_center = position + c_rot;
            return AABB<Scalar>{
                pebble::math::vec2(world_center.x - rotated_half.x, world_center.y - rotated_half.y),
                pebble::math::vec2(world_center.x + rotated_half.x, world_center.y + rotated_half.y)
            };
        }

        [[nodiscard]] constexpr Vec2<Scalar> support(Vec2<Scalar> d) const noexcept {
            if (std::fabs(angle) < Scalar(1e-7)) {
                return shape.support(d) + position;
            }
            // Rotate direction into local frame: d_local = R(-angle) * d
            const Scalar c_inv = std::cos(-angle);
            const Scalar s_inv = std::sin(-angle);
            const Vec2<Scalar> d_local{c_inv * d.x - s_inv * d.y, s_inv * d.x + c_inv * d.y};

            const Vec2<Scalar> sup_local = shape.support(d_local);

            // Rotate support point back to world frame: sup_world = R(angle) * sup_local + position
            const Scalar c = std::cos(angle);
            const Scalar s = std::sin(angle);
            const Vec2<Scalar> sup_rot{c * sup_local.x - s * sup_local.y, s * sup_local.x + c * sup_local.y};
            return sup_rot + position;
        }

        [[nodiscard]] Vec2<Scalar> centroid() const noexcept {
            const Vec2<Scalar> local = shape.centroid();
            if (std::fabs(angle) < Scalar(1e-7)) {
                return local + position;
            }
            const Scalar c = std::cos(angle);
            const Scalar s = std::sin(angle);
            const Vec2<Scalar> rot{c * local.x - s * local.y, s * local.x + c * local.y};
            return rot + position;
        }
    };

    template <Shape S>
    TransformedShape(const S&, Vec2<Scalar>, Scalar) -> TransformedShape<S>;

    template <Shape S>
    [[nodiscard]] constexpr auto transform_shape(const S& s, Vec2<Scalar> pos = {}, Scalar angle = 0) noexcept {
        return TransformedShape<S>{s, pos, angle};
    }
} // namespace akruti
