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

        constexpr explicit DynamicBody(S s, const Scalar m = 1.0f, const Scalar i = 1.0f) noexcept
            : shape(std::move(s)), mass(m), inv_mass(m > 0.0f ? 1.0f / m : 0.0f),
              inertia(i), inv_inertia(i > 0.0f ? 1.0f / i : 0.0f) {}

        [[nodiscard]] Scalar sdf(pebble::math::vec2 p) const noexcept {
            const TransformedShape ts{shape, position, angle};
            return ts.sdf(p);
        }

        [[nodiscard]] AABB aabb() const noexcept {
            const TransformedShape ts{shape, position, angle};
            return ts.aabb();
        }

        [[nodiscard]] Vec support(Vec d) const noexcept {
            const TransformedShape ts{shape, position, angle};
            return ts.support(d);
        }

        [[nodiscard]] Vec centroid() const noexcept {
            const TransformedShape ts{shape, position, angle};
            return ts.centroid();
        }

        void step(const Scalar dt, const Vec gravity = Vec{0.0f, 0.0f}) noexcept {
            if (inv_mass <= 0.0f) return;
            linear_vel = linear_vel + (gravity + force * inv_mass) * dt;
            position = position + linear_vel * dt;
            angular_vel += (torque * inv_inertia) * dt;
            angle += angular_vel * dt;
            force = Vec{0.0f, 0.0f};
            torque = 0.0f;
        }

        void apply_impulse(Vec impulse, const Vec world_pt) noexcept {
            linear_vel = linear_vel + impulse * inv_mass;
            Vec r = world_pt - position;
            torque += (x(r) * y(impulse) - y(r) * x(impulse));
        }
    };

    template <Shape S>
    DynamicBody(S, Scalar, Scalar) -> DynamicBody<S>;
} // namespace akruti
