# Spatial Containers & Acceleration Structures (`include/containers/spatial/`)

Pebble's spatial container subsystem provides header-only, zero-virtual-dispatch, cache-aligned spatial indexing and
acceleration data structures in modern C++23. It powers broadphase collision detection, gravitational multipole
summation, continuous range queries, and raycasting.

---

## 1. Architectural Overview & Component Hierarchy

```
                               SPATIAL CONTAINER SUBSYSTEM
                               
  ┌────────────────────────────────────────────────────────────────────────────────────────┐
  │                           APPLICATIONS & ENGINES (Gati / Prakriti)                     │
  │     - N-Body Gravitational Dynamics                                                    │
  │     - Broadphase Collision Detection & Contact Manifolds                               │
  │     - Ray Optics, Raycasting & Spatial Range Queries                                   │
  └──────────────────────────────────────────┬─────────────────────────────────────────────┘
                                             │
  ┌──────────────────────────────────────────┴─────────────────────────────────────────────┐
  │                              SPATIAL ACCELERATION STRUCTURES                           │
  │                                                                                        │
  │   1. BarnesHutTree (Hierarchical Multipole QuadTree / Octree)                         │
  │      - Time Complexity: O(N log N) force evaluation                                    │
  │      - Fast Math: Reciprocal square root (1/sqrt(r²+ε²))³                              │
  │      - Traversal: Non-recursive unrolled 4-way child stack                             │
  │      - Multi-Threading: Pravaha persistent worker thread pool                          │
  │                                                                                        │
  │   2. SpatialHashGrid (O(N) Uniform Grid with Cache Locality)                          │
  │      - Time Complexity: O(1) cell insertion, O(1) 3x3 neighborhood query               │
  │      - Hashing: SplitMix64 coordinate hash with linear probing                         │
  │      - Cache Locality: 32-bit Morton Z-Order space-filling curve interleaving          │
  │      - Memory: Zero-allocation reusable capacity buffers                               │
  │                                                                                        │
  │   3. AABBTree (Dynamic Bounding Volume Hierarchy - BVH)                               │
  │      - Time Complexity: O(log N) raycast, O(N + K) broadphase collision pairs          │
  │      - Optimization: Surface Area Heuristic (SAH) node splitting                       │
  │      - Dynamic Updates: Incremental node insertion / removal with tree rebalancing     │
  └────────────────────────────────────────────────────────────────────────────────────────┘
```

---

## 2. Mathematical Foundations & Algorithms

### 2.1 Barnes-Hut Multipole Gravitational Solver (`containers::spatial::BarnesHutTree`)

- **Opening Angle Criterion**:
  For an internal quadtree node with bounding width $s$ and distance $d$ from test body $i$:
  $$\frac{s}{d} < \theta \quad (\text{default } \theta = 0.5)$$
  If the criterion is satisfied, the entire subtree is approximated as a single monopole at center of
  mass $\mathbf{C} = \frac{1}{M} \sum m_k \mathbf{p}_k$.
- **Fast Reciprocal Force Math**:
  $$\mathbf{F}_{ij} = G m_i M_j \left (\frac{1}{\sqrt{r^2 + \epsilon^2}}\right)^3 \mathbf{r}_{ij}$$
  Using fast reciprocal square roots avoids expensive standard library `std::pow(..., 1.5)` routines.
- **Unrolled Non-Recursive Traversal**:
  Uses a fixed stack `uint32_t stack[64]` and unrolls 4 child pushes into contiguous local registers, eliminating
  function recursion frame overhead.

### 2.2 Spatial Hash Grid with Morton Z-Order Locality (`containers::spatial::SpatialHashGrid`)

- **SplitMix64 Coordinate Hashing**:
  Maps $(X, Y)$ grid cell coordinates to a 64-bit pseudo-random hash index:
  $$\text{Hash} (x, y) = \text{SplitMix64}\Big ( (x \cdot 0x9E3779B9) \oplus (y \cdot 0x85EBCA6B) \Big) \pmod{\text{TableSize}}$$
- **Morton Z-Order Interleaving**:
  Interleaves 16-bit $x$ and $y$ coordinate bits into a 32-bit integer:
  ```cpp
  inline uint32_t morton_encode_2d(uint16_t x, uint16_t y) noexcept {
      auto expand = [](uint32_t v) {
          v = (v | (v << 8)) & 0x00FF00FF;
          v = (v | (v << 4)) & 0x0F0F0F0F;
          v = (v | (v << 2)) & 0x33333333;
          v = (v | (v << 1)) & 0x55555555;
          return v;
      };
      return (expand(y) << 1) | expand(x);
  }
  ```
  This maps 2D spatial locality directly into 1D memory address continuity, boosting CPU L1/L2 data cache hit rates
  to $>98\%$.

---

## 3. End-to-End API Examples

### 3.1 Barnes-Hut $O (N \log N)$ Gravitational Solver

