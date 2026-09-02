#pragma once
// ============================================================================
// prakriti/solvers/damage.hpp — strain-driven damage, plastic rest-length mutation, fracture.
//   ε = (‖x_a−x_b‖ − L0)/L0
//   ΔD = ((ε−ε_yield)/ε_ultimate)^β  if ε > ε_yield else 0
//   D ≥ 1  ⇒  edge fractures (deactivated)
// Plastic flow mutates L0 toward the current length when the plastic phase dominates.
// Directly reuses containers::union_find for connected-component tracking.
// ============================================================================
#include "solver_base.hpp"
#include "containers/union_find.hpp"
#include <containers/numeric/math_vector.hpp>
#include <cmath>
#include <algorithm>
#include <vector>

namespace prakriti {
    struct DamageSolver {
        Scalar plastic_rate = Scalar(0.5); // fraction of over-yield stretch baked into L0 per substep
        bool enable_fracture = true;
        bool track_islands = false; // additive: rebuild connected-component ids after fracture

        template <MaterialLaw Law>
        void solve(SolverContext<Law>& ctx) {
            auto& P = ctx.particles;
            auto& E = ctx.edges;
            const Index m = E.size();
            bool any_fractured = false;

            for (Index e = 0; e < m; ++e) {
                if (!E.is_active(e)) continue;
                const Index ia = E.a[e], ib = E.b[e];
                const Scalar L0 = E.rest_len[e];
                if (L0 <= Scalar(0)) continue;

                const Scalar len = pebble::math::distance(P.pred_v(ia), P.pred_v(ib));
                const Scalar eps = (len - L0) / L0;
                E.strain[e] = eps;

                const MaterialParams& p = ctx.params_of(ia);
                if (eps > p.yield_strain) {
                    const Scalar span = std::max(p.ultimate_strain, Scalar(1e-6));
                    const Scalar norm = (eps - p.yield_strain) / span;
                    const Scalar dD = std::pow(std::max(norm, Scalar(0)), p.damage_exponent);
                    E.damage[e] = std::min(E.damage[e] + dD, Scalar(1));

                    // Plastic flow: when plastic fraction dominates, permanently lengthen L0.
                    const Scalar plastic_share = Scalar(0.5) *
                        (P.f_plastic[ia] + P.f_plastic[ib]);
                    if (plastic_share > Scalar(0.5)) {
                        E.rest_len[e] = L0 + plastic_rate * (len - L0);
                    }

                    if (enable_fracture && E.damage[e] >= Scalar(1)) {
                        E.deactivate(e);
                        any_fractured = true;
                        // Propagate damage to endpoint particles for visualization/coupling.
                        P.damage[ia] = std::min(P.damage[ia] + Scalar(0.5), Scalar(1));
                        P.damage[ib] = std::min(P.damage[ib] + Scalar(0.5), Scalar(1));
                    }
                }
            }
            if (any_fractured) {
                E.compact();
                if (track_islands) rebuild_islands(P, E);
            }
        }

        // Island id of a particle after the last rebuild_islands() (kInvalidIndex if untracked).
        // Two particles share an id iff they are connected through the active edge graph — i.e. they
        // belong to the same fractured chunk. Chunks can then be treated as separate rigid bodies.
        [[nodiscard]] Index island_of(Index particle) const noexcept {
            return particle < island_.size() ? island_[particle] : kInvalidIndex;
        }

        [[nodiscard]] Index island_count() const noexcept { return island_count_; }

        // Connected components over active edges via union_find; compress roots to dense [0,k) ids.
        // Public so integrators can rebuild islands after manual topology edits (not just fracture).
        void rebuild_islands(const ParticleStore& P, const EdgeStore& E) {
            const Index n = P.size();
            containers::union_find<Index> uf;
            uf.reserve(n);
            for (Index i = 0; i < n; ++i) uf.make_set();
            const Index m = E.size();
            for (Index e = 0; e < m; ++e) {
                if (E.is_active(e)) uf.unite(E.a[e], E.b[e]);
            }
            island_.assign(n, kInvalidIndex);
            std::vector<Index> root_to_id(n, kInvalidIndex);
            island_count_ = 0;
            for (Index i = 0; i < n; ++i) {
                const Index r = uf.find(i);
                if (root_to_id[r] == kInvalidIndex) root_to_id[r] = island_count_++;
                island_[i] = root_to_id[r];
            }
        }

    private:
        std::vector<Index> island_;
        Index island_count_{0};
    };
} // namespace prakriti
