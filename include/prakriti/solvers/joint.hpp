#pragma once
// ============================================================================
// prakriti/solvers/joint.hpp — XPBD joint constraints driven by akruti joint-frame geometry.
// A PhysicsSolver over a list of akruti::Joint. body_a/body_b are particle indices here (point
// masses, no rotational DOF), so the solver projects the POSITIONAL part of each joint:
//   Distance  — keep |a-b| == rest_length (compliant rod/spring).
//   Revolute  — coincident anchors (rest_length 0 distance constraint).
//   Prismatic — project relative offset onto slide axis (point-on-line), clamp to limits.
//   Weld      — coincident anchors (angle DOF absent for point particles → positional weld).
//   Motor     — angle-only; skipped for point particles.
//
// akruti is OPTIONAL: guarded with __has_include("akruti/joint.hpp").
// Directly reuses pebble::math::vec2 with zero conversion overhead.
// ============================================================================
#if __has_include("akruti/joint.hpp")
#define PRAKRITI_HAS_AKRUTI_JOINT 1
#include "solver_base.hpp"
#include "akruti/joint.hpp"
#include <containers/numeric/math_vector.hpp>
#include <vector>
#include <cmath>
#include <algorithm>

namespace prakriti {

struct JointSolver {
    const std::vector<akruti::Joint>* joints{nullptr};
    JointConfig cfg{};

    explicit JointSolver(const std::vector<akruti::Joint>& js, JointConfig c = {}) noexcept
        : joints(&js), cfg(c) {}

    template <MaterialLaw Law>
    void solve(SolverContext<Law>& ctx) {
        if (!joints) return;
        auto& P = ctx.particles;
        const Scalar inv_dt2 = ctx.dt_sub > Scalar(0) ? Scalar(1) / (ctx.dt_sub * ctx.dt_sub) : Scalar(0);
        for (int it = 0; it < cfg.iterations; ++it) {
            for (const akruti::Joint& j : *joints) project(P, j, inv_dt2);
        }
    }

private:
    template <class Store>
    void project(Store& P, const akruti::Joint& j, Scalar inv_dt2) {
        const Index ia = j.body_a, ib = j.body_b;
        if (ia >= P.size() || ib >= P.size()) return;
        const Scalar wa = P.inv_mass[ia], wb = P.inv_mass[ib];
        if (wa + wb <= Scalar(0)) return;
        const Scalar alpha = (j.compliance > Scalar(0) ? j.compliance : cfg.default_compliance) * inv_dt2;

        switch (j.type) {
            case akruti::JointType::Distance:
                solve_distance(P, ia, ib, wa, wb, j.rest_length, alpha);
                break;
            case akruti::JointType::Revolute:
            case akruti::JointType::Weld:
                solve_distance(P, ia, ib, wa, wb, Scalar(0), alpha); // coincident anchors
                break;
            case akruti::JointType::Prismatic:
                solve_prismatic(P, ia, ib, wa, wb, j, alpha);
                break;
            case akruti::JointType::Motor:
                break; // angle-only; no positional projection for point masses
        }
    }

    // Distance constraint C = |a-b| - rest, gradient along separation. (xpbd.hpp pattern.)
    template <class Store>
    void solve_distance(Store& P, Index ia, Index ib, Scalar wa, Scalar wb, Scalar rest, Scalar alpha) {
        pebble::math::vec2 d = P.pred_v(ia) - P.pred_v(ib);
        const Scalar len = pebble::math::length(d);
        if (len <= Scalar(1e-9)) return;
        const pebble::math::vec2 grad = d * (Scalar(1) / len);
        const Scalar C = len - rest;
        const Scalar dlambda = -C / (wa + wb + alpha);
        const pebble::math::vec2 corr = grad * dlambda;
        P.pred_x[ia] += corr[0] * wa; P.pred_y[ia] += corr[1] * wa;
        P.pred_x[ib] -= corr[0] * wb; P.pred_y[ib] -= corr[1] * wb;
    }

    // Prismatic: constrain relative offset to lie on `axis` (kill perpendicular component),
    // then clamp along-axis coordinate to [min_limit, max_limit].
    template <class Store>
    void solve_prismatic(Store& P, Index ia, Index ib, Scalar wa, Scalar wb,
                         const akruti::Joint& j, Scalar alpha) {
        const pebble::math::vec2 axis{j.axis.x, j.axis.y};
        pebble::math::vec2 d = P.pred_v(ib) - P.pred_v(ia);
        const Scalar along = pebble::math::dot(d, axis);
        const pebble::math::vec2 along_v = axis * along;
        const pebble::math::vec2 perp = d - along_v;      // component to remove
        const Scalar C = pebble::math::length(perp);
        if (C > Scalar(1e-9)) {
            const pebble::math::vec2 grad = perp * (Scalar(1) / C);
            const Scalar dlambda = -C / (wa + wb + alpha);
            const pebble::math::vec2 corr = grad * dlambda;
            P.pred_x[ib] += corr[0] * wb; P.pred_y[ib] += corr[1] * wb;
            P.pred_x[ia] -= corr[0] * wa; P.pred_y[ia] -= corr[1] * wa;
        }
        // Limit clamp along the axis.
        const Scalar lo = static_cast<Scalar>(j.min_limit), hi = static_cast<Scalar>(j.max_limit);
        const Scalar clamped = std::clamp(along, lo, hi);
        const Scalar excess = along - clamped;
        if (std::fabs(excess) > Scalar(1e-9)) {
            const pebble::math::vec2 corr = axis * (-excess);
            const Scalar denom = wa + wb;
            P.pred_x[ib] += corr[0] * (wb / denom); P.pred_y[ib] += corr[1] * (wb / denom);
            P.pred_x[ia] -= corr[0] * (wa / denom); P.pred_y[ia] -= corr[1] * (wa / denom);
        }
    }
};

} // namespace prakriti
#endif // __has_include akruti/joint.hpp
