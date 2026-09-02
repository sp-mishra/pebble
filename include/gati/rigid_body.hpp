#pragma once
// gati/rigid_body.hpp — Dynamic 2D Rigid Body with mass, inertia, and Akruti Shape integration.
#include "akruti/shape_store.hpp"
#include "akruti/transformed.hpp"
#include "akruti/math.hpp"
#include <containers/numeric/math_vector.hpp>
#include <cmath>
#include <algorithm>

namespace gati {
    using Scalar = akruti::Scalar;
    using Vec = akruti::Vec;

    struct RigidBodyDesc {
        pebble::math::vec2 position{0.0f, 0.0f};
        pebble::math::vec2 velocity{0.0f, 0.0f};
        Scalar angle{0.0f};
        Scalar angular_velocity{0.0f};
        Scalar mass{1.0f};
        Scalar inertia{1.0f};
        Scalar restitution{0.2f};
        Scalar friction{0.5f};
        bool is_static{false};
    };

    struct RigidBody {
        akruti::ShapeStore shape{};
        pebble::math::vec2 position{0.0f, 0.0f};
        pebble::math::vec2 velocity{0.0f, 0.0f};
        pebble::math::vec2 force{0.0f, 0.0f};

        Scalar angle{0.0f};
        Scalar angular_velocity{0.0f};
        Scalar torque{0.0f};

        Scalar mass{1.0f};
        Scalar inv_mass{1.0f};
        Scalar inertia{1.0f};
        Scalar inv_inertia{1.0f};

        Scalar restitution{0.2f};
        Scalar friction{0.5f};

        bool is_sleeping{false};
        Scalar sleep_timer{0.0f};

        constexpr RigidBody() noexcept = default;

        RigidBody(const akruti::ShapeStore& s, const RigidBodyDesc& desc = {}) noexcept
            : shape(s), position(desc.position), velocity(desc.velocity),
              angle(desc.angle), angular_velocity(desc.angular_velocity),
              mass(desc.is_static ? 0.0f : desc.mass),
              inv_mass(desc.is_static || desc.mass <= 0.0f ? 0.0f : 1.0f / desc.mass),
              inertia(desc.is_static ? 0.0f : desc.inertia),
              inv_inertia(desc.is_static || desc.inertia <= 0.0f ? 0.0f : 1.0f / desc.inertia),
              restitution(desc.restitution), friction(desc.friction) {}

        template <akruti::Shape S>
        RigidBody(const S& s, const RigidBodyDesc& desc = {}) noexcept
            : shape(s), position(desc.position), velocity(desc.velocity),
              angle(desc.angle), angular_velocity(desc.angular_velocity),
              mass(desc.is_static ? 0.0f : desc.mass),
              inv_mass(desc.is_static || desc.mass <= 0.0f ? 0.0f : 1.0f / desc.mass),
              inertia(desc.is_static ? 0.0f : desc.inertia),
              inv_inertia(desc.is_static || desc.inertia <= 0.0f ? 0.0f : 1.0f / desc.inertia),
              restitution(desc.restitution), friction(desc.friction) {}

        [[nodiscard]] bool is_static() const noexcept { return inv_mass <= 0.0f; }

        [[nodiscard]] Scalar kinetic_energy() const noexcept {
            if (is_static()) return 0.0f;
            const Scalar v2 = velocity[0] * velocity[0] + velocity[1] * velocity[1];
            const Scalar w2 = angular_velocity * angular_velocity;
            return 0.5f * (mass * v2 + inertia * w2);
        }

        [[nodiscard]] Scalar sdf(pebble::math::vec2 p) const noexcept {
            const akruti::TransformedShape ts{shape, Vec{position[0], position[1]}, angle};
            return ts.sdf(Vec{p[0], p[1]});
        }

        [[nodiscard]] akruti::AABB<Scalar> aabb() const noexcept {
            const akruti::TransformedShape ts{shape, Vec{position[0], position[1]}, angle};
            return ts.aabb();
        }

        [[nodiscard]] Vec support(Vec d) const noexcept {
            const akruti::TransformedShape ts{shape, Vec{position[0], position[1]}, angle};
            return ts.support(d);
        }

        void apply_force(pebble::math::vec2 f) noexcept {
            if (is_static()) return;
            force = force + f;
            is_sleeping = false;
        }

        void apply_torque(Scalar t) noexcept {
            if (is_static()) return;
            torque += t;
            is_sleeping = false;
        }

        void apply_impulse(pebble::math::vec2 impulse, pebble::math::vec2 world_pt) noexcept {
            if (is_static()) return;
            velocity = velocity + impulse * inv_mass;
            const pebble::math::vec2 r = world_pt - position;
            angular_velocity += (r[0] * impulse[1] - r[1] * impulse[0]) * inv_inertia;
            is_sleeping = false;
        }

        void step(Scalar dt, pebble::math::vec2 gravity = {0.0f, 0.0f}) noexcept {
            if (is_static() || is_sleeping) return;

            // Semi-implicit Euler integration
            velocity = velocity + (gravity + force * inv_mass) * dt;
            position = position + velocity * dt;

            angular_velocity += (torque * inv_inertia) * dt;
            angle += angular_velocity * dt;

            force = {0.0f, 0.0f};
            torque = 0.0f;
        }
    };
} // namespace gati
