#pragma once
// gati/coupling.hpp — Two-Way Dynamic Coupling Bridge between Gati Rigid Bodies and Prakriti Continuum.
#include "rigid_body.hpp"
#include "akruti/gradient.hpp"
#include "prakriti/state/particle_store.hpp"
#include <span>
#include <cmath>

namespace gati {
    class DynamicCouplingBridge {
    public:
        // Pushes fluid/continuum particles out of penetrating rigid bodies and applies penalty impulse
        void apply_rigid_to_particle(std::span<RigidBody> rigids, prakriti::ParticleStore& particles) const {
            const std::size_t num_particles = particles.size();

            for (std::size_t i = 0; i < num_particles; ++i) {
                const pebble::math::vec2 p{particles.pred_x[i], particles.pred_y[i]};
                for (auto& b : rigids) {
                    const float dist = b.sdf(p);
                    if (dist < 0.0f) {
                        // Penetrating into rigid body: compute outward normal
                        const akruti::TransformedShape ts{b.shape, akruti::Vec{b.position[0], b.position[1]}, b.angle};
                        const akruti::Vec norm = akruti::sdf_gradient(ts, akruti::Vec{p[0], p[1]});

                        // Project position to boundary
                        particles.pred_x[i] -= norm.x * dist;
                        particles.pred_y[i] -= norm.y * dist;

                        // Match rigid body velocity at contact
                        const akruti::Vec r = akruti::Vec{p[0], p[1]} - akruti::Vec{b.position[0], b.position[1]};
                        const float r_vx = b.velocity[0] - b.angular_velocity * r.y;
                        const float r_vy = b.velocity[1] + b.angular_velocity * r.x;

                        particles.vel_x[i] = r_vx + norm.x * 0.1f;
                        particles.vel_y[i] = r_vy + norm.y * 0.1f;
                    }
                }
            }
        }

        // Accumulates forces from particles onto rigid bodies (buoyancy, drag, impact)
        void apply_particle_to_rigid(std::span<RigidBody> rigids, const prakriti::ParticleStore& particles) const {
            const std::size_t num_particles = particles.size();

            for (std::size_t i = 0; i < num_particles; ++i) {
                const pebble::math::vec2 p{particles.pos_x[i], particles.pos_y[i]};
                const float m = particles.inv_mass[i] > 0.0f ? 1.0f / particles.inv_mass[i] : 1.0f;

                for (auto& b : rigids) {
                    if (b.is_static()) continue;
                    const float dist = b.sdf(p);
                    if (dist < 0.1f) {
                        const akruti::TransformedShape ts{b.shape, akruti::Vec{b.position[0], b.position[1]}, b.angle};
                        const akruti::Vec norm = akruti::sdf_gradient(ts, akruti::Vec{p[0], p[1]});

                        const float force_mag = (0.1f - dist) * 100.0f * m;
                        const pebble::math::vec2 f{-norm.x * force_mag, -norm.y * force_mag};
                        b.apply_force(f);
                    }
                }
            }
        }
    };
} // namespace gati
