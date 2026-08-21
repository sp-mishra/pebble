#pragma once
// ============================================================================
// prakriti/solvers/obstacle.hpp — rigid-obstacle contact via akruti SDF shapes.
// A PhysicsSolver that pushes dynamic particles out of any obstacle's solid region (sdf < 0),
// then applies Coulomb friction on the tangential velocity and restitution on the normal velocity.
//
// akruti is an OPTIONAL dependency: guarded with __has_include("akruti/akruti.hpp").
// Directly reuses pebble::math::vec2 with zero conversion overhead.
// ============================================================================
#if __has_include("akruti/akruti.hpp")
#define PRAKRITI_HAS_AKRUTI 1
#include "solver_base.hpp"
#include "akruti/akruti.hpp"
#include <containers/numeric/math_vector.hpp>
#include <cmath>
#include <algorithm>

namespace prakriti {

namespace detail {
// Finite-difference SDF gradient (outward normal) for an arbitrary akruti Shape.
template <class Shape>
[[nodiscard]] inline pebble::math::vec2 sdf_normal(const Shape& s,
                                                   const pebble::math::vec2& p,
                                                   Scalar h = Scalar(1e-3)) noexcept {
    const Scalar gx = s.sdf(pebble::math::vec2(p[0] + h, p[1])) - s.sdf(pebble::math::vec2(p[0] - h, p[1]));
    const Scalar gy = s.sdf(pebble::math::vec2(p[0], p[1] + h)) - s.sdf(pebble::math::vec2(p[0], p[1] - h));
    return pebble::math::normalize(pebble::math::vec2(gx, gy));
}
} // namespace detail

// ObstacleSet requirement: `set.for_each_shape([](const auto& shape){...})`.
template <class ObstacleSet>
struct ObstacleSolver {
    const ObstacleSet* obstacles{nullptr};
    ObstacleConfig     cfg{};

    explicit ObstacleSolver(const ObstacleSet& set, ObstacleConfig c = {}) noexcept
        : obstacles(&set), cfg(c) {}

    template <MaterialLaw Law>
    void solve(SolverContext<Law>& ctx) {
        if (!obstacles) return;
        dt_sub_ = ctx.dt_sub;
        auto& P = ctx.particles;
        const Index n = P.size();
        for (Index i = 0; i < n; ++i) {
            if (P.is_static(i)) continue;
            resolve_particle(P, i);
        }
    }

private:
    Scalar dt_sub_{Scalar(1)};

    // Prakriti is a position-based integrator: after the mechanics loop the engine derives
    // velocity as (pred - pos)/dt and commits pos = pred. So a contact must be resolved purely
    // by moving `pred`. We encode non-penetration AND restitution as position corrections:
    //   • push pred out of the solid by the penetration depth (contact_stiffness scales it);
    //   • re-inject the inbound normal velocity as an outward pred offset e·vn·dt so the derived
    //     (pred-pos)/dt velocity reverses with coefficient e (restitution).
    //   • remove a Coulomb-bounded friction fraction of the tangential inbound velocity.
    template <class Store>
    void resolve_particle(Store& P, Index i) {
        const pebble::math::vec2 p = P.pred_v(i);
        obstacles->for_each_shape([&](const auto& shape) {
            const Scalar d = shape.sdf(p);
            if (d >= cfg.contact_offset) return; // no contact
            const pebble::math::vec2 normal = detail::sdf_normal(shape, p);
            const Scalar penetration = cfg.contact_offset - d;

            pebble::math::vec2 pred = P.pred_v(i);
            // 1) non-penetration.
            pred = pred + normal * (penetration * cfg.contact_stiffness);

            // 2) restitution + friction from the pre-solve velocity's inbound component.
            const pebble::math::vec2 v = P.vel_v(i);
            const Scalar vn = pebble::math::dot(v, normal);          // <0 => moving into the surface
            if (vn < Scalar(0)) {
                // reverse the normal velocity with coefficient e: outward pred offset = e|vn|dt.
                pred = pred + normal * (-cfg.restitution * vn * dt_sub_);
                // friction: remove up to μ|vn| of tangential velocity, expressed as a pred shift.
                const pebble::math::vec2 normal_vn = normal * vn;
                const pebble::math::vec2 v_t = v - normal_vn;
                const Scalar t_len = pebble::math::length(v_t);
                if (t_len > Scalar(1e-9)) {
                    const Scalar remove = std::min(cfg.friction * std::fabs(vn), t_len);
                    const pebble::math::vec2 shift = v_t * ((remove / t_len) * dt_sub_);
                    pred = pred - shift;
                }
            }
            P.set_pred(i, pred);
        });
    }
};

// Deduction guide: infer ObstacleSet from constructor argument.
template <class ObstacleSet>
ObstacleSolver(const ObstacleSet&, ObstacleConfig) -> ObstacleSolver<ObstacleSet>;
template <class ObstacleSet>
ObstacleSolver(const ObstacleSet&) -> ObstacleSolver<ObstacleSet>;

} // namespace prakriti
#endif // __has_include akruti/akruti.hpp
