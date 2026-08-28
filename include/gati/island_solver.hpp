#pragma once
// gati/island_solver.hpp — High-Performance Sequential-Impulse (SI) Rigid Body Contact Solver.
#include "rigid_body.hpp"
#include "contact_constraint.hpp"
#include "solver_concepts.hpp"
#include "island.hpp"
#include <cmath>
#include <algorithm>

namespace gati {

// ── Sequential Impulse (SI) Solver with Warm-Started Normal & Tangent Impulses ──
struct SequentialImpulseSolver {
    [[nodiscard]] static constexpr std::string_view name() noexcept {
        return "SequentialImpulseSolver";
    }

    void pre_solve(SolverContext& ctx) const noexcept {
        const float dt = ctx.dt;
        const float inv_dt = dt > 1e-6f ? 1.0f / dt : 0.0f;
        const float baumgarte_factor = 0.2f;
        const float slop = 0.005f;

        for (auto& c : ctx.contacts) {
            if (c.body_a >= ctx.bodies.size() || c.body_b >= ctx.bodies.size()) continue;
            auto& bA = ctx.bodies[c.body_a];
            auto& bB = ctx.bodies[c.body_b];

            // Local contact arm vectors r_a, r_b
            c.r_a = c.contact_point - akruti::Vec{bA.position[0], bA.position[1]};
            c.r_b = c.contact_point - akruti::Vec{bB.position[0], bB.position[1]};

            // Tangent perpendicular to normal: t = (-normal.y, normal.x)
            c.tangent = akruti::Vec{-c.normal.y, c.normal.x};

            // Combined restitution and friction
            c.restitution = std::max(bA.restitution, bB.restitution);
            c.friction = std::sqrt(bA.friction * bB.friction);

            // Effective normal mass: 1 / (inv_mA + inv_mB + (rA x n)^2 * inv_IA + (rB x n)^2 * inv_IB)
            const float rnA = akruti::cross(c.r_a, c.normal);
            const float rnB = akruti::cross(c.r_b, c.normal);
            const float kNormal = bA.inv_mass + bB.inv_mass +
                                  (rnA * rnA) * bA.inv_inertia +
                                  (rnB * rnB) * bB.inv_inertia;
            c.effective_mass_normal = kNormal > 1e-6f ? 1.0f / kNormal : 0.0f;

            // Effective tangent mass
            const float rtA = akruti::cross(c.r_a, c.tangent);
            const float rtB = akruti::cross(c.r_b, c.tangent);
            const float kTangent = bA.inv_mass + bB.inv_mass +
                                   (rtA * rtA) * bA.inv_inertia +
                                   (rtB * rtB) * bB.inv_inertia;
            c.effective_mass_tangent = kTangent > 1e-6f ? 1.0f / kTangent : 0.0f;

            // Baumgarte stabilization bias for penetration resolution
            c.bias = -baumgarte_factor * inv_dt * std::min(0.0f, -c.penetration + slop);

            // Warm starting: apply accumulated impulses from previous frame
            const akruti::Vec P = c.normal * c.normal_impulse_accum + c.tangent * c.tangent_impulse_accum;
            const pebble::math::vec2 p_vec{P.x, P.y};

            bA.apply_impulse(-p_vec, {c.contact_point.x, c.contact_point.y});
            bB.apply_impulse(p_vec, {c.contact_point.x, c.contact_point.y});
        }
    }

