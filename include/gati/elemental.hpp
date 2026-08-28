#pragma once
// ============================================================================
// gati/elemental.hpp — Discrete & Continuous Elemental & Chemical Reaction Matrix
// ============================================================================
// Automatically resolves elemental contacts (Water, Lava, Fire, Wood, Metal,
// Acid, Electricity) into phase changes, combustion, obsidian synthesis, and gas bursts.
// ============================================================================

#include "material.hpp"
#include "transform.hpp"
#include "ecs.hpp"
#include "event.hpp"
#include "containers/numeric/math_vector.hpp"
#include <string_view>
#include <cstdint>

namespace gati {

enum class ElementType : std::uint8_t {
    Neutral = 0,
    Water,
    Lava,
    Fire,
    Wood,
    Metal,
    Acid,
    Electricity,
    Obsidian,
    Ice
};

struct ElementalComponent {
    ElementType type = ElementType::Neutral;
    float       intensity = 1.0f; // Concentration / charge
};

struct ReactionResult {
    bool        reacted = false;
    std::string_view description{};
    bool        spawn_obsidian = false;
    bool        spawn_steam_burst = false;
    bool        ignite = false;
    float       corrosion_damage = 0.0f;
    float       shockwave_impulse = 0.0f;
};

class ElementalReactionMatrix {
public:
    // Evaluate elemental contact interaction
    [[nodiscard]] static ReactionResult evaluate(ElementType a, ElementType b) noexcept {
        ReactionResult res;

        // Symmetric check
        auto matches = [a, b](ElementType x, ElementType y) {
            return (a == x && b == y) || (a == y && b == x);
        };

        // 1. Water + Lava -> Obsidian (Hard Solid) + Steam (Gas Burst)
        if (matches(ElementType::Water, ElementType::Lava)) {
            res.reacted = true;
            res.description = "Water and Lava fused into Obsidian with Steam burst";
            res.spawn_obsidian = true;
            res.spawn_steam_burst = true;
            return res;
        }

        // 2. Fire + Wood -> Ignition + Smoke
        if (matches(ElementType::Fire, ElementType::Wood)) {
            res.reacted = true;
            res.description = "Fire ignited Wood";
            res.ignite = true;
            return res;
        }

        // 3. Acid + Metal -> Corrosion Damage + Hydrogen
        if (matches(ElementType::Acid, ElementType::Metal)) {
            res.reacted = true;
            res.description = "Acid corroded Metal";
            res.corrosion_damage = 0.35f;
            return res;
        }

        // 4. Electricity + Water -> Conductive Shockwave
        if (matches(ElementType::Electricity, ElementType::Water)) {
            res.reacted = true;
            res.description = "Electricity conducted through Water creating shockwave";
            res.shockwave_impulse = 300.0f;
            return res;
        }

        // 5. Fire + Ice -> Rapid Melt to Water
        if (matches(ElementType::Fire, ElementType::Ice)) {
            res.reacted = true;
            res.description = "Fire melted Ice to Water";
            return res;
        }

        return res;
    }

    // Process elemental reactions across contact events
    static void process_contact(pebble::ecs::World& world, const ContactEvent& ce) {
        const pebble::ecs::Entity ent_a = world.entity_from_index(ce.a);
        const pebble::ecs::Entity ent_b = world.entity_from_index(ce.b);

        auto* elem_a = world.get<ElementalComponent>(ent_a);
        auto* elem_b = world.get<ElementalComponent>(ent_b);
        if (!elem_a && !elem_b) return;

        const ElementType type_a = elem_a ? elem_a->type : ElementType::Neutral;
        const ElementType type_b = elem_b ? elem_b->type : ElementType::Neutral;

        const auto reaction = evaluate(type_a, type_b);
        if (!reaction.reacted) return;

        // Apply Reaction Consequences
        if (reaction.spawn_obsidian) {
            // Transform Lava entity into Obsidian solid
            const auto lava_entity = (type_a == ElementType::Lava) ? ent_a : ent_b;
            const auto water_entity = (type_a == ElementType::Water) ? ent_a : ent_b;

            if (world.alive(lava_entity)) {
                if (auto* mat = world.get<MaterialComponent>(lava_entity)) {
                    mat->params.rest_density = 2600.0f;
                    mat->params.youngs_modulus = 6e10f;
                    mat->params.yield_strain = 0.005f;
                    mat->params.ultimate_strain = 0.01f;
                    mat->temperature = 100.0f;
                    mat->phase_fractions = prakriti::phase_from_temperature(mat->temperature, mat->params);
                }
                if (auto* elem = world.get<ElementalComponent>(lava_entity)) {
                    elem->type = ElementType::Obsidian;
                }
            }

            // Despawn consumed water entity
            if (world.alive(water_entity)) {
                world.despawn(water_entity);
            }
        }

        if (reaction.ignite) {
            const auto wood_entity = (type_a == ElementType::Wood) ? ent_a : ent_b;
            if (world.alive(wood_entity)) {
                if (auto* mat = world.get<MaterialComponent>(wood_entity)) {
                    mat->temperature = 350.0f;
                    mat->flammable = true;
                }
            }
        }

        if (reaction.corrosion_damage > 0.0f) {
            const auto metal_entity = (type_a == ElementType::Metal) ? ent_a : ent_b;
            if (world.alive(metal_entity)) {
                if (auto* mat = world.get<MaterialComponent>(metal_entity)) {
                    mat->damage = std::min(1.0f, mat->damage + reaction.corrosion_damage);
                }
            }
        }
    }
};

} // namespace gati
