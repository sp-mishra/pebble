#pragma once
// akruti/spatial_hash.hpp — Generic dynamic-resolution Spatial Hash with Morton Z-order and 3x3 neighbor queries.
#include "math.hpp"
#include "broadphase_concepts.hpp"
#include <span>
#include <vector>
#include <cmath>
#include <cstdint>
#include <algorithm>

namespace akruti {

struct MortonOrder {};
struct LinearOrder {};

template <typename Payload = std::uint32_t, class Order = MortonOrder>
class SpatialHash {
public:
    static constexpr std::uint32_t kTableSize = 4096;
    static constexpr std::uint32_t kTableMask = kTableSize - 1;
    static constexpr std::uint32_t kInvalid = static_cast<std::uint32_t>(-1);

    explicit SpatialHash(Scalar cell_size = Scalar(1))
        : inv_cell_(Scalar(1) / std::max(cell_size, Scalar(1e-4))), cell_size_(cell_size) {
        head_.assign(kTableSize, kInvalid);
    }

    void set_cell_size(Scalar s) noexcept {
        cell_size_ = std::max(s, Scalar(1e-4));
        inv_cell_ = Scalar(1) / cell_size_;
    }
    [[nodiscard]] Scalar cell_size() const noexcept { return cell_size_; }

    void clear() noexcept {
        std::fill(head_.begin(), head_.end(), kInvalid);
        next_.clear();
        cell_of_.clear();
        payloads_.clear();
        boxes_.clear();
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return payloads_.size();
    }

    // Insert entity AABB and payload ID
    uint32_t insert(AABB<Scalar> box, Payload id) {
        const uint32_t index = static_cast<uint32_t>(payloads_.size());
        payloads_.push_back(id);
        boxes_.push_back(box);
        next_.push_back(kInvalid);

        const Vec2<Scalar> center = Vec2<Scalar>((box.lo[0] + box.hi[0]) * 0.5f,
                                                 (box.lo[1] + box.hi[1]) * 0.5f);
        const auto [cx, cy] = cell_coord(center.x, center.y);
        cell_of_.push_back(pack_coords(cx, cy));

        const std::uint32_t slot = hash_coords(cx, cy);
        next_[index] = head_[slot];
        head_[slot] = index;

        return index;
    }

    void remove(uint32_t id) {
        // Soft removal by marking invalid or rebuilding
        for (std::size_t i = 0; i < payloads_.size(); ++i) {
            if (payloads_[i] == id) {
                payloads_[i] = static_cast<Payload>(-1);
                break;
            }
        }
    }

    bool update(uint32_t id, AABB<Scalar> box) {
        for (std::size_t i = 0; i < payloads_.size(); ++i) {
            if (payloads_[i] == id) {
                boxes_[i] = box;
                return true;
            }
        }
        return false;
    }

    // Full batch rebuild from spans with automatic dynamic cell sizing if requested
    void rebuild(std::span<const AABB<Scalar>> boxes, std::span<const Payload> ids, bool auto_cell_size = false) {
        const std::size_t n = std::min(boxes.size(), ids.size());
        if (auto_cell_size && n > 0) {
            // Compute median radius
            std::vector<Scalar> radii(n);
            for (std::size_t i = 0; i < n; ++i) {
                const Scalar dx = (boxes[i].hi[0] - boxes[i].lo[0]) * 0.5f;
                const Scalar dy = (boxes[i].hi[1] - boxes[i].lo[1]) * 0.5f;
                radii[i] = std::max(dx, dy);
            }
            std::nth_element(radii.begin(), radii.begin() + n / 2, radii.end());
            const Scalar median_r = radii[n / 2];
            set_cell_size(std::max(Scalar(1), median_r * Scalar(2)));
        }

        payloads_.assign(ids.begin(), ids.begin() + n);
        boxes_.assign(boxes.begin(), boxes.begin() + n);
        cell_of_.resize(n);
        next_.resize(n);
        std::fill(head_.begin(), head_.end(), kInvalid);

        for (std::size_t i = 0; i < n; ++i) {
            const Vec2<Scalar> center = Vec2<Scalar>((boxes[i].lo[0] + boxes[i].hi[0]) * 0.5f,
                                                     (boxes[i].lo[1] + boxes[i].hi[1]) * 0.5f);
            const auto [cx, cy] = cell_coord(center.x, center.y);
            cell_of_[i] = pack_coords(cx, cy);

            const std::uint32_t slot = hash_coords(cx, cy);
            next_[i] = head_[slot];
            head_[slot] = static_cast<std::uint32_t>(i);
        }
    }

