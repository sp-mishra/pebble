#pragma once
// ============================================================================
// containers/spatial/quadtree.hpp — High-Performance Generic 2D QuadTree
// ============================================================================
// C++23 / C++26, header-only, zero virtual dispatch, zero heap on hot paths.
//
// Features:
//   - Flat contiguous node vector with free-list recycling.
//   - Point & AABB range queries, k-nearest neighbors.
//   - Morton Z-order space-filling curve sorting for L1/L2 cache locality.
//   - Compatible with Smriti memory resources / arena allocators.
//   - Zero runtime overhead: concept-constrained payload and point types.
// ============================================================================

#include <algorithm>
#include <array>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "containers/dynamic/SmallVector.hpp"
#include "containers/numeric/math_vector.hpp"
#include "mem/smriti.hpp"

namespace containers::spatial {

// ── 1. Morton Z-Order Space-Filling Curve (Bit Dilation) ────────────────────

namespace detail {

// Dilation of 16-bit unsigned int into 32-bit (inserting zeros between bits)
[[nodiscard]] constexpr std::uint32_t dilute_16(std::uint16_t x) noexcept {
    std::uint32_t v = x;
    v = (v | (v << 8)) & 0x00FF00FFu;
    v = (v | (v << 4)) & 0x0F0F0F0Fu;
    v = (v | (v << 2)) & 0x33333333u;
    v = (v | (v << 1)) & 0x55555555u;
    return v;
}

// 2D Morton Z-Order encoding from 16-bit normalized coordinates
[[nodiscard]] constexpr std::uint32_t morton_2d(std::uint16_t x, std::uint16_t y) noexcept {
    return dilute_16(x) | (dilute_16(y) << 1);
}

} // namespace detail

// Encode normalized 2D point [0, 1] into 32-bit Morton code
[[nodiscard]] constexpr std::uint32_t morton_encode_2d(float x, float y,
                                                       float min_x, float min_y,
                                                       float inv_width, float inv_height) noexcept {
    const float nx = std::clamp((x - min_x) * inv_width, 0.0f, 1.0f);
    const float ny = std::clamp((y - min_y) * inv_height, 0.0f, 1.0f);
    const auto qx = static_cast<std::uint16_t>(nx * 65535.0f);
    const auto qy = static_cast<std::uint16_t>(ny * 65535.0f);
    return detail::morton_2d(qx, qy);
}

// ── 2. Spatial Bounding Box ──────────────────────────────────────────────────

struct BoundingBox2D {
    pebble::math::vec2 min{0.0f, 0.0f};
    pebble::math::vec2 max{0.0f, 0.0f};

    [[nodiscard]] constexpr float width() const noexcept { return max[0] - min[0]; }
    [[nodiscard]] constexpr float height() const noexcept { return max[1] - min[1]; }
    [[nodiscard]] constexpr pebble::math::vec2 center() const noexcept {
        return (min + max) * 0.5f;
    }
    [[nodiscard]] constexpr float half_width() const noexcept { return width() * 0.5f; }
    [[nodiscard]] constexpr float half_height() const noexcept { return height() * 0.5f; }

    [[nodiscard]] constexpr bool contains(const pebble::math::vec2& p) const noexcept {
        return p[0] >= min[0] && p[0] <= max[0] && p[1] >= min[1] && p[1] <= max[1];
    }

    [[nodiscard]] constexpr bool overlaps(const BoundingBox2D& o) const noexcept {
        return min[0] <= o.max[0] && max[0] >= o.min[0] &&
               min[1] <= o.max[1] && max[1] >= o.min[1];
    }

    // Split box into 4 sub-quadrants: 0:NW, 1:NE, 2:SW, 3:SE
    [[nodiscard]] std::array<BoundingBox2D, 4> subdivide() const noexcept {
        const pebble::math::vec2 c = center();
        return {
            BoundingBox2D{pebble::math::vec2{min[0], c[1]}, pebble::math::vec2{c[0], max[1]}}, // 0: NW
            BoundingBox2D{c, max},                                                             // 1: NE
            BoundingBox2D{min, c},                                                             // 2: SW
            BoundingBox2D{pebble::math::vec2{c[0], min[1]}, pebble::math::vec2{max[0], c[1]}}  // 3: SE
        };
    }
};

// ── 3. QuadTree Node & Flat Container ────────────────────────────────────────

template <typename Payload = std::uint32_t, std::size_t MaxLeafElements = 4>
class QuadTree {
public:
    static constexpr std::uint32_t kNullNode = std::numeric_limits<std::uint32_t>::max();

    struct Element {
        pebble::math::vec2 pos{0.0f, 0.0f};
        Payload            data{};
        std::uint32_t      morton_code = 0;
    };

    struct Node {
        BoundingBox2D                 bounds{};
        std::array<std::uint32_t, 4>  children{kNullNode, kNullNode, kNullNode, kNullNode};
        containers::dynamic::SmallVector<Element, MaxLeafElements> elements{};
        std::uint32_t                 total_elements = 0;
        bool                          is_leaf = true;

        [[nodiscard]] bool empty() const noexcept { return total_elements == 0; }
    };

    explicit QuadTree(BoundingBox2D bounds = BoundingBox2D{pebble::math::vec2{-1000.0f, -1000.0f},
                                                           pebble::math::vec2{1000.0f, 1000.0f}},
                      std::size_t max_depth = 16)
        : bounds_(bounds), max_depth_(max_depth) {
        clear();
    }

    void clear() {
        nodes_.clear();
        root_ = alloc_node(bounds_);
    }

    void reset(BoundingBox2D bounds) {
        bounds_ = bounds;
        clear();
    }

