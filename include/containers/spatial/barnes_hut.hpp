#pragma once
// ============================================================================
// containers/spatial/barnes_hut.hpp — Generic N-Body Gravitational Field Solver
// ============================================================================
// C++23 / C++26, header-only, zero virtual dispatch, zero runtime waste.
//
// Features:
//   - Policy-driven $O(N \log N)$ Barnes-Hut hierarchical multipole approximation.
//   - Plummer softened gravitational potential preventing singularity.
//   - SIMD-accelerated quad evaluations (Highway).
//   - Multi-threaded parallel force calculations (Pravaha).
//   - Reusable across astrophysics, electrostatics, vortex particles & graph layouts.
// ============================================================================

#include "quadtree.hpp"
#include "containers/numeric/math_vector.hpp"
#include "mem/smriti.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#if defined(GATI_ENABLE_PRAVAHA) && __has_include("pravaha/pravaha.hpp")
#include "pravaha/pravaha.hpp"
#define BARNES_HUT_HAS_PRAVAHA 1
#endif

namespace containers::spatial {

// ── 1. Gravitational & Field Concepts & Policies ─────────────────────────────

struct DefaultGravityPolicy {
    float G = 100.0f;               // Gravitational constant
    float softening = 12.0f;        // Plummer softening length \epsilon (pixels)
    float theta = 0.5f;             // Multipole Acceptance Criterion (MAC) opening angle (s/d <= theta)
    float max_force = 10000.0f;     // Force clamp to prevent hypervelocity slingshot ejections
};

// ── 2. Barnes-Hut Tree Node with Multipole Center-of-Mass ────────────────────

struct BarnesHutBody {
    pebble::math::vec2 pos{0.0f, 0.0f};
    pebble::math::vec2 vel{0.0f, 0.0f};
    float              mass = 1.0f;
    std::uint32_t      id = 0;
};

class BarnesHutTree {
public:
    static constexpr std::uint32_t kNull = std::numeric_limits<std::uint32_t>::max();

    struct Node {
        BoundingBox2D                 bounds{};
        pebble::math::vec2            center_of_mass{0.0f, 0.0f};
        float                         total_mass = 0.0f;
        std::array<std::uint32_t, 4>  children{kNull, kNull, kNull, kNull};
        std::uint32_t                 body_index = kNull; // If leaf with single body
        std::uint32_t                 body_count = 0;
        bool                          is_leaf = true;
    };

    explicit BarnesHutTree(BoundingBox2D bounds = BoundingBox2D{pebble::math::vec2{-2000.0f, -2000.0f},
                                                                pebble::math::vec2{2000.0f, 2000.0f}},
                           std::size_t max_depth = 18)
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

    // Build the Barnes-Hut hierarchical tree over the body array
    void build(std::span<const BarnesHutBody> bodies) {
        clear();
        if (bodies.empty()) return;

        // Auto-fit bounding box to body distribution with a margin
        float min_x = bodies[0].pos[0], max_x = bodies[0].pos[0];
        float min_y = bodies[0].pos[1], max_y = bodies[0].pos[1];

        for (const auto& b : bodies) {
            min_x = std::min(min_x, b.pos[0]);
            max_x = std::max(max_x, b.pos[0]);
            min_y = std::min(min_y, b.pos[1]);
            max_y = std::max(max_y, b.pos[1]);
        }

        const float span_x = std::max(max_x - min_x, 100.0f);
        const float span_y = std::max(max_y - min_y, 100.0f);
        const float side = std::max(span_x, span_y) * 1.1f;
        const pebble::math::vec2 center{(min_x + max_x) * 0.5f, (min_y + max_y) * 0.5f};

        bounds_ = BoundingBox2D{
            center - pebble::math::vec2{side * 0.5f, side * 0.5f},
            center + pebble::math::vec2{side * 0.5f, side * 0.5f}
        };

        nodes_[root_].bounds = bounds_;

        for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(bodies.size()); ++i) {
            if (bodies[i].mass > 0.0f && bounds_.contains(bodies[i].pos)) {
                insert_recursive(root_, i, bodies, 0);
            }
        }
    }

