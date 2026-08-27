#pragma once
// ============================================================================
// prakriti/core/spatial_hash.hpp — uniform-grid neighbor search over predicted positions.
// Cell-list built by counting sort into contiguous arrays for cache-coherent iteration.
// Rebuilt each frame; query yields candidate indices within a search radius.
// ============================================================================
#include "config.hpp"
#include <vector>
#include <span>
#include <cmath>
#include <cstdint>
#include <algorithm>

namespace prakriti {

class SpatialHash {
public:
    static constexpr std::uint32_t kTableSize = 4096; // Power-of-two flat table
    static constexpr std::uint32_t kTableMask = kTableSize - 1;
    static constexpr Index kInvalid = static_cast<Index>(-1);

    explicit SpatialHash(Scalar cell_size = Scalar(1))
        : inv_cell_(Scalar(1) / cell_size), cell_size_(cell_size) {
        head_.assign(kTableSize, kInvalid);
    }

    void set_cell_size(Scalar s) noexcept { cell_size_ = s; inv_cell_ = Scalar(1) / s; }
    [[nodiscard]] Scalar cell_size() const noexcept { return cell_size_; }

    // (Re)build spatial linked-list cell structures with O(N) single-pass zero-allocation
    void build(std::span<const Scalar> px, std::span<const Scalar> py) {
        const Index n = static_cast<Index>(px.size());
        cell_of_.resize(n);
        next_.resize(n);
        std::fill(head_.begin(), head_.end(), kInvalid);

        for (Index i = 0; i < n; ++i) {
            const auto coord = cell_coord(px[i], py[i]);
            const std::uint32_t slot = hash_coords(coord.x, coord.y);
            cell_of_[i] = pack_coords(coord.x, coord.y);
            next_[i] = head_[slot];
            head_[slot] = i;
        }
    }

    // 4-Color Checkerboard Partition for Lock-Free Parallel Sweeps
    [[nodiscard]] static constexpr std::uint32_t cell_color(std::int32_t cx, std::int32_t cy) noexcept {
        return (static_cast<std::uint32_t>(cx) & 1u) | ((static_cast<std::uint32_t>(cy) & 1u) << 1);
    }

    [[nodiscard]] std::uint32_t particle_color(Index i) const noexcept {
        if (i >= cell_of_.size()) return 0;
        const std::uint32_t packed = cell_of_[i];
        const auto cx = static_cast<std::int16_t>(packed >> 16);
        const auto cy = static_cast<std::int16_t>(packed & 0xFFFF);
        return cell_color(cx, cy);
    }

    // Fast 3x3 neighbor traversal with zero hash table lookups
    template <class Fn>
    void for_each_neighbor(Scalar px, Scalar py, Scalar radius, Fn&& fn) const {
        const auto [cx, cy] = cell_coord(px, py);
        const Scalar r2 = radius * radius;

        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                const int nx = cx + dx;
                const int ny = cy + dy;
                const std::uint32_t target_packed = pack_coords(nx, ny);
                const std::uint32_t slot = hash_coords(nx, ny);

                Index cur = head_[slot];
                while (cur != kInvalid) {
                    if (cell_of_[cur] == target_packed) {
                        fn(cur, r2);
                    }
                    cur = next_[cur];
                }
            }
        }
    }

    struct CellCoord { std::int32_t x, y; };

    [[nodiscard]] CellCoord cell_coord(Scalar px, Scalar py) const noexcept {
        return {static_cast<std::int32_t>(std::floor(px * inv_cell_)),
                static_cast<std::int32_t>(std::floor(py * inv_cell_))};
    }

private:
    [[nodiscard]] static constexpr std::uint32_t part1by1(std::uint32_t x) noexcept {
        x &= 0x0000ffff;
        x = (x | (x << 8)) & 0x00FF00FF;
        x = (x | (x << 4)) & 0x0F0F0F0F;
        x = (x | (x << 2)) & 0x33333333;
        x = (x | (x << 1)) & 0x55555555;
        return x;
    }

    [[nodiscard]] static constexpr std::uint32_t morton2d(std::uint32_t x, std::uint32_t y) noexcept {
        return (part1by1(y) << 1) | part1by1(x);
    }

    [[nodiscard]] static constexpr std::uint32_t pack_coords(std::int32_t x, std::int32_t y) noexcept {
        return (static_cast<std::uint32_t>(x & 0xFFFF) << 16) | static_cast<std::uint32_t>(y & 0xFFFF);
    }

    [[nodiscard]] static constexpr std::uint32_t hash_coords(std::int32_t x, std::int32_t y) noexcept {
        // High-avalanche spatial prime multiplier hash combined with Morton interleaving
        const std::uint32_t m = morton2d(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
        return (m ^ (m >> 12)) & kTableMask;
    }

    Scalar inv_cell_;
    Scalar cell_size_;
    std::vector<std::uint32_t> head_;
    std::vector<std::uint32_t> next_;
    std::vector<std::uint32_t> cell_of_;
};

} // namespace prakriti
