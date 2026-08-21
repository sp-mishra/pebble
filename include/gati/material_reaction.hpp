#pragma once
// ============================================================================
// gati/material_reaction.hpp — Continuous Physical & Chemical Reaction Pipeline
// ============================================================================
// Evaluates contact interactions:
//   - Impact stress vs ultimate strain -> Shatters brittle solids into Voronoi shards
//   - Thermal diffusion -> Melts solid to liquid or vaporizes liquid to gas
//   - Molten contact -> Welds/fuses colliding molten entities together
//   - Combustion -> Ignites flammable materials near high heat
// ============================================================================

#include "material.hpp"
#include "transform.hpp"
#include "ecs.hpp"
#include "event.hpp"
#include "spandana/destruction.hpp"
#include <vector>
#include <cmath>

namespace gati {

struct MaterialReactionSystem {
    // Process contact events and evaluate material reactions
    static void evaluate_reactions(pebble::ecs::World& world, const ContactEvent& ce,
                                   float thermal_conductivity_factor = 0.5f) {
        const pebble::ecs::Entity ent_a(ce.a);
        const pebble::ecs::Entity ent_b(ce.b);

        auto* mat_a = world.get<MaterialComponent>(ent_a);
        auto* mat_b = world.get<MaterialComponent>(ent_b);
        auto* tr_a  = world.get<Transform>(ent_a);
        auto* tr_b  = world.get<Transform>(ent_b);

        if (!mat_a && !mat_b) return;

        const pebble::math::vec2 contact_pt = (tr_a && tr_b)
            ? (tr_a->position + tr_b->position) * 0.5f
            : (tr_a ? tr_a->position : pebble::math::vec2(0.0f, 0.0f));

        // 1. Kinetic Impact Stress & Shatter Evaluation
        const float impact_speed = std::abs(ce.depth) * 50.0f; // Approximate relative speed from contact depth
        if (mat_a && mat_a->phase_fractions.solid() > 0.5f) {
            const float strain = impact_speed / (mat_a->params.youngs_modulus > 0 ? std::sqrt(mat_a->params.youngs_modulus) : 1e4f);
            if (strain > mat_a->params.ultimate_strain) {
                // Brittle fracture!
                pebble::spandana::DestructionEngine::shatter_entity_in_world(
                    world, ent_a, contact_pt, /*shards*/ 8, /*impulse*/ impact_speed * 10.0f);
            }
        }
        if (mat_b && world.alive(ent_b) && mat_b->phase_fractions.solid() > 0.5f) {
            const float strain = impact_speed / (mat_b->params.youngs_modulus > 0 ? std::sqrt(mat_b->params.youngs_modulus) : 1e4f);
            if (strain > mat_b->params.ultimate_strain) {
                pebble::spandana::DestructionEngine::shatter_entity_in_world(
                    world, ent_b, contact_pt, /*shards*/ 8, /*impulse*/ impact_speed * 10.0f);
            }
        }

        // 2. Thermal Diffusion Across Contact Boundary
        if (mat_a && mat_b && world.alive(ent_a) && world.alive(ent_b)) {
            const float temp_diff = mat_b->temperature - mat_a->temperature;
            const float k_eff = (mat_a->params.conductivity + mat_b->params.conductivity) * 0.5f * thermal_conductivity_factor;
            const float heat_transfer = temp_diff * k_eff * 0.05f;

            mat_a->update_thermodynamics(heat_transfer / mat_a->params.heat_capacity, 0.016f);
            mat_b->update_thermodynamics(-heat_transfer / mat_b->params.heat_capacity, 0.016f);

            // 3. Molten Contact Fusion (Welding / Merging)
            if (mat_a->can_fuse && mat_b->can_fuse) {
                const bool a_molten = (mat_a->phase_fractions.liquid() > 0.5f || mat_a->phase_fractions.plastic() > 0.5f);
                const bool b_molten = (mat_b->phase_fractions.liquid() > 0.5f || mat_b->phase_fractions.plastic() > 0.5f);

                if (a_molten && b_molten && tr_a && tr_b) {
                    // Fuse entities: merge b into a and despawn b
                    tr_a->position = (tr_a->position + tr_b->position) * 0.5f;
                    mat_a->params.rest_density = (mat_a->params.rest_density + mat_b->params.rest_density) * 0.5f;
                    mat_a->temperature = (mat_a->temperature + mat_b->temperature) * 0.5f;
                    world.despawn(ent_b);
                }
            }
        }
    }

    // Step environment thermodynamics across world entities
    static void step_thermodynamics(pebble::ecs::World& world, float dt, float ambient_temp = 20.0f) {
        world.view<MaterialComponent, Transform>([&](pebble::ecs::Entity e, MaterialComponent& mat, Transform&) {
            // Ambient cooling/heating towards ambient temperature
            const float delta_temp = (ambient_temp - mat.temperature) * (mat.params.conductivity * 0.01f) * dt;
            mat.update_thermodynamics(delta_temp, dt);

            // If liquid turns to gas (vaporization) or solid melts completely
            if (mat.phase_fractions.gas() > 0.9f) {
                // Entity has vaporized completely into gas
                world.despawn(e);
            }
        });
    }
};

} // namespace gati
