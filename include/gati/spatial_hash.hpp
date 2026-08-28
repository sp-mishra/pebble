#pragma once
// gati/spatial_hash.hpp — Gati Broadphase Adapter using Akruti SpatialHash.
#include "akruti/spatial_hash.hpp"
#include <cstdint>

namespace gati {

using SpatialHash = akruti::SpatialHash<std::uint32_t, akruti::MortonOrder>;

} // namespace gati
