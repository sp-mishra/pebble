#pragma once
// ============================================================================
// gati/world/spatial_tile_streamer.hpp — Generic Open-World Spatial Streamer
// ============================================================================
// Modern C++23 header-only 2D viewport spatial chunk/tile streaming engine.
// Usable across open-world RPGs, RTS, strategy, cosmology and particle simulations.
// ============================================================================

#include "containers/numeric/math_vector.hpp"
#include "containers/static/static_vector.hpp"
#include <cstdint>
#include <cstddef>
#include <cmath>
#include <unordered_set>
#include <concepts>
#include <utility>

namespace gati::world {

struct TileCoord {
    std::int32_t x = 0;
    std::int32_t y = 0;

    auto operator<=>(const TileCoord&) const = default;
};

// SplitMix64 coordinate avalanche hash
[[nodiscard]] inline constexpr std::uint64_t hash_tile_coord(const TileCoord& coord) noexcept {
    std::uint64_t h = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(coord.x)) << 32) ^
                      static_cast<std::uint64_t>(static_cast<std::uint32_t>(coord.y)) ^
                      0x9e3779b97f4a7c15ULL;
    h ^= h >> 30;
    h *= 0xbf58476d1ce4e5b9ULL;
    h ^= h >> 27;
    h *= 0x94d049bb133111ebULL;
    h ^= h >> 31;
    return h;
}

template <float TileWidth = 320.0f, float TileHeight = 200.0f>
class SpatialTileStreamer {
public:
    static constexpr float kTileWidth = TileWidth;
    static constexpr float kTileHeight = TileHeight;

    SpatialTileStreamer() = default;

    void reset() noexcept {
        visited_tiles_.clear();
        prev_active_tiles_.clear();
    }

    void mark_visited(TileCoord coord) {
        visited_tiles_.insert(hash_tile_coord(coord));
    }

    [[nodiscard]] bool is_visited(TileCoord coord) const noexcept {
        return visited_tiles_.contains(hash_tile_coord(coord));
    }

    // Updates camera position and invokes callbacks for discovered, active, and evicted tiles
    template <typename OnDiscoverCallback, typename OnActiveTileCallback, typename OnEvictCallback>
    void update_viewport(pebble::math::vec2 camera_pos,
                         float viewport_w,
                         float viewport_h,
                         float margin_ratio,
                         OnDiscoverCallback&& on_discover,
                         OnActiveTileCallback&& on_active,
                         OnEvictCallback&& on_evict) {
        const float margin_w = viewport_w * margin_ratio;
        const float margin_h = viewport_h * margin_ratio;

        const float min_x = camera_pos[0] - viewport_w * 0.5f - margin_w;
        const float max_x = camera_pos[0] + viewport_w * 0.5f + margin_w;
        const float min_y = camera_pos[1] - viewport_h * 0.5f - margin_h;
        const float max_y = camera_pos[1] + viewport_h * 0.5f + margin_h;

        const std::int32_t min_tx = static_cast<std::int32_t>(std::floor(min_x / kTileWidth));
        const std::int32_t max_tx = static_cast<std::int32_t>(std::floor(max_x / kTileWidth));
        const std::int32_t min_ty = static_cast<std::int32_t>(std::floor(min_y / kTileHeight));
        const std::int32_t max_ty = static_cast<std::int32_t>(std::floor(max_y / kTileHeight));

        // Build current active set
        containers::static_vector<TileCoord, 256> curr_active;
        for (std::int32_t tx = min_tx; tx <= max_tx; ++tx) {
            for (std::int32_t ty = min_ty; ty <= max_ty; ++ty) {
                const TileCoord coord{tx, ty};
                const std::uint64_t hid = hash_tile_coord(coord);

                if (!visited_tiles_.contains(hid)) {
                    visited_tiles_.insert(hid);
                    on_discover(coord);
                }
                on_active(coord);
                (void)curr_active.push_back(coord);
            }
        }

        // Fire on_evict for tiles that were active last frame but not this frame
        for (const TileCoord& prev : prev_active_tiles_) {
            bool still_active = false;
            for (const TileCoord& curr : curr_active) {
                if (curr.x == prev.x && curr.y == prev.y) {
                    still_active = true;
                    break;
                }
            }
            if (!still_active) {
                on_evict(prev);
            }
        }

        prev_active_tiles_ = curr_active;
    }

    // Backwards-compatible overload without on_evict
    template <typename OnDiscoverCallback, typename OnActiveTileCallback>
    void update_viewport(pebble::math::vec2 camera_pos,
                         float viewport_w,
                         float viewport_h,
                         float margin_ratio,
                         OnDiscoverCallback&& on_discover,
                         OnActiveTileCallback&& on_active) {
        update_viewport(camera_pos, viewport_w, viewport_h, margin_ratio,
                        std::forward<OnDiscoverCallback>(on_discover),
                        std::forward<OnActiveTileCallback>(on_active),
                        [](TileCoord) {});
    }

    [[nodiscard]] std::size_t visited_count() const noexcept {
        return visited_tiles_.size();
    }

private:
    std::unordered_set<std::uint64_t> visited_tiles_;
    containers::static_vector<TileCoord, 256> prev_active_tiles_;
};

} // namespace gati::world
