#pragma once
// gati/simulation.hpp — Unified 2D Multi-Body Physics Simulation Facade.
//
// 9-Phase Pipeline:
//   Phase 1: Broadphase (Akruti HybridBroadphase / SpatialHash)
//   Phase 2: Narrowphase (Akruti Matrix Dispatch + Manifold Cache)
//   Phase 3: Island Detection & Sleeping (Gati UnionFindIslands)
//   Phase 4: Rigid Body Solver (Gati SequentialImpulseSolver)
//   Phase 5: CCD Sweep (Akruti MprDistanceOracle)
//   Phase 6: Integrate (Gati RigidBody Semi-Implicit Euler)
//   Phase 7: Continuum Multi-Physics (Prakriti Engine)
//   Phase 8: Fracture Evaluation (Akruti Voronoi Shatter & CDT)
//   Phase 9: Commit & Events
#include "sim_config.hpp"
#include "rigid_body.hpp"
#include "contact_constraint.hpp"
#include "contact_cache.hpp"
#include "island.hpp"
#include "island_solver.hpp"
#include "adaptive_config.hpp"
#include "coupling.hpp"
#include "event.hpp"
#include "akruti/akruti.hpp"
#include "prakriti/prakriti.hpp"
#include <vector>
#include <memory>
#include <cstdint>

namespace gati {
    using BodyHandle = std::uint32_t;
    using ParticleHandle = std::uint32_t;
    using ObstacleHandle = std::uint32_t;
    using JointHandle = std::uint32_t;

    template <typename Config = DefaultSimConfig>
    class Simulation {
    public:
        using Broadphase = typename Config::BroadphaseType;
        using Narrowphase = typename Config::NarrowType;
        using Triangulator = typename Config::TriangulatorType;
        using VoronoiBuilder = typename Config::VoronoiType;
        using Decomposer = typename Config::DecomposerType;
        using DistanceOracle = typename Config::DistanceOracleType;
        using Solver = typename Config::SolverType;
        using Coupling = typename Config::CouplingType;
        using IslandStrategyType = typename Config::IslandType;

        Simulation() {
            prakriti_world_ = std::make_unique<prakriti::World<>>();
        }

        // === Rigid Bodies ===
        template <akruti::Shape S>
        BodyHandle add_rigid(const S& shape, RigidBodyDesc desc = {}) {
            const BodyHandle h = static_cast<BodyHandle>(bodies_.size());
            bodies_.emplace_back(shape, desc);
            const auto box = bodies_.back().aabb();
            broadphase_.insert(box, h);
            return h;
        }

        template <class T>
        BodyHandle add_body(const T& shape, RigidBodyDesc desc = {}) {
            return add_rigid(shape, desc);
        }

        void remove_rigid(BodyHandle h) {
            if (h < bodies_.size()) {
                broadphase_.remove(h);
                bodies_[h].mass = 0.0f;
                bodies_[h].inv_mass = 0.0f;
            }
        }

        [[nodiscard]] RigidBody& get_body(BodyHandle h) { return bodies_[h]; }
        [[nodiscard]] const RigidBody& get_body(BodyHandle h) const { return bodies_[h]; }
        [[nodiscard]] std::size_t body_count() const noexcept { return bodies_.size(); }

        // === Continuum Particles ===
        void add_fluid_region(akruti::AABB<float> region, prakriti::MaterialId mat, int count) {
            if (!prakriti_world_ || count <= 0) return;
            const float dx = (region.hi[0] - region.lo[0]) / std::sqrt(float(count));
            const float dy = (region.hi[1] - region.lo[1]) / std::sqrt(float(count));
            for (float y = region.lo[1]; y < region.hi[1]; y += dy) {
                for (float x = region.lo[0]; x < region.hi[0]; x += dx) {
                    prakriti_world_->particles().add({.position = pebble::math::vec2{x, y}, .material = mat});
                }
            }
        }

        [[nodiscard]] prakriti::World<>& prakriti() noexcept { return *prakriti_world_; }

