#pragma once
// ============================================================================
// prakriti/celestial/sector_types.hpp — Infinite Cosmos Sector Data & Glaze Schemas
// ============================================================================
// Zero-virtual, header-only C++23 structures for out-of-core cosmic sector streaming.
// ============================================================================

#include "glaze/glaze.hpp"
#include "containers/numeric/math_vector.hpp"
#include <cstdint>
#include <vector>
#include <concepts>
#include <compare>

namespace prakriti::celestial {

// Sector spatial tile dimensions (320px x 200px = 0.25x of 1280x800 viewport for smooth sliding)
inline constexpr float kSectorWidth  = 320.0f;
inline constexpr float kSectorHeight = 200.0f;

// 2D Spatial Grid Coordinates for Sectors
struct SectorKey {
    std::int32_t x = 0;
    std::int32_t y = 0;

    auto operator<=>(const SectorKey&) const = default;
};

// 64-bit Morton Z-Order & Spatial Hash for SectorKey
[[nodiscard]] inline std::uint64_t hash_sector_key(const SectorKey& k) noexcept {
    // SplitMix64 coordinate avalanche hash
    std::uint64_t h = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(k.x)) << 32) ^
                      static_cast<std::uint64_t>(static_cast<std::uint32_t>(k.y)) ^
                      0x9e3779b97f4a7c15ULL;
    h ^= h >> 30;
    h *= 0xbf58476d1ce4e5b9ULL;
    h ^= h >> 27;
    h *= 0x94d049bb133111ebULL;
    h ^= h >> 31;
    return h;
}

// Compact 44-byte POD for high-density particle storage in RAM and on Disk
struct CompactBodyRecord {
    float x = 0.0f;
    float y = 0.0f;
    float vx = 0.0f;
    float vy = 0.0f;
    float mass = 10.0f;
    float radius = 2.0f;
    float temperature = 20.0f; // Celsius
    float omega = 0.0f;       // Spin rad/s
    std::uint8_t type = 1;    // CelestialType enum integer
    float ocean_fraction = 0.0f;
    float atmosphere_mass = 0.0f;
    float crust_solid = 1.0f;
};

// Quadrupole Tensor Moment for Fast Multipole Far-Field Gravitational Influence
struct SectorQuadrupole {
    float qxx = 0.0f;
    float qxy = 0.0f;
    float qyy = 0.0f;
};

// State vector of a single cosmic sector
struct SectorData {
    SectorKey key{};
    std::uint64_t discovery_tick = 0;
    float total_mass = 0.0f;
    float barycenter_x = 0.0f;
    float barycenter_y = 0.0f;
    SectorQuadrupole quadrupole{};
    std::vector<CompactBodyRecord> bodies;
    bool is_active_in_sim = false;
};

// Ultra-compact Dormant Sector Macro-Node for O(1) collective gravitational pull
struct SectorMacroNode {
    SectorKey key{};
    float total_mass = 0.0f;
    float bx = 0.0f;
    float by = 0.0f;
    SectorQuadrupole q{};
};

} // namespace prakriti::celestial

// ── Compile-time Glaze Metadata Reflection for Zero-Copy Serialization ───────

template <>
struct glz::meta<prakriti::celestial::SectorKey> {
    using T = prakriti::celestial::SectorKey;
    static constexpr auto value = object("x", &T::x, "y", &T::y);
};

template <>
struct glz::meta<prakriti::celestial::SectorQuadrupole> {
    using T = prakriti::celestial::SectorQuadrupole;
    static constexpr auto value = object("qxx", &T::qxx, "qxy", &T::qxy, "qyy", &T::qyy);
};

template <>
struct glz::meta<prakriti::celestial::CompactBodyRecord> {
    using T = prakriti::celestial::CompactBodyRecord;
    static constexpr auto value = object(
        "x", &T::x, "y", &T::y,
        "vx", &T::vx, "vy", &T::vy,
        "m", &T::mass, "r", &T::radius,
        "temp", &T::temperature, "omega", &T::omega,
        "type", &T::type, "ocean", &T::ocean_fraction,
        "atmo", &T::atmosphere_mass, "crust", &T::crust_solid
    );
};

template <>
struct glz::meta<prakriti::celestial::SectorData> {
    using T = prakriti::celestial::SectorData;
    static constexpr auto value = object(
        "key", &T::key,
        "tick", &T::discovery_tick,
        "mass", &T::total_mass,
        "bx", &T::barycenter_x, "by", &T::barycenter_y,
        "q", &T::quadrupole,
        "bodies", &T::bodies
    );
};
