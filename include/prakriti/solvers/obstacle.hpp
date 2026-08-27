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
// Fast analytical outward normals for core primitives avoiding finite-difference overhead
inline pebble::math::vec2 sdf_normal(const akruti::Circle& c, const pebble::math::vec2& p, Scalar = 0.0f) noexcept {
    const akruti::Vec d = akruti::Vec(p) - c.center;
    const Scalar len = d.len();
    return len > Scalar(1e-6) ? pebble::math::vec2(d.x / len, d.y / len) : pebble::math::vec2(0.0f, 1.0f);
}

inline pebble::math::vec2 sdf_normal(const akruti::Capsule& cap, const pebble::math::vec2& p, Scalar = 0.0f) noexcept {
    const akruti::Vec ap = akruti::Vec(p) - cap.a;
    const akruti::Vec ab = cap.b - cap.a;
    const Scalar t = std::clamp(ap.dot(ab) / std::max(ab.len2(), Scalar(1e-12)), Scalar(0), Scalar(1));
    const akruti::Vec proj = cap.a + ab * t;
    const akruti::Vec d = akruti::Vec(p) - proj;
    const Scalar len = d.len();
    return len > Scalar(1e-6) ? pebble::math::vec2(d.x / len, d.y / len) : pebble::math::vec2(0.0f, 1.0f);
}

inline pebble::math::vec2 sdf_normal(const akruti::Box& b, const pebble::math::vec2& p, Scalar = 0.0f) noexcept {
    const akruti::Vec d = akruti::Vec(p) - b.center;
    const Scalar dx = std::abs(d.x) - b.half.x;
    const Scalar dy = std::abs(d.y) - b.half.y;
    const Scalar sx = d.x >= 0.0f ? 1.0f : -1.0f;
    const Scalar sy = d.y >= 0.0f ? 1.0f : -1.0f;
    if (dx > dy) {
        return pebble::math::vec2(sx, 0.0f);
    } else {
        return pebble::math::vec2(0.0f, sy);
    }
}

inline pebble::math::vec2 sdf_normal(const akruti::OrientedBox& obb, const pebble::math::vec2& p, Scalar = 0.0f) noexcept {
    const akruti::Vec diff = akruti::Vec(p) - obb.center;
    const akruti::Vec local_d{obb.rot.m00 * diff.x + obb.rot.m10 * diff.y,
                              obb.rot.m01 * diff.x + diff.y * obb.rot.m11};
    const Scalar dx = std::abs(local_d.x) - obb.half.x;
    const Scalar dy = std::abs(local_d.y) - obb.half.y;
    const Scalar sx = local_d.x >= 0.0f ? 1.0f : -1.0f;
    const Scalar sy = local_d.y >= 0.0f ? 1.0f : -1.0f;
    const akruti::Vec local_n = (dx > dy) ? akruti::Vec(sx, 0.0f) : akruti::Vec(0.0f, sy);
    const akruti::Vec world_n = obb.rot * local_n;
    return pebble::math::vec2(world_n.x, world_n.y);
}

// Fallback finite-difference SDF gradient for arbitrary akruti Shapes.
template <class Shape>
[[nodiscard]] inline pebble::math::vec2 sdf_normal(const Shape& s,
                                                   const pebble::math::vec2& p,
                                                   Scalar h = Scalar(1e-2)) noexcept {
    const Scalar gx = s.sdf(pebble::math::vec2(p[0] + h, p[1])) - s.sdf(pebble::math::vec2(p[0] - h, p[1]));
    const Scalar gy = s.sdf(pebble::math::vec2(p[0], p[1] + h)) - s.sdf(pebble::math::vec2(p[0], p[1] - h));
    const Scalar len = std::sqrt(gx * gx + gy * gy);
    return len > Scalar(1e-6) ? pebble::math::vec2(gx / len, gy / len) : pebble::math::vec2(0.0f, 1.0f);
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
        pebble::math::vec2 pred = P.pred_v(i);
        const pebble::math::vec2 pos = P.pos_v(i);
        const pebble::math::vec2 v = P.vel_v(i);

        obstacles->for_each_shape([&](const auto& shape) {
            // 1. Continuous Collision Detection (CCD) Ray Sweep
            // Test if trajectory from pos to pred crosses the obstacle surface
            const Scalar d_pred = shape.sdf(pred);
            const Scalar d_pos = shape.sdf(pos);

            // If tunneling detected (started outside, ended inside or deeply penetrated)
            if (d_pos >= Scalar(0) && d_pred < cfg.contact_offset) {
                // Speculative contact manifold projection
                const Scalar denom = (d_pos - d_pred);
                const Scalar t_hit = denom > Scalar(1e-6) ? std::clamp(d_pos / denom, Scalar(0), Scalar(1)) : Scalar(0.5);
                const pebble::math::vec2 hit_pt = pos + (pred - pos) * t_hit;
                const pebble::math::vec2 normal = detail::sdf_normal(shape, hit_pt);
                pred = hit_pt + normal * (cfg.contact_offset * cfg.contact_stiffness);

                const Scalar vn = pebble::math::dot(v, normal);
                if (vn < Scalar(-25.0)) { // Restitution threshold to avoid resting chatter
                    pred = pred + normal * (-cfg.restitution * vn * dt_sub_);
                }
                if (vn < Scalar(0)) {
                    const pebble::math::vec2 v_t = v - normal * vn;
                    const Scalar t_len = pebble::math::length(v_t);
                    if (t_len > Scalar(1e-6)) {
                        const Scalar remove = std::min(cfg.friction * std::fabs(vn), t_len);
                        pred = pred - v_t * ((remove / t_len) * dt_sub_);
                    }
                }
                return;
            }

            if (d_pred >= cfg.contact_offset) return; // fast early out

            const pebble::math::vec2 normal = detail::sdf_normal(shape, pred);
            const Scalar penetration = cfg.contact_offset - d_pred;

            // Non-penetration projection
            pred = pred + normal * (penetration * cfg.contact_stiffness);

            // Restitution + Coulomb friction
            const Scalar vn = pebble::math::dot(v, normal);
            if (vn < Scalar(-25.0)) { // Restitution threshold to avoid resting chatter
                pred = pred + normal * (-cfg.restitution * vn * dt_sub_);
            }
            if (vn < Scalar(0)) {
                const pebble::math::vec2 v_t = v - normal * vn;
                const Scalar t_len_sq = pebble::math::length_sq(v_t);
                if (t_len_sq > Scalar(1e-12)) {
                    const Scalar t_len = std::sqrt(t_len_sq);
                    const Scalar remove = std::min(cfg.friction * std::fabs(vn), t_len);
                    const pebble::math::vec2 shift = v_t * ((remove / t_len) * dt_sub_);
                    pred = pred - shift;
                }
            }
        });
        P.set_pred(i, pred);
    }
};

// Deduction guide: infer ObstacleSet from constructor argument.
template <class ObstacleSet>
ObstacleSolver(const ObstacleSet&, ObstacleConfig) -> ObstacleSolver<ObstacleSet>;
template <class ObstacleSet>
ObstacleSolver(const ObstacleSet&) -> ObstacleSolver<ObstacleSet>;

} // namespace prakriti
#endif // __has_include akruti/akruti.hpp
