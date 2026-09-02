#pragma once
// gati/solver_concepts.hpp — Solver, Island Strategy, and Continuum Coupling Concepts for Gati.
#include "rigid_body.hpp"
#include "contact_constraint.hpp"
#include "island.hpp"
#include <concepts>
#include <span>
#include <string_view>
#include <cstdint>

namespace prakriti {
    class ParticleStore;
}

namespace gati {
    struct SolverContext {
        std::span<RigidBody> bodies;
        std::span<ContactConstraint> contacts;
        float dt{1.0f / 60.0f};
        int velocity_iterations{8};
        int position_iterations{3};
    };

    // ── RigidSolver: the constraint solver for rigid body contacts + joints ─
    template <class T>
    concept RigidSolver = requires(T solver, SolverContext& ctx) {
        solver.solve_velocities(ctx);
        solver.solve_positions(ctx);
        { solver.name() } -> std::convertible_to<std::string_view>;
    };

    // ── CouplingStrategy: how rigid bodies interact with continuum ──────────
    template <class T, class ParticleStoreType = prakriti::ParticleStore>
    concept CouplingStrategy = requires(T cs, std::span<RigidBody> rigids,
                                        ParticleStoreType& particles) {
        cs.apply_rigid_to_particle(rigids, particles);
        cs.apply_particle_to_rigid(rigids, particles);
    };

    // ── IslandStrategy: how bodies are grouped for solver dispatch ──────────
    template <class T>
    concept IslandStrategy = requires(T is, std::span<const ContactConstraint> contacts,
                                      std::span<RigidBody> bodies, IslandSet& iset) {
        { is.build(contacts, bodies) } -> std::convertible_to<IslandSet>;
        is.wake(uint32_t{}, bodies);
        is.try_sleep(iset, bodies, float{});
    };
} // namespace gati
