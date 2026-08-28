# Barnes-Hut N-Body Gravitational Field Solver & QuadTree

Header-only C++23/C++26 generic spatial tree and multipole gravitational solver.
No virtual dispatch, no heap on hot paths, concept-based static polymorphism.

Includes:
```cpp
#include <containers/spatial/quadtree.hpp>
#include <containers/spatial/barnes_hut.hpp>
```

---

## 1. Overview & Mathematical Foundations

### QuadTree
`containers::spatial::QuadTree<Payload, MaxLeafElements>` is a flat-array 2D spatial tree supporting $O(\log N)$ spatial range and radial queries with Morton Z-order space-filling curve sorting for optimal hardware L1/L2 cache spatial locality.

### Barnes-Hut N-Body Solver
`containers::spatial::BarnesHutTree` reduces the classical $O(N^2)$ direct all-pairs gravitational summation to $O(N \log N)$ by recursively grouping distant clusters into single equivalent center-of-mass multipole nodes.

$$\vec{R}_{\text{com}} = \frac{\sum m_i \vec{r}_i}{\sum m_i}, \qquad M = \sum m_i$$

### Multipole Acceptance Criterion (MAC)
For a node of width $s$ at distance $d = \|\vec{r} - \vec{R}_{\text{com}}\|$:
$$\frac{s}{d} \le \theta \quad (\text{Default } \theta = 0.5)$$

### Softened Plummer Potential
$$\vec{F}_{ij} = G \frac{m_i m_j}{(\|\vec{r}_j - \vec{r}_i\|^2 + \epsilon^2)^{3/2}} (\vec{r}_j - \vec{r}_i)$$

---

## 2. Quick Start Example

```cpp
#include <containers/spatial/barnes_hut.hpp>
#include <vector>

std::vector<containers::spatial::BarnesHutBody> bodies = {
    {.pos = {0.0f, 0.0f}, .vel = {0.0f, 0.0f}, .mass = 1000.0f, .id = 0},
    {.pos = {100.0f, 0.0f}, .vel = {0.0f, 15.0f}, .mass = 10.0f, .id = 1}
};

containers::spatial::BarnesHutTree tree;
tree.build(bodies);

std::vector<pebble::math::vec2> forces(bodies.size());
containers::spatial::DefaultGravityPolicy policy{.G = 100.0f, .softening = 10.0f, .theta = 0.5f};
containers::spatial::compute_all_forces(tree, bodies, forces, policy);
```
