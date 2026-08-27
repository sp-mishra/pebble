#pragma once
// ============================================================================
// prakriti/solvers/density.hpp — Position-Based Fluids density projection (Macklin/Müller 2013).
// Applies to liquid-dominant particles. Density constraint C_i = ρ_i/ρ0 − 1 solved via
// per-particle Lagrange multiplier λ_i with anti-clustering scorr; EOS pressure recorded for
// diagnostics/buoyancy.
// ============================================================================
#include "solver_base.hpp"
#include "kernels.hpp"
#include "../material/eos.hpp"
#include <containers/numeric/math_vector.hpp>
#include <vector>
#include <cmath>

namespace prakriti {

struct DensitySolver {
    FluidConfig cfg{};

    template <MaterialLaw Law>
    void solve(SolverContext<Law>& ctx) {
        auto& P = ctx.particles;
        const Index n = P.size();
        const Scalar h = cfg.smoothing_h;

        if (lambda_.size() < n) {
            lambda_.resize(n);
            dp_.resize(n);
        }
        std::fill(lambda_.begin(), lambda_.begin() + n, Scalar(0));

        // 1. Density + Lagrange Multiplier Evaluation
        for (Index i = 0; i < n; ++i) {
            if (P.f_liquid[i] < Scalar(0.5)) continue;
            Scalar rho = Scalar(0);
            Scalar sum_grad2 = Scalar(0);
            pebble::math::vec2 grad_i{0.0f, 0.0f};
            const pebble::math::vec2 pi_v = P.pred_v(i);

            ctx.grid.for_each_neighbor(P.pred_x[i], P.pred_y[i], h, [&](Index j, Scalar) {
                const pebble::math::vec2 pj_v = P.pred_v(j);
                const Scalar r2 = pebble::math::length_sq(pi_v - pj_v);
                rho += cfg.particle_mass * kernels::poly6(r2, h);
                const pebble::math::vec2 g = kernels::spiky_grad(pi_v - pj_v, h)
                                             * (Scalar(1) / cfg.rest_density);
                grad_i = grad_i + g;
                if (j != i) sum_grad2 += pebble::math::length_sq(g);
            });
            P.density[i] = rho;
            const Scalar C = std::max(Scalar(0), rho / cfg.rest_density - Scalar(1));
            sum_grad2 += pebble::math::length_sq(grad_i);
            lambda_[i] = C > Scalar(0) ? -C / (sum_grad2 + cfg.relaxation_eps) : Scalar(0);

            const MaterialParams& p = ctx.params_of(i);
            P.pressure[i] = tait_pressure(rho, cfg.rest_density, p, P.f_gas[i],
                                          P.temperature[i], ctx.world.clamp_negative_pressure);
        }

        // 2. Position Delta Evaluation with 4-Color Checkerboard Independence
        const Scalar dq2 = (cfg.scorr_dq * h) * (cfg.scorr_dq * h);
        const Scalar wdq = kernels::poly6(dq2, h);
        const Scalar inv_wdq = wdq > Scalar(0) ? Scalar(1) / wdq : Scalar(0);

        for (std::uint32_t color = 0; color < 4; ++color) {
            for (Index i = 0; i < n; ++i) {
                if (P.f_liquid[i] < Scalar(0.5)) continue;
                if (ctx.grid.particle_color(i) != color) continue;

                pebble::math::vec2 dpi{0.0f, 0.0f};
                pebble::math::vec2 color_grad{0.0f, 0.0f};
                const pebble::math::vec2 pi_v = P.pred_v(i);
                const MaterialId mi = P.material[i];

                ctx.grid.for_each_neighbor(P.pred_x[i], P.pred_y[i], h, [&](Index j, Scalar) {
                    if (j == i) return;
                    const pebble::math::vec2 pj_v = P.pred_v(j);
                    const Scalar r2 = pebble::math::length_sq(pi_v - pj_v);
                    Scalar scorr = Scalar(0);
                    if (inv_wdq > Scalar(0)) {
                        const Scalar ratio = kernels::poly6(r2, h) * inv_wdq;
                        // Fast integer power unroll: scorr_n is typically 4
                        if (cfg.scorr_n == 4) {
                            const Scalar r2_val = ratio * ratio;
                            scorr = -cfg.scorr_k * (r2_val * r2_val);
                        } else if (cfg.scorr_n == 2) {
                            scorr = -cfg.scorr_k * (ratio * ratio);
                        } else {
                            Scalar p = ratio;
                            for (int k = 1; k < cfg.scorr_n; ++k) p *= ratio;
                            scorr = -cfg.scorr_k * p;
                        }
                    }
                    const pebble::math::vec2 g = kernels::spiky_grad(pi_v - pj_v, h);
                    const Scalar sum_lambda = lambda_[i] + lambda_[j];
                    if (sum_lambda < Scalar(0)) {
                        const Scalar scale = (sum_lambda + scorr) / cfg.rest_density;
                        dpi = dpi + g * scale;
                    }

                    // Multiphase Interfacial Tension: Repel different fluid materials across boundary
                    if (P.material[j] != mi && P.f_liquid[j] > Scalar(0.3)) {
                        color_grad = color_grad + g * (cfg.particle_mass / std::max(Scalar(1), P.density[j]));
                    }
                });

                // Apply interfacial surface tension along color field normal
                const Scalar cg_len_sq = pebble::math::length_sq(color_grad);
                if (cg_len_sq > Scalar(1e-6)) {
                    const Scalar cg_len = std::sqrt(cg_len_sq);
                    const pebble::math::vec2 n_inter = color_grad * (Scalar(1) / cg_len);
                    const Scalar tension = std::min(cg_len * Scalar(0.01) * h, Scalar(0.02) * h);
                    dpi = dpi - n_inter * tension;
                }

                // Numerical stability: Clamp maximum position delta per solver iteration to prevent geyser pops
                const Scalar dp_len_sq = pebble::math::length_sq(dpi);
                const Scalar max_dp = Scalar(0.12) * h; // Gentle physical separation limit (~1.9px at h=16)
                if (dp_len_sq > max_dp * max_dp) {
                    dpi = dpi * (max_dp / std::sqrt(dp_len_sq));
                }

                dp_[i] = dpi;
            }
        }

        // 3. Apply position corrections (skip static particles).
        for (Index i = 0; i < n; ++i) {
            if (P.f_liquid[i] < Scalar(0.5) || P.is_static(i)) continue;
            P.pred_x[i] += dp_[i][0];
            P.pred_y[i] += dp_[i][1];
        }

        // 4. XSPH Artificial Viscosity & 2D Vorticity Confinement (Macklin & Müller 2013)
        constexpr Scalar c_xsph = Scalar(0.08);
        constexpr Scalar c_vort = Scalar(0.015);
        for (Index i = 0; i < n; ++i) {
            if (P.f_liquid[i] < Scalar(0.5) || P.is_static(i)) continue;
            pebble::math::vec2 vi = P.vel_v(i);
            pebble::math::vec2 v_smooth{0.0f, 0.0f};
            Scalar omega_i = Scalar(0);
            Scalar w_sum = Scalar(0);
            const pebble::math::vec2 pi_v = P.pred_v(i);

            ctx.grid.for_each_neighbor(P.pred_x[i], P.pred_y[i], h, [&](Index j, Scalar) {
                if (j == i) return;
                const pebble::math::vec2 pj_v = P.pred_v(j);
                const Scalar r2 = pebble::math::length_sq(pi_v - pj_v);
                const Scalar w = kernels::poly6(r2, h);
                const pebble::math::vec2 dv = P.vel_v(j) - vi;
                v_smooth = v_smooth + dv * w;
                const pebble::math::vec2 g = kernels::spiky_grad(pi_v - pj_v, h);
                omega_i += (dv[1] * g[0] - dv[0] * g[1]);
                w_sum += w;
            });
            if (w_sum > Scalar(1e-6)) {
                const Scalar inv_rest = (cfg.rest_density > Scalar(0) ? Scalar(1) / cfg.rest_density : Scalar(1));
                vi = vi + v_smooth * (c_xsph * inv_rest);
                if (std::abs(omega_i) > Scalar(1e-5)) {
                    const Scalar s = c_vort * omega_i * ctx.dt_sub;
                    const Scalar vx_old = vi[0];
                    vi[0] -= vi[1] * s;
                    vi[1] += vx_old * s;
                }
                P.vel_x[i] = vi[0];
                P.vel_y[i] = vi[1];
            }
        }
    }

private:
    std::vector<Scalar>             lambda_;
    std::vector<pebble::math::vec2> dp_;
};

} // namespace prakriti
