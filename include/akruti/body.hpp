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
#include <containers/numeric/math_vector.hpp>
#include <cmath>
#include <algorithm>

namespace akruti {

template <Shape S>
struct DynamicBody {
    S                  shape{};
    pebble::math::vec2 position{0.0f, 0.0f};
    pebble::math::vec2 linear_vel{0.0f, 0.0f};
    pebble::math::vec2 force{0.0f, 0.0f};

    Scalar angle = 0.0f;       // Rotation in radians
    Scalar angular_vel = 0.0f; // rad/s
    Scalar torque = 0.0f;

    Scalar mass = 1.0f;
    Scalar inv_mass = 1.0f;
    Scalar inertia = 1.0f;
    Scalar inv_inertia = 1.0f;

    constexpr DynamicBody() noexcept = default;
    constexpr explicit DynamicBody(S s, Scalar m = 1.0f, Scalar i = 1.0f) noexcept
        : shape(std::move(s)), mass(m), inv_mass(m > 0.0f ? 1.0f / m : 0.0f),
          inertia(i), inv_inertia(i > 0.0f ? 1.0f / i : 0.0f) {}

    // Evaluate signed distance in world coordinates
    [[nodiscard]] Scalar sdf(pebble::math::vec2 p) const noexcept {
        // Transform world point into local body frame
        pebble::math::vec2 local_p = p - position;
        const Scalar c = std::cos(-angle);
        const Scalar s = std::sin(-angle);
        pebble::math::vec2 rot_p{
            local_p[0] * c - local_p[1] * s,
            local_p[0] * s + local_p[1] * c
        };
        return shape.sdf(rot_p);
    }

    // World AABB enclosing rotated shape
    [[nodiscard]] AABB<Scalar> aabb() const noexcept {
        auto box = shape.aabb();
        Scalar rad = std::max(std::abs(box.hi[0] - box.lo[0]), std::abs(box.hi[1] - box.lo[1])) * 0.7071f;
        return AABB<Scalar>{
            pebble::math::vec2(position[0] - rad, position[1] - rad),
            pebble::math::vec2(position[0] + rad, position[1] + rad)
        };
    }

    // Support point in world space for GJK/EPA
    [[nodiscard]] Vec2<Scalar> support(Vec2<Scalar> d) const noexcept {
        const Scalar c = std::cos(-angle);
        const Scalar s = std::sin(-angle);
        Vec2<Scalar> local_d{
            d.x * c - d.y * s,
            d.x * s + d.y * c
        };
        Vec2<Scalar> local_s = shape.support(local_d);
        const Scalar rc = std::cos(angle);
        const Scalar rs = std::sin(angle);
        return Vec2<Scalar>{
            position[0] + (local_s.x * rc - local_s.y * rs),
            position[1] + (local_s.x * rs + local_s.y * rc)
        };
    }

    // Integrate forces and update position/orientation over time dt
    void step(Scalar dt, pebble::math::vec2 gravity = {0.0f, 0.0f}) noexcept {
        if (inv_mass <= 0.0f) return; // Static body

        // Linear velocity integration
        linear_vel = linear_vel + (gravity + force * inv_mass) * dt;
        position = position + linear_vel * dt;

        // Angular velocity integration
        angular_vel += (torque * inv_inertia) * dt;
        angle += angular_vel * dt;

        // Reset accumulators
        force = {0.0f, 0.0f};
        torque = 0.0f;
    }

    // Apply world impulse at a specific world contact point
    void apply_impulse(pebble::math::vec2 impulse, pebble::math::vec2 world_pt) noexcept {
        linear_vel = linear_vel + impulse * inv_mass;
        pebble::math::vec2 r = world_pt - position;
        torque += (r[0] * impulse[1] - r[1] * impulse[0]);
    }
};

template <Shape S>
DynamicBody(S, Scalar, Scalar) -> DynamicBody<S>;

} // namespace akruti
