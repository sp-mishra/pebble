#pragma once
// ============================================================================
// prakriti/celestial/sector_generator.hpp — SplitMix64 / Jeans Sector Nucleation
// ============================================================================
// Zero-virtual, deterministic procedural matter generator for boundless cosmos.
// ============================================================================

#include "sector_types.hpp"
#include <random>
#include <cmath>
#include <algorithm>

namespace prakriti::celestial {
    // Evaluates deterministic SplitMix64 spatial seed from 2D sector grid coordinates
    [[nodiscard]] inline std::uint64_t
    compute_sector_seed(int32_t sx, int32_t sy, std::uint64_t cosmic_seed = 13371337ULL) noexcept {
        std::uint64_t h = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(sx)) << 32) ^
            static_cast<std::uint64_t>(static_cast<std::uint32_t>(sy)) ^
            cosmic_seed;
        h ^= h >> 30;
        h *= 0xbf58476d1ce4e5b9ULL;
        h ^= h >> 27;
        h *= 0x94d049bb133111ebULL;
        h ^= h >> 31;
        return h;
    }

    // Procedurally nucleates initial primordial celestial bodies in an uncharted sector
    [[nodiscard]] inline SectorData
    generate_procedural_sector(SectorKey key, std::uint64_t cosmic_seed = 13371337ULL) noexcept {
        SectorData sector;
        sector.key = key;
        sector.discovery_tick = 0;

        const std::uint64_t seed = compute_sector_seed(key.x, key.y, cosmic_seed);
        std::mt19937_64 rng(seed);
        std::uniform_real_distribution<float> dist01(0.0f, 1.0f);

        const float sector_base_x = static_cast<float>(key.x) * kSectorWidth;
        const float sector_base_y = static_cast<float>(key.y) * kSectorHeight;

        // Density regime determination via spatial hash entropy
        const float density_roll = dist01(rng);

        std::size_t num_specs = 0;
        if (density_roll < 0.25f) {
            // Regime 1: Deep Cosmic Void (Sparse diffuse hydrogen dust)
            num_specs = 8 + static_cast<std::size_t>(dist01(rng) * 12.0f);
        }
        else if (density_roll < 0.80f) {
            // Regime 2: Star-Forming Stellar Nursery / Diffuse Nebula
            num_specs = 25 + static_cast<std::size_t>(dist01(rng) * 20.0f);
        }
        else {
            // Regime 3: Dense Protogalaxy / Binary Star Accretion Cluster
            num_specs = 45 + static_cast<std::size_t>(dist01(rng) * 35.0f);
        }

        sector.bodies.reserve(num_specs + 2);

        float sum_m = 0.0f;
        float sum_mx = 0.0f;
        float sum_my = 0.0f;

        // For Regime 3, seed a massive central protostar at the sector core
        if (density_roll >= 0.75f) {
            CompactBodyRecord core_star;
            core_star.x = sector_base_x + kSectorWidth * 0.5f + (dist01(rng) - 0.5f) * 200.0f;
            core_star.y = sector_base_y + kSectorHeight * 0.5f + (dist01(rng) - 0.5f) * 200.0f;
            core_star.vx = (dist01(rng) - 0.5f) * 8.0f;
            core_star.vy = (dist01(rng) - 0.5f) * 8.0f;
            core_star.mass = 350.0f + dist01(rng) * 450.0f;
            core_star.radius = std::clamp(std::cbrt(core_star.mass) * 0.65f, 5.0f, 12.0f);
            core_star.temperature = 4200.0f + dist01(rng) * 8000.0f;
            core_star.type = 4; // Superheated Plasma
            core_star.omega = (dist01(rng) - 0.5f) * 6.0f;
            sector.bodies.push_back(core_star);

            sum_m += core_star.mass;
            sum_mx += core_star.mass * core_star.x;
            sum_my += core_star.mass * core_star.y;
        }

        // Seed orbiting matter & planetoids
        for (std::size_t i = 0; i < num_specs; ++i) {
            CompactBodyRecord b;
            b.x = sector_base_x + 30.0f + dist01(rng) * (kSectorWidth - 60.0f);
            b.y = sector_base_y + 30.0f + dist01(rng) * (kSectorHeight - 60.0f);

            const float mat_roll = dist01(rng);
            if (mat_roll < 0.35f) {
                b.type = 0; // IceCrust
                b.mass = 8.0f + dist01(rng) * 8.0f;
                b.temperature = -70.0f + dist01(rng) * 40.0f;
            }
            else if (mat_roll < 0.78f) {
                b.type = 1; // SilicateRock
                b.mass = 14.0f + dist01(rng) * 12.0f;
                b.temperature = 15.0f + dist01(rng) * 50.0f;
            }
            else if (mat_roll < 0.95f) {
                b.type = 2; // IronCore
                b.mass = 24.0f + dist01(rng) * 18.0f;
                b.temperature = 60.0f + dist01(rng) * 90.0f;
            }
            else {
                b.type = 3; // MoltenMagma
                b.mass = 40.0f + dist01(rng) * 35.0f;
                b.temperature = 1100.0f + dist01(rng) * 400.0f;
            }

            b.radius = std::clamp(std::cbrt(b.mass) * 0.55f, 0.85f, 2.8f);

            // Primordial Keplerian drift around sector center
            const float dx = b.x - (sector_base_x + kSectorWidth * 0.5f);
            const float dy = b.y - (sector_base_y + kSectorHeight * 0.5f);
            const float dist = std::sqrt(dx * dx + dy * dy) + 10.0f;
            const float v_orbit = (density_roll >= 0.75f) ? (std::sqrt(18000.0f * 400.0f / dist) * 0.06f) : 2.5f;

            const float norm_tangent_x = -dy / dist;
            const float norm_tangent_y = dx / dist;

            const float dispersion = 1.0f + dist01(rng) * 2.0f;
            const float disp_ang = dist01(rng) * 6.2831853f;

            b.vx = norm_tangent_x * v_orbit + std::cos(disp_ang) * dispersion;
            b.vy = norm_tangent_y * v_orbit + std::sin(disp_ang) * dispersion;
            b.omega = (dist01(rng) - 0.5f) * 5.0f;

            sector.bodies.push_back(b);

            sum_m += b.mass;
            sum_mx += b.mass * b.x;
            sum_my += b.mass * b.y;
        }

        if (sum_m > 0.0f) {
            sector.total_mass = sum_m;
            sector.barycenter_x = sum_mx / sum_m;
            sector.barycenter_y = sum_my / sum_m;

            // Compute Quadrupole Tensor Moments Qxx, Qxy, Qyy relative to barycenter
            for (const auto& b : sector.bodies) {
                const float rx = b.x - sector.barycenter_x;
                const float ry = b.y - sector.barycenter_y;
                const float r2 = rx * rx + ry * ry;
                sector.quadrupole.qxx += b.mass * (3.0f * rx * rx - r2);
                sector.quadrupole.qxy += b.mass * (3.0f * rx * ry);
                sector.quadrupole.qyy += b.mass * (3.0f * ry * ry - r2);
            }
        }

        return sector;
    }
} // namespace prakriti::celestial