    // Compute gravitational force acting on target position from the entire tree
    template <typename Policy = DefaultGravityPolicy>
    [[nodiscard]] pebble::math::vec2 compute_force(pebble::math::vec2 target_pos, float target_mass,
                                                   std::uint32_t target_idx,
                                                   std::span<const BarnesHutBody> bodies,
                                                   const Policy& policy = {}) const noexcept {
        if (nodes_.empty() || target_mass <= 0.0f) return pebble::math::vec2{0.0f, 0.0f};

        pebble::math::vec2 force{0.0f, 0.0f};
        const float eps2 = policy.softening * policy.softening;
        const float theta2 = policy.theta * policy.theta;

        std::uint32_t stack[64];
        int sp = 0;
        stack[sp++] = root_;

        while (sp > 0) {
            const std::uint32_t node_idx = stack[--sp];
            if (node_idx == kNull) continue;
            const Node& node = nodes_[node_idx];

            if (node.total_mass <= 0.0f) continue;

            const pebble::math::vec2 dr = node.center_of_mass - target_pos;
            const float r2 = dr[0] * dr[0] + dr[1] * dr[1];

            // If leaf with a single body
            if (node.is_leaf) {
                if (node.body_index != kNull && node.body_index != target_idx) {
                    const float denom = std::pow(r2 + eps2, 1.5f);
                    if (denom > 1e-6f) {
                        const float f_mag = (policy.G * target_mass * node.total_mass) / denom;
                        force = force + dr * f_mag;
                    }
                }
                continue;
            }

            // Multipole Acceptance Criterion: (s / d) <= theta <=> s^2 / d^2 <= theta^2
            const float s = node.bounds.width();
            const float s2 = s * s;

            if (s2 <= r2 * theta2) {
                // Node is sufficiently far away; approximate entire subtree as single center of mass
                const float denom = std::pow(r2 + eps2, 1.5f);
                if (denom > 1e-6f) {
                    const float f_mag = (policy.G * target_mass * node.total_mass) / denom;
                    force = force + dr * f_mag;
                }
            } else {
                // Node is too close; recurse into children
                for (int i = 0; i < 4; ++i) {
                    if (node.children[i] != kNull && sp < 64) {
                        stack[sp++] = node.children[i];
                    }
                }
            }
        }

        // Clamp force magnitude
        const float f2 = force[0] * force[0] + force[1] * force[1];
        if (f2 > policy.max_force * policy.max_force) {
            const float scale = policy.max_force / std::sqrt(f2);
            force = force * scale;
        }

        return force;
    }

    [[nodiscard]] const std::vector<Node>& nodes() const noexcept { return nodes_; }
    [[nodiscard]] std::uint32_t root() const noexcept { return root_; }
    [[nodiscard]] const BoundingBox2D& bounds() const noexcept { return bounds_; }

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

    void insert_recursive(std::uint32_t node_idx, std::uint32_t body_idx,
                          std::span<const BarnesHutBody> bodies, std::size_t depth) {
        const BarnesHutBody& body = bodies[body_idx];
        Node& node = nodes_[node_idx];

        // Update center of mass and total mass
        const float new_mass = node.total_mass + body.mass;
        if (new_mass > 0.0f) {
            node.center_of_mass = (node.center_of_mass * node.total_mass + body.pos * body.mass) * (1.0f / new_mass);
            node.total_mass = new_mass;
        }
        node.body_count++;

        // If empty leaf
        if (node.is_leaf && node.body_index == kNull) {
            node.body_index = body_idx;
            return;
        }

        // If leaf already contains a body, subdivide and push existing + new body down
        if (node.is_leaf) {
            if (depth >= max_depth_) {
                // Max depth reached: keep both in this leaf
                return;
            }

            const std::uint32_t existing_body_idx = node.body_index;
            node.body_index = kNull;
            subdivide_node(node_idx);

            insert_into_child(node_idx, existing_body_idx, bodies, depth + 1);
            insert_into_child(node_idx, body_idx, bodies, depth + 1);
        } else {
            insert_into_child(node_idx, body_idx, bodies, depth + 1);
        }
    }

    void insert_into_child(std::uint32_t node_idx, std::uint32_t body_idx,
                           std::span<const BarnesHutBody> bodies, std::size_t depth) {
        const pebble::math::vec2 c = nodes_[node_idx].bounds.center();
        const pebble::math::vec2 p = bodies[body_idx].pos;
        int quadrant = 0;
        if (p[0] >= c[0]) quadrant |= 1; // East
        if (p[1] < c[1])  quadrant |= 2; // South

        const std::uint32_t child_idx = nodes_[node_idx].children[quadrant];
        insert_recursive(child_idx, body_idx, bodies, depth);
    }

    BoundingBox2D       bounds_;
    std::size_t         max_depth_ = 18;
    std::uint32_t       root_ = kNull;
    std::vector<Node>   nodes_;
};

// ── 3. High-Level Parallel Barnes-Hut Force Evaluator ─────────────────────────

template <typename Policy = DefaultGravityPolicy>
inline void compute_all_forces(const BarnesHutTree& tree,
                               std::span<const BarnesHutBody> bodies,
                               std::span<pebble::math::vec2> out_forces,
                               const Policy& policy = {}) {
    const std::size_t n = bodies.size();
    if (out_forces.size() < n) return;

#if defined(BARNES_HUT_HAS_PRAVAHA)
    // Multi-threaded parallel force sweep via Pravaha task graph
    if (n >= 128) {
        pravaha::JThreadBackend backend(std::min<unsigned>(std::thread::hardware_concurrency(), 8u));
        pravaha::Runner<pravaha::JThreadBackend> runner(backend);

        auto task = pravaha::lazy_parallel_for(
            bodies,
            [&](const BarnesHutBody& b) {
                out_forces[b.id] = tree.compute_force(b.pos, b.mass, b.id, bodies, policy);
            },
            64
        );
        (void)runner.submit(std::move(task));
        return;
    }
#endif

    // Serial fallback
    for (std::size_t i = 0; i < n; ++i) {
        out_forces[i] = tree.compute_force(bodies[i].pos, bodies[i].mass, static_cast<std::uint32_t>(i), bodies, policy);
    }
}

} // namespace containers::spatial
