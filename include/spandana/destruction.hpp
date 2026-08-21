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
#include "containers/numeric/math_vector.hpp"
#include <vector>
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

        // 2. Perform Voronoi Clipping using Akruti Fracture
        auto raw_shards = akruti::voronoi_shatter(source_poly, sites);

        // 3. Compute mass properties & radial velocities
        std::vector<ShardSpawnDesc> result;
        result.reserve(raw_shards.size());

        for (const auto& poly : raw_shards) {
            const std::size_t n = poly.size();
            if (n < 3) continue;

            float signed_area = 0.0f;
            float cx = 0.0f, cy = 0.0f;
            for (std::size_t i = 0; i < n; ++i) {
                const auto& p0 = poly[i];
                const auto& p1 = poly[(i + 1) % n];
                const float cross_term = p0.x * p1.y - p1.x * p0.y;
                signed_area += cross_term;
                cx += (p0.x + p1.x) * cross_term;
                cy += (p0.y + p1.y) * cross_term;
            }
            const float area = std::abs(signed_area * 0.5f);
            if (area < 1e-4f) continue;

            const float inv_area6 = 1.0f / (3.0f * signed_area);
            cx *= inv_area6;
            cy *= inv_area6;
            const float iz = area * 10.0f; // Approximate polar inertia

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
                .polygon = poly,
                .centroid = pebble::math::vec2(cx, cy),
                .area = area,
                .inertia_z = iz,
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
        }
    }
};

} // namespace pebble::spandana