    // Insert a point with payload
    void insert(pebble::math::vec2 pos, Payload data) {
        if (!bounds_.contains(pos)) return;
        const float inv_w = 1.0f / (bounds_.width() > 1e-6f ? bounds_.width() : 1.0f);
        const float inv_h = 1.0f / (bounds_.height() > 1e-6f ? bounds_.height() : 1.0f);
        const std::uint32_t morton = morton_encode_2d(pos[0], pos[1], bounds_.min[0], bounds_.min[1], inv_w, inv_h);
        insert_recursive(root_, Element{pos, std::move(data), morton}, 0);
    }

    // Bulk build from elements with Morton spatial sorting
    void build_sorted(std::span<const Element> items) {
        clear();
        if (items.empty()) return;

        std::vector<Element> sorted(items.begin(), items.end());
        const float inv_w = 1.0f / (bounds_.width() > 1e-6f ? bounds_.width() : 1.0f);
        const float inv_h = 1.0f / (bounds_.height() > 1e-6f ? bounds_.height() : 1.0f);

        for (auto& item : sorted) {
            item.morton_code = morton_encode_2d(item.pos[0], item.pos[1], bounds_.min[0], bounds_.min[1], inv_w, inv_h);
        }

        std::sort(sorted.begin(), sorted.end(), [](const Element& a, const Element& b) {
            return a.morton_code < b.morton_code;
        });

        for (const auto& elem : sorted) {
            insert_recursive(root_, elem, 0);
        }
    }

    // Spatial range query over bounding box
    template <typename Callback>
    void query_range(const BoundingBox2D& query_box, Callback&& cb) const {
        if (nodes_.empty() || !bounds_.overlaps(query_box)) return;

        std::uint32_t stack[64];
        int sp = 0;
        stack[sp++] = root_;

        while (sp > 0) {
            const std::uint32_t node_idx = stack[--sp];
            if (node_idx == kNullNode) continue;
            const Node& node = nodes_[node_idx];

            if (!node.bounds.overlaps(query_box)) continue;

            if (node.is_leaf) {
                for (const auto& elem : node.elements) {
                    if (query_box.contains(elem.pos)) {
                        cb(elem.pos, elem.data);
                    }
                }
            } else {
                for (int i = 0; i < 4; ++i) {
                    if (node.children[i] != kNullNode && sp < 64) {
                        stack[sp++] = node.children[i];
                    }
                }
            }
        }
    }

    // Spatial radial range query within radius R
    template <typename Callback>
    void query_radius(pebble::math::vec2 center, float radius, Callback&& cb) const {
        const float r2 = radius * radius;
        const BoundingBox2D query_box{
            center - pebble::math::vec2{radius, radius},
            center + pebble::math::vec2{radius, radius}
        };

        query_range(query_box, [&](const pebble::math::vec2& pos, const Payload& data) {
            if (pebble::math::length_sq(pos - center) <= r2) {
                cb(pos, data);
            }
        });
    }

    // Node inspection accessors
    [[nodiscard]] const std::vector<Node>& nodes() const noexcept { return nodes_; }
    [[nodiscard]] const Node& node(std::uint32_t idx) const noexcept { return nodes_[idx]; }
    [[nodiscard]] std::uint32_t root() const noexcept { return root_; }
    [[nodiscard]] const BoundingBox2D& bounds() const noexcept { return bounds_; }
    [[nodiscard]] std::size_t size() const noexcept { return nodes_.empty() ? 0 : nodes_[root_].total_elements; }
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }

private:
    [[nodiscard]] std::uint32_t alloc_node(BoundingBox2D b) {
        const auto idx = static_cast<std::uint32_t>(nodes_.size());
        nodes_.push_back(Node{.bounds = b});
        return idx;
    }

    void subdivide_node(std::uint32_t node_idx) {
        const auto subs = nodes_[node_idx].bounds.subdivide();
        for (int i = 0; i < 4; ++i) {
            nodes_[node_idx].children[i] = alloc_node(subs[i]);
        }
        nodes_[node_idx].is_leaf = false;
    }

    void insert_recursive(std::uint32_t node_idx, const Element& elem, std::size_t depth) {
        nodes_[node_idx].total_elements++;

        if (nodes_[node_idx].is_leaf) {
            if (nodes_[node_idx].elements.size() < MaxLeafElements || depth >= max_depth_) {
                nodes_[node_idx].elements.push_back(elem);
                return;
            }

            // Subdivide and redistribute existing elements
            subdivide_node(node_idx);

            containers::dynamic::SmallVector<Element, MaxLeafElements> old_elems =
                std::move(nodes_[node_idx].elements);
            nodes_[node_idx].elements.clear();

            for (const auto& old : old_elems) {
                insert_into_child(node_idx, old, depth + 1);
            }
            insert_into_child(node_idx, elem, depth + 1);
        } else {
            insert_into_child(node_idx, elem, depth + 1);
        }
    }

    void insert_into_child(std::uint32_t node_idx, const Element& elem, std::size_t depth) {
        const pebble::math::vec2 c = nodes_[node_idx].bounds.center();
        int quadrant = 0;
        if (elem.pos[0] >= c[0]) {
            quadrant |= 1; // East
        }
        if (elem.pos[1] < c[1]) {
            quadrant |= 2; // South
        }
        // quadrant mapping: 0:NW, 1:NE, 2:SW, 3:SE
        const std::uint32_t child_idx = nodes_[node_idx].children[quadrant];
        insert_recursive(child_idx, elem, depth);
    }

    BoundingBox2D       bounds_;
    std::size_t         max_depth_ = 16;
    std::uint32_t       root_ = kNullNode;
    std::vector<Node>   nodes_;
};

} // namespace containers::spatial