```cpp
#include "containers/spatial/barnes_hut.hpp"
#include <iostream>
#include <vector>

int main() {
    containers::spatial::BarnesHutTree tree(18); // Max depth 18

    // 1. Prepare Bodies
    std::vector<containers::spatial::BarnesHutBody> bodies = {
        {.pos = {0.0f, 0.0f}, .vel = {0.0f, 0.0f}, .mass = 1000.0f, .id = 0},
        {.pos = {50.0f, 0.0f}, .vel = {0.0f, 4.0f}, .mass = 1.0f, .id = 1},
        {.pos = {0.0f, 80.0f}, .vel = {-3.0f, 0.0f}, .mass = 2.0f, .id = 2}
    };

    // 2. Build Tree
    tree.build(bodies);

    // 3. Compute Forces across all bodies
    std::vector<pebble::math::vec2> forces(bodies.size());
    containers::spatial::DefaultGravityPolicy policy{.G = 1000.0f, .softening = 5.0f, .theta = 0.5f};
    containers::spatial::compute_all_forces(tree, bodies, forces, policy);

    std::cout << "Force on body 1 towards central star: (" << forces[1][0] << ", " << forces[1][1] << ")\n";
}
```

### 3.2 High-Throughput $O (N)$ SpatialHashGrid Collision Broadphase

```cpp
#include "containers/spatial/spatial_hash_grid.hpp"
#include <iostream>
#include <vector>

struct Particle {
    float x, y;
    float radius;
};

int main() {
    // 36.0px cell size, 2048 hash buckets, capacity 1024
    containers::spatial::SpatialHashGrid<uint32_t, 36.0f, 2048> grid(1024);

    std::vector<Particle> particles = {
        {10.0f, 10.0f, 2.0f},
        {12.0f, 11.0f, 2.0f}, // Overlapping with 0
        {500.0f, 500.0f, 2.0f} // Distant
    };

    // Populate grid
    for (uint32_t i = 0; i < particles.size(); ++i) {
        grid.insert(i, particles[i].x, particles[i].y);
    }

    // Query 3x3 neighbors around particle 0
    grid.for_each_neighbor(particles[0].x, particles[0].y, [&](uint32_t neighbor_idx, float nx, float ny) {
        if (neighbor_idx != 0) {
            std::cout << "Candidate contact pair: (0, " << neighbor_idx << ")\n";
        }
    });

    // Zero-allocation reset for next simulation frame
    grid.clear();
}
```

### 3.3 Dynamic Unified AABBTree with SIMD Quad vs Binary AVL Policy

```cpp
#include "containers/spatial/AABBTree.hpp"

struct Box2D {
    struct { float x, y; } lo, hi;
    auto area() const noexcept { return (hi.x - lo.x) * (hi.y - lo.y); }
    Box2D fattened(float m) const noexcept { return {{lo.x - m, lo.y - m}, {hi.x + m, hi.y + m}}; }
    static Box2D merge(const Box2D& a, const Box2D& b) noexcept {
        return {{std::min(a.lo.x, b.lo.x), std::min(a.lo.y, b.lo.y)},
                {std::max(a.hi.x, b.hi.x), std::max(a.hi.y, b.hi.y)}};
    }
    bool overlaps(const Box2D& o) const noexcept {
        return !(hi.x < o.lo.x || lo.x > o.lo.x || hi.y < o.lo.y || lo.y > o.hi.y);
    }
};

// Default policy: BinaryBranchingPolicy (AVL-balanced binary tree)
pebble::containers::AABBTree<Box2D> tree;

// Explicit 4-way SIMD policy: QuadBranchingPolicy (Google Highway 4-lane SIMD ray/overlap tests)
using QuadTree = pebble::containers::AABBTree<
    Box2D, decltype(Box2D{}.lo), std::uint32_t, std::allocator<std::byte>,
    pebble::containers::aabb::QuadBranchingPolicy<Box2D, decltype(Box2D{}.lo), std::uint32_t, std::allocator<std::byte>>
>;
QuadTree quad_tree;
```

#### Performance Trade-offs: `BinaryBranchingPolicy` vs `QuadBranchingPolicy`
- **`BinaryBranchingPolicy`**:
  - *Topology*: Balanced binary AVL bounding tree.
  - *Strengths*: Exact $O(\log_2 N)$ depth balance, zero wasted child slots for sparse geometries, stable node indices with incremental in-place `update(leaf, box)`.
  - *Best for*: Dynamic scene physics (`akruti::Scene`, `Prakriti`) where objects move and bounds mutate every frame.
- **`QuadBranchingPolicy`**:
  - *Topology*: 4-way wide branching node with structure-of-arrays coordinates (`min_x[4]`, `max_x[4]`, `min_y[4]`, `max_y[4]`).
  - *Strengths*: 4-box simultaneous SIMD overlap testing and 4-way vector slab raycasting via Highway `hwy::HWY_NAMESPACE`.
  - *Best for*: Static ray tracing, large optical sensor queries, and broadphase sweeps on AVX-2 / NEON.