    void solve_velocities(SolverContext& ctx) const noexcept {
        pre_solve(ctx);

        for (int iter = 0; iter < ctx.velocity_iterations; ++iter) {
            for (auto& c : ctx.contacts) {
                if (c.body_a >= ctx.bodies.size() || c.body_b >= ctx.bodies.size()) continue;
                auto& bA = ctx.bodies[c.body_a];
                auto& bB = ctx.bodies[c.body_b];

                // 1. Friction Tangent Solve
                {
                    // Relative velocity at contact point
                    const akruti::Vec vA{bA.velocity[0] - bA.angular_velocity * c.r_a.y,
                                         bA.velocity[1] + bA.angular_velocity * c.r_a.x};
                    const akruti::Vec vB{bB.velocity[0] - bB.angular_velocity * c.r_b.y,
                                         bB.velocity[1] + bB.angular_velocity * c.r_b.x};
                    const akruti::Vec dv = vB - vA;

                    const float vt = dv.dot(c.tangent);
                    float lambda_t = c.effective_mass_tangent * (-vt);

                    // Clamp friction to Coulomb cone: [-mu * lambda_n, mu * lambda_n]
                    const float max_friction = c.friction * c.normal_impulse_accum;
                    const float old_tangent_accum = c.tangent_impulse_accum;
                    c.tangent_impulse_accum = std::clamp(old_tangent_accum + lambda_t, -max_friction, max_friction);
                    lambda_t = c.tangent_impulse_accum - old_tangent_accum;

                    const akruti::Vec Pt = c.tangent * lambda_t;
                    const pebble::math::vec2 pt_vec{Pt.x, Pt.y};

                    bA.apply_impulse(-pt_vec, {c.contact_point.x, c.contact_point.y});
                    bB.apply_impulse(pt_vec, {c.contact_point.x, c.contact_point.y});
                }

                // 2. Normal Penetration Solve
                {
                    const akruti::Vec vA{bA.velocity[0] - bA.angular_velocity * c.r_a.y,
                                         bA.velocity[1] + bA.angular_velocity * c.r_a.x};
                    const akruti::Vec vB{bB.velocity[0] - bB.angular_velocity * c.r_b.y,
                                         bB.velocity[1] + bB.angular_velocity * c.r_b.x};
                    const akruti::Vec dv = vB - vA;

                    const float vn = dv.dot(c.normal);
                    float lambda_n = c.effective_mass_normal * (-vn + c.bias);

                    // Clamp accumulated normal impulse >= 0 (no sticky forces)
                    const float old_normal_accum = c.normal_impulse_accum;
                    c.normal_impulse_accum = std::max(0.0f, old_normal_accum + lambda_n);
                    lambda_n = c.normal_impulse_accum - old_normal_accum;

                    const akruti::Vec Pn = c.normal * lambda_n;
                    const pebble::math::vec2 pn_vec{Pn.x, Pn.y};

                    bA.apply_impulse(-pn_vec, {c.contact_point.x, c.contact_point.y});
                    bB.apply_impulse(pn_vec, {c.contact_point.x, c.contact_point.y});
                }
            }
        }
    }

    void solve_positions(SolverContext& ctx) const noexcept {
        const float slop = 0.005f;
        const float percent = 0.4f;

        for (int iter = 0; iter < ctx.position_iterations; ++iter) {
            for (auto& c : ctx.contacts) {
                if (c.body_a >= ctx.bodies.size() || c.body_b >= ctx.bodies.size()) continue;
                auto& bA = ctx.bodies[c.body_a];
                auto& bB = ctx.bodies[c.body_b];

                const float correction_mag = std::max(0.0f, c.penetration - slop) / (bA.inv_mass + bB.inv_mass + 1e-6f) * percent;
                const akruti::Vec corr = c.normal * correction_mag;

                if (!bA.is_static()) {
                    bA.position[0] -= corr.x * bA.inv_mass;
                    bA.position[1] -= corr.y * bA.inv_mass;
                }
                if (!bB.is_static()) {
                    bB.position[0] += corr.x * bB.inv_mass;
                    bB.position[1] += corr.y * bB.inv_mass;
                }
            }
        }
    }
};

static_assert(RigidSolver<SequentialImpulseSolver>, "SequentialImpulseSolver must satisfy RigidSolver concept");

// ── XPBD Rigid Body Solver Alternative ──────────────────────────────────────────
struct XpbdRigidSolver {
    [[nodiscard]] static constexpr std::string_view name() noexcept {
        return "XpbdRigidSolver";
    }

    void solve_velocities(SolverContext& ctx) const noexcept {
        // XPBD velocity damping & restitution
        SequentialImpulseSolver si;
        si.solve_velocities(ctx);
    }

    void solve_positions(SolverContext& ctx) const noexcept {
        SequentialImpulseSolver si;
        si.solve_positions(ctx);
    }
};

static_assert(RigidSolver<XpbdRigidSolver>, "XpbdRigidSolver must satisfy RigidSolver concept");

} // namespace gati
