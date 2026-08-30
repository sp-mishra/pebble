#pragma once
// ============================================================================
// spandana/destruction.hpp — Procedural 2D Voronoi Entity Shattering & Physics Shards
// ============================================================================
// Slices an entity's Akruti geometry into Voronoi shards upon impact, calculates
// exact mass properties (centroid, area, polar moment of inertia Iz), and spawns
// dynamic shard entities with radial explosive impulse velocities.
// ============================================================================

#include "akruti/akruti.hpp"
#include "akruti/khanda.hpp"
#include "ecs/ecs.hpp"
#include "gati/transform.hpp"
#include "gati/collision.hpp"
#include "containers/numeric/math_vector.hpp"
#include <vector>
#include <span>
#include <cmath>

namespace pebble::spandana {

struct ShardSpawnDesc {
    akruti::Poly       polygon;
    pebble::math::vec2 centroid{0.0f, 0.0f};
    float              area = 0.0f;
    float              inertia_z = 0.0f;
    pebble::math::vec2 initial_velocity{0.0f, 0.0f};
    float              initial_angular_velocity = 0.0f;
};

// Procedural Voronoi Fracture Generator
class DestructionEngine {
public:
    // Fractures a source polygon centered around an impact point into shards
    [[nodiscard]] static std::vector<ShardSpawnDesc> shatter_polygon(
        const akruti::Poly& source_poly,
        const pebble::math::vec2& impact_point,
        std::size_t shard_count = 8,
        float radial_impulse_mag = 200.0f,
        float max_spin = 5.0f) {

        if (source_poly.size() < 3 || shard_count == 0) return {};

        // 1. Generate Voronoi fracture sites clustered around impact
        std::vector<akruti::Vec> sites;
        sites.reserve(shard_count);

        std::mt19937 rng(42);
        std::uniform_real_distribution<float> angle_dist(0.0f, 6.2831853f);
        std::uniform_real_distribution<float> radius_dist(2.0f, 40.0f);

        for (std::size_t i = 0; i < shard_count; ++i) {
            const float angle = angle_dist(rng);
            const float r = (i == 0) ? 0.0f : radius_dist(rng);
            sites.push_back(akruti::Vec{
                impact_point[0] + std::cos(angle) * r,
                impact_point[1] + std::sin(angle) * r
            });
        }

        // 2. Fracture into shards with EXACT mass properties (Akruti Khanda owns the geometry
        //    + centroid/area/polar-inertia; Spandana keeps only the choreography below).
        akruti::khanda::FractureConfig cfg{};
        cfg.compute_mass_props = true;
        const auto shards = akruti::khanda::fracture_voronoi(
            source_poly, std::span<const akruti::Vec>(sites.data(), sites.size()), cfg);

        // 3. Author launch velocities/spin per shard (motion intent, not geometry).
        std::vector<ShardSpawnDesc> result;
        result.reserve(shards.size());

        for (const auto& shard : shards) {
            const float area = shard.area;
            if (area < 1e-4f) continue;
            const float cx = shard.centroid.x;
            const float cy = shard.centroid.y;

            // Radial velocity directed away from impact point
            const float dx = cx - impact_point[0];
            const float dy = cy - impact_point[1];
            const float dist = std::sqrt(dx * dx + dy * dy);

            pebble::math::vec2 vel{0.0f, 0.0f};
            if (dist > 1e-4f) {
                vel = pebble::math::vec2((dx / dist) * radial_impulse_mag,
                                         (dy / dist) * radial_impulse_mag);
            }

            const float spin = ((dx * dy > 0.0f) ? 1.0f : -1.0f) * max_spin * (1.0f / (1.0f + area * 0.01f));

            result.push_back(ShardSpawnDesc{
                .polygon = shard.outline,
                .centroid = pebble::math::vec2(cx, cy),
                .area = area,
                .inertia_z = shard.inertia, // exact polar moment from Akruti
                .initial_velocity = vel,
                .initial_angular_velocity = spin
            });
        }

        return result;
    }

    // Shatters an existing entity in the World and spawns dynamic shard entities
    static void shatter_entity_in_world(
        pebble::ecs::World& world,
        pebble::ecs::Entity entity,
        const pebble::math::vec2& impact_point,
        std::size_t shard_count = 8,
        float radial_impulse_mag = 200.0f) {

        auto* tr = world.get<gati::Transform>(entity);
        if (!tr) return;

        // Default bounding box polygon if not explicitly set
        akruti::Poly source_poly{
            akruti::Vec{tr->position[0] - 20.0f, tr->position[1] - 20.0f},
            akruti::Vec{tr->position[0] + 20.0f, tr->position[1] - 20.0f},
            akruti::Vec{tr->position[0] + 20.0f, tr->position[1] + 20.0f},
            akruti::Vec{tr->position[0] - 20.0f, tr->position[1] + 20.0f}
        };

        auto shards = shatter_polygon(source_poly, impact_point, shard_count, radial_impulse_mag);

        // Despawn original intact entity
        world.despawn(entity);

        // Spawn dynamic shards
        for (const auto& shard : shards) {
            auto shard_entity = world.spawn();
            world.add<gati::Transform>(shard_entity, {
                .position = shard.centroid,
                .prev_position = shard.centroid
            });

#if defined(GATI_HAS_AKRUTI)
            if (shard.polygon.size() <= 8) {
                akruti::ConvexPoly<8> cp;
                for (const auto& v : shard.polygon) {
                    // Local relative coordinates to centroid
                    (void)cp.verts.push_back(akruti::Vec{v.x - shard.centroid[0], v.y - shard.centroid[1]});
                }
                world.add<gati::ShapeRef>(shard_entity, {.shape = cp});
            }
#endif
        }
    }
};

} // namespace pebble::spandana
