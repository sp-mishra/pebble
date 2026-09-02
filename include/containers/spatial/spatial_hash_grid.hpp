#pragma once
// ============================================================================
// containers/spatial/spatial_hash_grid.hpp — Generic Zero-Alloc Spatial Hash Grid
// ============================================================================
// Modern C++23 header-only, policy-driven spatial partitioning grid.
// O(N) broadphase culling, zero runtime heap allocations, zero virtual functions.
// ============================================================================

#include <cstdint>
#include <cstddef>
#include <cmath>
#include <vector>
#include <array>
#include <span>
#include <concepts>
#include <algorithm>

namespace containers::spatial {
    template <typename IdType = std::uint32_t, float CellSize = 32.0f, std::size_t TableSize = 2048>
    class SpatialHashGrid {
    public:
        static_assert((TableSize & (TableSize - 1)) == 0, "TableSize must be a power of two");
        static constexpr float kCellSize = CellSize;
        static constexpr float kInvCellSize = 1.0f / CellSize;
        static constexpr std::size_t kMask = TableSize - 1;

        struct Entry {
            IdType id{};
            float x = 0.0f;
            float y = 0.0f;
            std::int32_t next = -1;
        };

        SpatialHashGrid() {
            head_.fill(-1);
            entries_.reserve(1024);
        }

        explicit SpatialHashGrid(std::size_t reserve_capacity) {
            head_.fill(-1);
            entries_.reserve(reserve_capacity);
        }

        // O(1) frame reset
        void clear() noexcept {
            head_.fill(-1);
            entries_.clear();
        }

        // Hash integer cell coordinates via SplitMix64 avalanche
        [[nodiscard]] static constexpr std::size_t hash_coords(std::int32_t cx, std::int32_t cy) noexcept {
            std::uint64_t h = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(cx)) * 0x9e3779b97f4a7c15ULL) ^
                (static_cast<std::uint64_t>(static_cast<std::uint32_t>(cy)) * 0xbf58476d1ce4e5b9ULL);
            h ^= h >> 30;
            h *= 0x94d049bb133111ebULL;
            h ^= h >> 31;
            return static_cast<std::size_t>(h & kMask);
        }

        // O(1) particle insertion
        void insert(IdType id, float x, float y) noexcept {
            const std::int32_t cx = static_cast<std::int32_t>(std::floor(x * kInvCellSize));
            const std::int32_t cy = static_cast<std::int32_t>(std::floor(y * kInvCellSize));
            const std::size_t bucket = hash_coords(cx, cy);

            const std::int32_t new_idx = static_cast<std::int32_t>(entries_.size());
            entries_.push_back(Entry{
                .id = id,
                .x = x,
                .y = y,
                .next = head_[bucket]
            });
            head_[bucket] = new_idx;
        }

        // Iterates over all candidate neighbor bodies within the 3x3 adjacent cells
        template <typename Callback>
        void for_each_neighbor(float x, float y, Callback&& cb) const noexcept {
            const std::int32_t cx = static_cast<std::int32_t>(std::floor(x * kInvCellSize));
            const std::int32_t cy = static_cast<std::int32_t>(std::floor(y * kInvCellSize));

            for (std::int32_t dx = -1; dx <= 1; ++dx) {
                for (std::int32_t dy = -1; dy <= 1; ++dy) {
                    const std::size_t bucket = hash_coords(cx + dx, cy + dy);
                    std::int32_t curr = head_[bucket];
                    while (curr != -1) {
                        const auto& entry = entries_[static_cast<std::size_t>(curr)];
                        cb(entry.id, entry.x, entry.y);
                        curr = entry.next;
                    }
                }
            }
        }

        [[nodiscard]] std::size_t size() const noexcept {
            return entries_.size();
        }

        [[nodiscard]] bool empty() const noexcept {
            return entries_.empty();
        }

    private:
        std::array<std::int32_t, TableSize> head_;
        std::vector<Entry> entries_;
    };

    // ── Morton Z-Order 2D Curve Encoding for Cache-Locality Sorting ──────────────

    // Interleaves 16-bit integers to form a 32-bit Morton code (Z-order curve)
    [[nodiscard]] constexpr std::uint32_t morton_encode_2d(std::uint16_t x, std::uint16_t y) noexcept {
        auto expand_bits = [](std::uint16_t v) constexpr -> std::uint32_t {
            std::uint32_t x32 = v;
            x32 = (x32 | (x32 << 8)) & 0x00FF00FF;
            x32 = (x32 | (x32 << 4)) & 0x0F0F0F0F;
            x32 = (x32 | (x32 << 2)) & 0x33333333;
            x32 = (x32 | (x32 << 1)) & 0x55555555;
            return x32;
        };
        return expand_bits(x) | (expand_bits(y) << 1);
    }

    // Computes 2D Morton Z-code for spatial floating-point coordinates
    [[nodiscard]] inline std::uint32_t morton_key_from_pos(float x, float y, float min_x = -50000.0f,
                                                           float min_y = -50000.0f, float cell_res = 16.0f) noexcept {
        const auto qx = static_cast<std::uint16_t>(std::clamp((x - min_x) / cell_res, 0.0f, 65535.0f));
        const auto qy = static_cast<std::uint16_t>(std::clamp((y - min_y) / cell_res, 0.0f, 65535.0f));
        return morton_encode_2d(qx, qy);
    }
} // namespace containers::spatial
