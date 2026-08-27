#pragma once
// ============================================================================
// prakriti/engine.hpp — World: full material-state simulation pipeline.
// Templated on material law; solver stack composed internally with defaults for plug-and-play use.
// Energy/state diagnostics are exposed as accessors for external telemetry (e.g. NADI) to consume.
// ============================================================================
#include "core/config.hpp"
#include "core/spatial_hash.hpp"
#include "state/particle_store.hpp"
#include "state/edge_store.hpp"
#include "state/material_registry.hpp"
#include "material/constitutive.hpp"
#include "material/phase.hpp"
#include "compute/compute_backend.hpp"
#include "compute/scalar_backend.hpp"
#include "compute/highway_backend.hpp"
#include "compute/pravaha_backend.hpp"
#include "solvers/solver_base.hpp"
#include "solvers/thermal.hpp"
#include "solvers/xpbd.hpp"
#include "solvers/density.hpp"
#include "solvers/damage.hpp"
#include <tuple>
#include <utility>
#include <vector>
#include <algorithm>

namespace prakriti {

// A solver stack is any tuple-like of PhysicsSolver policies applied in order.
template <class... Solvers>
struct SolverStack {
    std::tuple<Solvers...> solvers;

    template <MaterialLaw Law>
    void run(SolverContext<Law>& ctx) {
        std::apply([&](auto&... s) { (s.solve(ctx), ...); }, solvers);
    }
};

// Default stack: mechanics (XPBD) + fluids (density) each iteration; damage runs once per substep.
using DefaultMechanicsStack = SolverStack<XpbdSolver, DensitySolver>;

#if defined(PRAKRITI_HAS_PRAVAHA_BACKEND)
using DefaultComputeBackend = PravahaBackend;
#elif defined(PRAKRITI_HAS_HIGHWAY_BACKEND)
using DefaultComputeBackend = HighwayBackend;
#else
using DefaultComputeBackend = ScalarBackend;
#endif

// The mechanics stack is a template parameter so integrators can compose extra PhysicsSolvers
// (e.g. akruti-backed ObstacleSolver / JointSolver) without touching the engine. It defaults to
// DefaultMechanicsStack, so existing World<> uses are unchanged. A custom stack is passed in at
// construction (solvers may hold references to external obstacle/joint data).
template <MaterialLaw Law = DefaultMaterialLaw, ComputeBackend CB = DefaultComputeBackend,
          class MechanicsStack = DefaultMechanicsStack>
class World {
public:
    explicit World(WorldConfig cfg = {}) : cfg_(cfg), grid_(cfg.cell_size) {}
    World(WorldConfig cfg, MechanicsStack stack) : cfg_(cfg), grid_(cfg.cell_size),
                                                   mechanics_(std::move(stack)) {}

    // ── scene construction ──────────────────────────────────────────────────
    ParticleStore&          particles() noexcept { return particles_; }
    EdgeStore&              edges()     noexcept { return edges_; }
    MaterialRegistry&       materials() noexcept { return materials_; }
    const WorldConfig&      config()    const noexcept { return cfg_; }
    WorldConfig&            config()    noexcept { return cfg_; }

    ThermalSolver& thermal() noexcept { return thermal_; }
    DamageSolver&  damage()  noexcept { return damage_; }

    // ── one frame ───────────────────────────────────────────────────────────
    void step() {
        const Scalar dt_sub = cfg_.dt / static_cast<Scalar>(cfg_.substeps);
        grid_.set_cell_size(cfg_.cell_size);
        for (int s = 0; s < cfg_.substeps; ++s) substep(dt_sub);
    }

    // ── diagnostics ─────────────────────────────────────────────────────────
    [[nodiscard]] Scalar kinetic_energy() const noexcept {
        const Index n = particles_.size();
        return compute_.kinetic_energy(
            {particles_.vel_x.data(), n},
            {particles_.vel_y.data(), n},
            {particles_.inv_mass.data(), n}
        );
    }

private:
    // Refresh the active mask (0 = static, 1 = dynamic) to match the current particle count.
    void refresh_masks(Index n) {
        active_.resize(n);
        damp_.resize(n);
        for (Index i = 0; i < n; ++i)
            active_[i] = particles_.is_static(i) ? Scalar(0) : Scalar(1);
    }