    template <class Fn>
    void query(AABB<Scalar> box, Fn&& fn) const {
        const auto [min_cx, min_cy] = cell_coord(box.lo[0], box.lo[1]);
        const auto [max_cx, max_cy] = cell_coord(box.hi[0], box.hi[1]);

        for (std::int32_t cy = min_cy; cy <= max_cy; ++cy) {
            for (std::int32_t cx = min_cx; cx <= max_cx; ++cx) {
                const std::uint32_t target_packed = pack_coords(cx, cy);
                const std::uint32_t slot = hash_coords(cx, cy);

                std::uint32_t cur = head_[slot];
                std::uint32_t guard = 0;
                while (cur != kInvalid && guard++ < 1024) {
                    if (cell_of_[cur] == target_packed) {
                        if (payloads_[cur] != static_cast<Payload>(-1) && boxes_[cur].overlaps(box)) {
                            fn(payloads_[cur]);
                        }
                    }
                    cur = next_[cur];
                }
            }
        }
    }

    template <class Fn>
    void raycast(Vec2<Scalar> origin, Vec2<Scalar> dir, Scalar max_t, Fn&& fn) const {
        // DDA or bounding box query
        AABB<Scalar> ray_box{
            Vec2<Scalar>{std::min(origin.x, origin.x + dir.x * max_t),
                         std::min(origin.y, origin.y + dir.y * max_t)},
            Vec2<Scalar>{std::max(origin.x, origin.x + dir.x * max_t),
                         std::max(origin.y, origin.y + dir.y * max_t)}
        };
        query(ray_box, fn);
    }

    template <class Fn>
    void for_each_neighbor(Scalar px, Scalar py, Scalar radius, Fn&& fn) const {
        if (!std::isfinite(px) || !std::isfinite(py)) return;
        const auto [cx, cy] = cell_coord(px, py);
        const Scalar r2 = radius * radius;

        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                const int nx = cx + dx;
                const int ny = cy + dy;
                const std::uint32_t target_packed = pack_coords(nx, ny);
                const std::uint32_t slot = hash_coords(nx, ny);

                std::uint32_t cur = head_[slot];
                std::uint32_t guard = 0;
                while (cur != kInvalid && guard++ < 512) {
                    if (cell_of_[cur] == target_packed && payloads_[cur] != static_cast<Payload>(-1)) {
                        fn(payloads_[cur], r2);
                    }
                    cur = next_[cur];
                }
            }
        }
    }

    struct CellCoord { std::int32_t x, y; };

    [[nodiscard]] CellCoord cell_coord(Scalar px, Scalar py) const noexcept {
        if (!std::isfinite(px) || !std::isfinite(py)) return {0, 0};
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
        const std::uint32_t m = morton2d(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
        return (m ^ (m >> 12)) & kTableMask;
    }

    Scalar inv_cell_{1.0f};
    Scalar cell_size_{1.0f};
    std::vector<std::uint32_t> head_;
    std::vector<std::uint32_t> next_;
    std::vector<std::uint32_t> cell_of_;
    std::vector<Payload> payloads_;
    std::vector<AABB<Scalar>> boxes_;
};

using SpatialHashBroadphase = SpatialHash<std::uint32_t, MortonOrder>;

} // namespace akruti
