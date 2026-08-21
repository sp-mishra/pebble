#pragma once
// ============================================================================
// prakriti/solvers/xpbd.hpp — compliant distance-constraint projection (Macklin XPBD).
// Operates on the structural edge graph. Compliance α̃ = α_eff/dt²; damage inflates it.
//   Δλ = (−C − α̃λ)/(w_a + w_b + α̃),  Δx_a = +w_a ∇C Δλ,  Δx_b = −w_b ∇C Δλ
// ============================================================================
#include "solver_base.hpp"
#include <containers/numeric/math_vector.hpp>
#include <vector>

namespace prakriti {

struct XpbdSolver {
    template <MaterialLaw Law>
    void solve(SolverContext<Law>& ctx) {
        auto& P = ctx.particles;
        auto& E = ctx.edges;
        const Index m = E.size();
        const Scalar inv_dt2 = ctx.dt_sub > Scalar(0)
                             ? Scalar(1) / (ctx.dt_sub * ctx.dt_sub) : Scalar(0);

        lambda_.assign(m, Scalar(0)); // reset multipliers each substep

        for (Index e = 0; e < m; ++e) {
            if (!E.is_active(e)) continue;
            const Index ia = E.a[e], ib = E.b[e];
            const Scalar wa = P.inv_mass[ia], wb = P.inv_mass[ib];
            if (wa + wb <= Scalar(0)) continue; // both static

            pebble::math::vec2 d = P.pred_v(ia) - P.pred_v(ib);
            const Scalar len = pebble::math::length(d);
            if (len <= Scalar(1e-9)) continue;
            const pebble::math::vec2 grad = d * (Scalar(1) / len); // ∇C w.r.t. a
            const Scalar C = len - E.rest_len[e];

            // Effective compliance from material law, inflated by edge damage.
            const MaterialParams& pa = ctx.params_of(ia);
            const Scalar base_alpha = ctx.law.effective_compliance(pa, ctx.phase_of(ia));
            const Scalar alpha = ctx.law.structural_alpha(base_alpha, E.damage[e]) * inv_dt2;

            const Scalar denom = wa + wb + alpha;
            const Scalar dlambda = (-C - alpha * lambda_[e]) / denom;
            lambda_[e] += dlambda;

            const pebble::math::vec2 corr = grad * dlambda;
            P.pred_x[ia] += corr[0] * wa; P.pred_y[ia] += corr[1] * wa;
            P.pred_x[ib] -= corr[0] * wb; P.pred_y[ib] -= corr[1] * wb;
            E.strain[e] = C / (E.rest_len[e] > Scalar(0) ? E.rest_len[e] : Scalar(1));
        }
    }

private:
    std::vector<Scalar> lambda_;
};

} // namespace prakriti
