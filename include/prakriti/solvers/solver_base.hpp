#pragma once
// ============================================================================
// prakriti/solvers/solver_base.hpp — shared solver context + PhysicsSolver concept.
// Solvers are decoupled policies: read material-derived coefficients, mutate state columns.
// ============================================================================
#include "../core/config.hpp"
#include "../core/spatial_hash.hpp"
#include "../state/particle_store.hpp"
#include "../state/edge_store.hpp"
#include "../state/material_registry.hpp"
#include "../material/constitutive.hpp"
#include "../material/phase.hpp"
#include <concepts>

namespace prakriti {

// Everything a solver needs for one substep. Non-owning references; law is a value policy.
template <MaterialLaw Law>
struct SolverContext {
    ParticleStore&          particles;
    EdgeStore&              edges;
    const MaterialRegistry& materials;
    const SpatialHash&      grid;
    const Law&              law;
    const WorldConfig&      world;
    Scalar                  dt_sub;

    // Read a particle's current phase fractions from the SoA columns.
    [[nodiscard]] PhaseFractions phase_of(Index i) const noexcept {
        PhaseFractions pf;
        pf.f = {particles.f_solid[i], particles.f_plastic[i],
                particles.f_liquid[i], particles.f_gas[i]};
        return pf;
    }
    [[nodiscard]] const MaterialParams& params_of(Index i) const noexcept {
        return materials.view(particles.material[i]);
    }
};

// A PhysicsSolver projects/updates state for one substep, given the context.
template <class S, class Law>
concept PhysicsSolver = requires(S s, SolverContext<Law>& ctx) {
    { s.solve(ctx) };
};

} // namespace prakriti