        // === Step Pipeline ===
        void step(float dt) {
            stats_ = gather_stats();
            adaptive_.update(stats_);

            const float dt_sub = dt / float(substeps_);

            for (int sub = 0; sub < substeps_; ++sub) {
                // PHASE 1: Broadphase
                refit_broadphase();
                std::vector<std::pair<std::uint32_t, std::uint32_t>> pairs;
                for (std::uint32_t i = 0; i < bodies_.size(); ++i) {
                    if (bodies_[i].is_sleeping && bodies_[i].is_static()) continue;
                    const auto box = bodies_[i].aabb();
                    broadphase_.query(box, [&](std::uint32_t other) {
                        if (other > i) {
                            pairs.emplace_back(i, other);
                        }
                    });
                }

                // PHASE 2: Narrowphase & Contact Generation
                contacts_.clear();
                for (const auto& [a, b] : pairs) {
                    const auto& bA = bodies_[a];
                    const auto& bB = bodies_[b];

                    if (bA.is_static() && bB.is_static()) continue;
                    if (bA.is_sleeping && bB.is_sleeping) continue;

                    // Check manifold cache for resting contact reuse
                    auto cached = contact_cache_.find(a, b);
                    akruti::Manifold m;

                    if (cached.has_value() &&
                        pebble::math::length_sq(bA.position - cached->pos_a) < 1e-6f &&
                        pebble::math::length_sq(bB.position - cached->pos_b) < 1e-6f &&
                        std::fabs(bA.angle - cached->angle_a) < 1e-4f &&
                        std::fabs(bB.angle - cached->angle_b) < 1e-4f) {
                        m = cached->manifold;
                    }
                    else {
                        const akruti::TransformedShape tsA{
                            bA.shape, akruti::Vec{bA.position[0], bA.position[1]}, bA.angle
                        };
                        const akruti::TransformedShape tsB{
                            bB.shape, akruti::Vec{bB.position[0], bB.position[1]}, bB.angle
                        };

                        m = akruti::collide_matrix(bA.shape.type, bA.shape.data(),
                                                   bB.shape.type, bB.shape.data(), nullptr);

                        contact_cache_.store(a, b, m, bA.position, bB.position, bA.angle, bB.angle);
                    }

                    if (m.hit) {
                        ContactConstraint c;
                        c.body_a = a;
                        c.body_b = b;
                        c.normal = m.normal;
                        c.penetration = m.depth;
                        c.contact_point = m.points.empty() ? akruti::Vec{0, 0} : m.points[0].point;

                        if (cached.has_value()) {
                            c.normal_impulse_accum = cached->normal_impulse;
                            c.tangent_impulse_accum = cached->tangent_impulse;
                        }
                        contacts_.push_back(c);
                    }
                }

                // PHASE 3: Island Detection & Sleeping
                auto islands = island_strategy_.build(contacts_, bodies_);
                island_strategy_.try_sleep(islands, bodies_, adaptive_.sleep_threshold);

                // PHASE 4: Rigid Solver
                SolverContext ctx{bodies_, contacts_, dt_sub, adaptive_.velocity_iters, adaptive_.position_iters};
                solver_.solve_velocities(ctx);
                solver_.solve_positions(ctx);

                // Update contact cache with latest accumulated impulses for warm-starting
                for (const auto& c : contacts_) {
                    contact_cache_.update_impulses(c.body_a, c.body_b, c.normal_impulse_accum, c.tangent_impulse_accum);
                }

                // PHASE 5: CCD Sweep for anti-tunneling
                if (adaptive_.enable_ccd) {
                    resolve_ccd(dt_sub);
                }

                // PHASE 6: Integrate
                for (auto& b : bodies_) {
                    b.step(dt_sub, {0.0f, gravity_});
                }

                // PHASE 7: Continuum Multi-Physics Coupling
                if (prakriti_world_ && prakriti_world_->particles().size() > 0) {
                    coupling_.apply_rigid_to_particle(bodies_, prakriti_world_->particles());
                    prakriti_world_->step();
                    coupling_.apply_particle_to_rigid(bodies_, prakriti_world_->particles());
                }

                // Age cache
                contact_cache_.tick();
            }
        }

        void set_gravity(float g) noexcept { gravity_ = g; }
        void set_substeps(int s) noexcept { substeps_ = std::max(1, s); }

    private:
        void refit_broadphase() {
            for (std::uint32_t i = 0; i < bodies_.size(); ++i) {
                if (!bodies_[i].is_sleeping) {
                    broadphase_.update(i, bodies_[i].aabb());
                }
            }
        }

        void resolve_ccd(float dt_sub) {
            for (auto& b : bodies_) {
                if (b.is_static() || b.is_sleeping) continue;
                const pebble::math::vec2 motion = b.velocity * dt_sub;
                if (pebble::math::length_sq(motion) > 1.0f) {
                    // High-speed body: conservative advancement
                    for (const auto& other : bodies_) {
                        if (&other == &b) continue;
                        const akruti::TransformedShape tsA{b.shape, akruti::Vec{b.position[0], b.position[1]}, b.angle};
                        const akruti::TransformedShape tsB{
                            other.shape, akruti::Vec{other.position[0], other.position[1]}, other.angle
                        };
                        const auto toi = akruti::time_of_impact(tsB, tsA, akruti::Vec{motion[0], motion[1]});
                        if (toi.hit && toi.t < 1.0f) {
                            b.position = b.position + motion * toi.t;
                            b.velocity = b.velocity * 0.5f;
                        }
                    }
                }
            }
        }

        SceneStats gather_stats() const {
            SceneStats s;
            s.rigid_count = bodies_.size();
            s.particle_count = prakriti_world_ ? prakriti_world_->particles().size() : 0;
            s.contact_count = contacts_.size();
            for (const auto& b : bodies_) {
                if (b.is_sleeping) ++s.sleeping_count;
            }
            return s;
        }

        std::vector<RigidBody> bodies_;
        std::vector<ContactConstraint> contacts_;

        Broadphase broadphase_{};
        Narrowphase narrowphase_{};
        Solver solver_{};
        Coupling coupling_{};
        IslandStrategyType island_strategy_{};

        ContactCache contact_cache_{2048};
        AdaptiveConfig adaptive_{};
        SceneStats stats_{};

        std::unique_ptr<prakriti::World<>> prakriti_world_;

        float gravity_{-9.81f};
        int substeps_{1};
    };
} // namespace gati
