#pragma once
// ============================================================================
// akruti/body.hpp — Dynamic 2-Way Coupled Rigid & Deformable Bodies
// ============================================================================
// First-class dynamic rigid body representation in Akruti that can:
//  - Integrate 6-DOF (2D translation + rotation) motion under hydrodynamic & contact forces
//  - Compute exact signed distance field (SDF) and bounding box in world space
//  - Two-way couple with Prakriti particle and fluid columns
// ============================================================================
#include "shape.hpp"
#include "math.hpp"
#include "transformed.hpp"
#include <containers/numeric/math_vector.hpp>
#include <cmath>
#include <algorithm>

namespace akruti {
    // Backward compatibility forwarder: for new code use gati::RigidBody and akruti::TransformedShape
    template <Shape S>
    struct DynamicBody {
        S shape{};
        pebble::math::vec2 position{0.0f, 0.0f};
        pebble::math::vec2 linear_vel{0.0f, 0.0f};
        pebble::math::vec2 force{0.0f, 0.0f};

        Scalar angle = 0.0f;
        Scalar angular_vel = 0.0f;
        Scalar torque = 0.0f;

        Scalar mass = 1.0f;
        Scalar inv_mass = 1.0f;
        Scalar inertia = 1.0f;
        Scalar inv_inertia = 1.0f;

        constexpr DynamicBody() noexcept = default;

        constexpr explicit DynamicBody(S s, Scalar m = 1.0f, Scalar i = 1.0f) noexcept
            : shape(std::move(s)), mass(m), inv_mass(m > 0.0f ? 1.0f / m : 0.0f),
              inertia(i), inv_inertia(i > 0.0f ? 1.0f / i : 0.0f) {}

        [[nodiscard]] Scalar sdf(pebble::math::vec2 p) const noexcept {
            const TransformedShape ts{shape, Vec{position[0], position[1]}, angle};
            return ts.sdf(Vec{p[0], p[1]});
        }

        [[nodiscard]] AABB<Scalar> aabb() const noexcept {
            const TransformedShape ts{shape, Vec{position[0], position[1]}, angle};
            return ts.aabb();
        }

        [[nodiscard]] Vec2<Scalar> support(Vec2<Scalar> d) const noexcept {
            const TransformedShape ts{shape, Vec{position[0], position[1]}, angle};
            return ts.support(d);
        }

        [[nodiscard]] Vec2<Scalar> centroid() const noexcept {
            const TransformedShape ts{shape, Vec{position[0], position[1]}, angle};
            return ts.centroid();
        }

        void step(Scalar dt, pebble::math::vec2 gravity = {0.0f, 0.0f}) noexcept {
            if (inv_mass <= 0.0f) return;
            linear_vel = linear_vel + (gravity + force * inv_mass) * dt;
            position = position + linear_vel * dt;
            angular_vel += (torque * inv_inertia) * dt;
            angle += angular_vel * dt;
            force = {0.0f, 0.0f};
            torque = 0.0f;
        }

        void apply_impulse(pebble::math::vec2 impulse, pebble::math::vec2 world_pt) noexcept {
            linear_vel = linear_vel + impulse * inv_mass;
            pebble::math::vec2 r = world_pt - position;
            torque += (r[0] * impulse[1] - r[1] * impulse[0]);
        }
    };

    template <Shape S>
    DynamicBody(S, Scalar, Scalar) -> DynamicBody<S>;
} // namespace akruti
