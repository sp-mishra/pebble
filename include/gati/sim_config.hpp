#pragma once
// gati/sim_config.hpp — Unified SimConfig Policy Composition with with_* overrides.
#include "solver_concepts.hpp"
#include "island_solver.hpp"
#include "island.hpp"
#include "adaptive_config.hpp"
#include "akruti/broadphase_concepts.hpp"
#include "akruti/auto_policies.hpp"
#include "akruti/narrowphase.hpp"
#include "akruti/mpr.hpp"
#include "prakriti/engine.hpp"

namespace gati {
    // Default Coupling Strategy
    struct BoundaryCoupling {
        template <class ParticleStoreType>
        void apply_rigid_to_particle(std::span<RigidBody> rigids, ParticleStoreType& particles) const noexcept {
            // Evaluate boundary pressure from rigids onto particles
        }

        template <class ParticleStoreType>
        void apply_particle_to_rigid(std::span<RigidBody> rigids, ParticleStoreType& particles) const noexcept {
            // Accumulate buoyancy and drag forces from fluid particles onto rigid bodies
        }
    };

    struct ParticlePenaltyCoupling {
        template <class ParticleStoreType>
        void apply_rigid_to_particle(std::span<RigidBody> rigids, ParticleStoreType& particles) const noexcept {}

        template <class ParticleStoreType>
        void apply_particle_to_rigid(std::span<RigidBody> rigids, ParticleStoreType& particles) const noexcept {}
    };

    template <
        // ── Akruti geometry policies ──────────────────
        akruti::Broadphase BroadphaseT = akruti::HybridBroadphase,
        akruti::NarrowphaseAlgo NarrowT = akruti::AnalyticMatrixNarrow,
        akruti::Triangulator TriT = akruti::AutoTriangulator,
        akruti::VoronoiBuilder VoroT = akruti::AutoVoronoiBuilder,
        akruti::ConvexDecomposer DecompT = akruti::khanda::TriangleMergeDecomposer,
        akruti::DistanceOracle DistT = akruti::MprDistanceOracle,
        // ── Gati rigid body policies ──────────────────
        RigidSolver SolverT = SequentialImpulseSolver,
        CouplingStrategy CoupleT = BoundaryCoupling,
        IslandStrategy IslandT = UnionFindIslands,
        // ── Prakriti continuum policies ────────────────
        typename ContinuumT = prakriti::DefaultMechanicsStack,
        prakriti::ComputeBackend ComputeT = prakriti::DefaultComputeBackend
    >
    struct SimConfig {
        using BroadphaseType = BroadphaseT;
        using NarrowType = NarrowT;
        using TriangulatorType = TriT;
        using VoronoiType = VoroT;
        using DecomposerType = DecompT;
        using DistanceOracleType = DistT;

        using SolverType = SolverT;
        using CouplingType = CoupleT;
        using IslandType = IslandT;
        using ContinuumStackType = ContinuumT;
        using ComputeBackendType = ComputeT;

        template <akruti::Broadphase B>
        using with_broadphase = SimConfig<B, NarrowT, TriT, VoroT, DecompT, DistT,
                                          SolverT, CoupleT, IslandT, ContinuumT, ComputeT>;
        template <RigidSolver S>
        using with_solver = SimConfig<BroadphaseT, NarrowT, TriT, VoroT, DecompT, DistT,
                                      S, CoupleT, IslandT, ContinuumT, ComputeT>;
        template <akruti::NarrowphaseAlgo N>
        using with_narrow = SimConfig<BroadphaseT, N, TriT, VoroT, DecompT, DistT,
                                      SolverT, CoupleT, IslandT, ContinuumT, ComputeT>;
    };

    using DefaultSimConfig = SimConfig<>;
} // namespace gati