    void substep(Scalar dt_sub) {
        auto& P = particles_;
        const Index n = P.size();
        refresh_masks(n);
        const CSpan mask{active_.data(), n};

        // 1. external accumulation: vel += active * gravity * dt.
        compute_.axpy_const_masked({P.vel_x.data(), n}, mask, cfg_.gravity[0] * dt_sub);
        compute_.axpy_const_masked({P.vel_y.data(), n}, mask, cfg_.gravity[1] * dt_sub);

        // 4. predict motion: pred = pos + active * vel * dt  (static => pred = pos).
        compute_.predict({P.pred_x.data(), n}, {P.pos_x.data(), n}, mask, {P.vel_x.data(), n}, dt_sub);
        compute_.predict({P.pred_y.data(), n}, {P.pos_y.data(), n}, mask, {P.vel_y.data(), n}, dt_sub);

        // 5. build neighborhoods over predicted positions.
        grid_.build({P.pred_x.data(), n}, {P.pred_y.data(), n});

        SolverContext<Law> ctx{P, edges_, materials_, grid_, law_, cfg_, dt_sub};

        // 2. thermal + phase update (uses neighbor grid).
        thermal_.solve(ctx);

        // 6. mechanics solve loop (constraint iterations).
        for (int it = 0; it < cfg_.solver_iters; ++it) {
            mechanics_.run(ctx);
            apply_boundary(n);
        }

        // 7. damage + plasticity.
        damage_.solve(ctx);

        // 8. velocity update + viscous damping; 9. commit.
        const Scalar inv_dt = dt_sub > Scalar(0) ? Scalar(1) / dt_sub : Scalar(0);
        for (Index i = 0; i < n; ++i) {
            const Scalar mu = law_.effective_viscosity(ctx.params_of(i), ctx.phase_of(i));
            damp_[i] = active_[i] * (Scalar(1) - std::min(mu + cfg_.global_damping, Scalar(1)));
        }
        const CSpan dampc{damp_.data(), n};
        // vel = (pred - pos) * inv_dt, then vel *= damp (damp folds in the static mask => 0).
        compute_.sub_scale({P.vel_x.data(), n}, {P.pred_x.data(), n}, {P.pos_x.data(), n}, inv_dt);
        compute_.sub_scale({P.vel_y.data(), n}, {P.pred_y.data(), n}, {P.pos_y.data(), n}, inv_dt);
        compute_.mul_col({P.vel_x.data(), n}, dampc);
        compute_.mul_col({P.vel_y.data(), n}, dampc);
        // commit: pos = pred (static pred already == pos from the masked predict).
        compute_.copy({P.pos_x.data(), n}, {P.pred_x.data(), n});
        compute_.copy({P.pos_y.data(), n}, {P.pred_y.data(), n});
    }

    // Boundary containment via AABB clamp on predicted positions (anti-tunneling with substeps).
    // Static particles keep pred == pos (bounds assumed to contain them), so a uniform clamp is safe.
    void apply_boundary(Index n) {
        auto& P = particles_;
        compute_.clamp({P.pred_x.data(), n}, cfg_.bounds.lo[0], cfg_.bounds.hi[0]);
        compute_.clamp({P.pred_y.data(), n}, cfg_.bounds.lo[1], cfg_.bounds.hi[1]);
    }

    WorldConfig            cfg_;
    ParticleStore          particles_;
    EdgeStore              edges_;
    MaterialRegistry       materials_;
    SpatialHash            grid_;
    std::vector<Scalar>    active_;   // per-particle 0/1 static mask (scratch)
    std::vector<Scalar>    damp_;     // per-particle velocity damping (scratch)
    [[no_unique_address]] Law law_{};
    [[no_unique_address]] CB  compute_{};
    [[no_unique_address]] MechanicsStack mechanics_{};
    ThermalSolver          thermal_{};
    DamageSolver           damage_{};
};

} // namespace prakriti
