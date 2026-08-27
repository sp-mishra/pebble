#pragma once
// ============================================================================
// prakriti/solvers/xpbd.hpp — compliant distance-constraint projection (Macklin XPBD).
// Operates on the structural edge graph. Compliance α̃ = α_eff/dt²; damage inflates it.
//   Δλ = (−C − α̃λ)/(w_a + w_b + α̃),  Δx_a = +w_a ∇C Δλ,  Δx_b = −w_b ∇C Δλ
// ============================================================================
#include "solver_base.hpp"
#include <containers/numeric/math_vector.hpp>
#include <containers/cache/kosha.hpp>
#include <vector>
#include <cstdint>

namespace prakriti {

struct XpbdSolver {
    XpbdSolver() = default;

    // Custom copy constructor: copies lambda_ scratch capacity and constructs a fresh warm_start_cache_
    XpbdSolver(const XpbdSolver& o) : lambda_(o.lambda_), warm_start_cache_{4096} {}
    XpbdSolver& operator=(const XpbdSolver& o) {
        if (this != &o) {
            lambda_ = o.lambda_;
            warm_start_cache_.clear();
        }
        return *this;
    }

    XpbdSolver(XpbdSolver&&) noexcept = default;
    XpbdSolver& operator=(XpbdSolver&&) noexcept = default;

    template <MaterialLaw Law>
    void solve(SolverContext<Law>& ctx) {
        auto& P = ctx.particles;
        auto& E = ctx.edges;
        const Index m = E.size();
        const Scalar inv_dt2 = ctx.dt_sub > Scalar(0)
                             ? Scalar(1) / (ctx.dt_sub * ctx.dt_sub) : Scalar(0);

        if (lambda_.size() < m) lambda_.resize(m);

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

            // Warm-start Lagrange multiplier from active manifold cache
            const std::uint64_t min_i = std::min<std::uint64_t>(ia, ib);
            const std::uint64_t max_i = std::max<std::uint64_t>(ia, ib);
            const std::uint64_t edge_key = (min_i << 32) | max_i;
            Scalar prev_lambda = Scalar(0);
            if (auto cached = warm_start_cache_.get(edge_key)) {
                prev_lambda = *cached * Scalar(0.85); // Warm-start relaxation factor
            }

            const Scalar denom = wa + wb + alpha;
            const Scalar dlambda = (-C - alpha * prev_lambda) / denom;
            const Scalar final_lambda = prev_lambda + dlambda;
            lambda_[e] = final_lambda;
            (void)warm_start_cache_.put(edge_key, final_lambda);

            const pebble::math::vec2 corr = grad * dlambda;
            P.pred_x[ia] += corr[0] * wa; P.pred_y[ia] += corr[1] * wa;
            P.pred_x[ib] -= corr[0] * wb; P.pred_y[ib] -= corr[1] * wb;
            E.strain[e] = C / (E.rest_len[e] > Scalar(0) ? E.rest_len[e] : Scalar(1));
        }
    }

private:
    std::vector<Scalar> lambda_;
    kosha::LRUCache<std::uint64_t, Scalar> warm_start_cache_{4096};
};

} // namespace prakriti
