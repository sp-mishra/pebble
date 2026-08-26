#pragma once
// ============================================================================
// prakriti/solvers/thermal.hpp — heat diffusion pass over the neighbor graph.
// Explicit graph Laplacian: ΔT_i = (k/c)·Σ_j w_ij (T_j − T_i)·dt, with latent-heat plateaus.
// Recomputes phase fractions from the updated temperature.
// ============================================================================
#include "solver_base.hpp"
#include "kernels.hpp"
#include <containers/numeric/math_vector.hpp>
#include <vector>

namespace prakriti {

struct ThermalSolver {
    ThermalConfig cfg{};

    template <MaterialLaw Law>
    void solve(SolverContext<Law>& ctx) {
        if (!cfg.enabled) return;
        auto& P = ctx.particles;
        const Index n = P.size();
        const Scalar h = ctx.grid.cell_size();

        delta_.assign(n, Scalar(0));

        // Accumulate conductive exchange with neighbors (symmetric, energy-conserving form).
        for (Index i = 0; i < n; ++i) {
            const MaterialParams& pi = ctx.params_of(i);
            const Scalar ki = pi.conductivity;
            const pebble::math::vec2 pi_v = P.pred_v(i);
            ctx.grid.for_each_neighbor(P.pred_x[i], P.pred_y[i], h, [&](Index j, Scalar) {
                if (j == i) return;
                const Scalar r2 = pebble::math::length_sq(pi_v - P.pred_v(j));
                const Scalar w = kernels::poly6(r2, h);
                if (w <= Scalar(0)) return;
                delta_[i] += ki * cfg.diffusivity * w * (P.temperature[j] - P.temperature[i]);
            });
        }

        // Apply, routing energy through latent-heat plateaus before temperature moves.
        for (Index i = 0; i < n; ++i) {
            const MaterialParams& p = ctx.params_of(i);
            const Scalar c = p.heat_capacity > Scalar(0) ? p.heat_capacity : Scalar(1);
            const Scalar dE = delta_[i] * ctx.dt_sub;
            apply_energy(P, i, p, c, dE);
            // Recompute phase fractions from the (possibly updated) temperature.
            const PhaseFractions pf = phase_from_temperature(P.temperature[i], p);
            P.f_solid[i]   = pf.solid();
            P.f_plastic[i] = pf.plastic();
            P.f_liquid[i]  = pf.liquid();
            P.f_gas[i]     = pf.gas();
        }
    }

private:
    // Latent-heat buffering: near a transition, absorb energy into internal_energy until the
    // latent budget is filled, then release into temperature. Supports fusion, vaporization & sublimation.
    static void apply_energy(ParticleStore& P, Index i, const MaterialParams& p,
                             Scalar c, Scalar dE) noexcept {
        const bool near_melt = P.temperature[i] >= p.melt_temp - Scalar(1) &&
                               P.temperature[i] <= p.melt_temp + Scalar(1);
        if (near_melt && p.latent_heat_fusion > Scalar(0)) {
            P.internal_energy[i] += dE;
            if (P.internal_energy[i] >= p.latent_heat_fusion) {
                const Scalar spill = P.internal_energy[i] - p.latent_heat_fusion;
                P.internal_energy[i] = p.latent_heat_fusion;
                P.temperature[i] += spill / c;
            } else if (P.internal_energy[i] < Scalar(0)) {
                P.temperature[i] += P.internal_energy[i] / c;
                P.internal_energy[i] = Scalar(0);
            }
            return;
        }

        const bool near_boil = P.temperature[i] >= p.boil_temp - Scalar(1) &&
                               P.temperature[i] <= p.boil_temp + Scalar(1);
        if (near_boil && p.latent_heat_vapor > Scalar(0)) {
            P.internal_energy[i] += dE;
            if (P.internal_energy[i] >= p.latent_heat_vapor) {
                const Scalar spill = P.internal_energy[i] - p.latent_heat_vapor;
                P.internal_energy[i] = p.latent_heat_vapor;
                P.temperature[i] += spill / c;
            } else if (P.internal_energy[i] < Scalar(0)) {
                P.temperature[i] += P.internal_energy[i] / c;
                P.internal_energy[i] = Scalar(0);
            }
            return;
        }

        P.temperature[i] += dE / c;
    }

    std::vector<Scalar> delta_;
};

} // namespace prakriti
