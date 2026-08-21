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
#include <unordered_map>

namespace prakriti {

class SpatialHash {
public:
    explicit SpatialHash(Scalar cell_size = Scalar(1))
        : inv_cell_(Scalar(1) / cell_size), cell_size_(cell_size) {}

    void set_cell_size(Scalar s) noexcept { cell_size_ = s; inv_cell_ = Scalar(1) / s; }
    [[nodiscard]] Scalar cell_size() const noexcept { return cell_size_; }

    // (Re)build the cell list from split position columns (x[], y[]).
    void build(std::span<const Scalar> px, std::span<const Scalar> py) {
        const Index n = static_cast<Index>(px.size());
        cell_of_.resize(n);
        bucket_start_.clear();
        entries_.resize(n);

        // Map each point to a cell key; count per cell.
        std::unordered_map<std::int64_t, Index> counts;
        counts.reserve(n * 2);
        for (Index i = 0; i < n; ++i) {
            const std::int64_t key = cell_key(cell_coord(px[i], py[i]));
            cell_of_[i] = key;
            ++counts[key];
        }
        // Prefix-sum bucket offsets.
        Index acc = 0;
        bucket_start_.reserve(counts.size());
        for (auto& [key, c] : counts) {
            bucket_start_[key] = acc;
            const Index cnt = c;
            c = acc;      // repurpose as running write cursor
            acc += cnt;
        }
        // Scatter indices into contiguous entries via counting sort.
        for (Index i = 0; i < n; ++i) {
            Index& cur = counts[cell_of_[i]];
            entries_[cur++] = i;
        }
        // Recompute bucket_start (counts now hold end cursors) and store lengths.
        bucket_len_.clear();
        for (auto& [key, end_cur] : counts) {
            const Index start = bucket_start_[key];
            bucket_len_[key] = end_cur - start;
        }
    }

    // Invoke fn(neighbor_index, radius²) for every candidate within `radius` of (px,py).
    // Scans the 3x3 cell block around the point (radius must be <= cell_size).
    template <class Fn>
    void for_each_neighbor(Scalar px, Scalar py, Scalar radius, Fn&& fn) const {
        const auto [cx, cy] = cell_coord(px, py);
        const Scalar r2 = radius * radius;
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                const std::int64_t key = cell_key({cx + dx, cy + dy});
                auto it = bucket_start_.find(key);
                if (it == bucket_start_.end()) continue;
                const Index start = it->second;
                const Index len   = bucket_len_.at(key);
                for (Index k = 0; k < len; ++k) {
                    const Index j = entries_[start + k];
                    fn(j, r2);
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
    [[nodiscard]] static std::int64_t cell_key(CellCoord c) noexcept {
        // Pack two 32-bit signed coords into one 64-bit key.
        return (static_cast<std::int64_t>(static_cast<std::uint32_t>(c.x)) << 32)
             | static_cast<std::uint32_t>(c.y);
    }

    Scalar inv_cell_;
    Scalar cell_size_;
    std::vector<std::int64_t> cell_of_;
    std::vector<Index>        entries_;      // particle ids grouped by cell
    std::unordered_map<std::int64_t, Index> bucket_start_;
    std::unordered_map<std::int64_t, Index> bucket_len_;
};

} // namespace prakriti
