# Structure-of-Arrays (SoA) & Inline Vector Containers

Pebble provides high-performance data-oriented array containers in `include/containers/dynamic/soa_vector.hpp` and `include/containers/static/static_vector.hpp`. They are engineered for SIMD vectorization, zero cache pollution, and predictable memory footprints in physics engines and numerical simulations.

---

## 1. Architectural Overview & Memory Layouts

```
                      Array of Structures (AoS) vs Structure of Arrays (SoA)
                      
   AoS (Traditional std::vector<Particle>):
   ┌───────────────────────┬───────────────────────┬───────────────────────┐
   │ x | y | vx | vy | m   │ x | y | vx | vy | m   │ x | y | vx | vy | m   │  (Cache lines polluted with
   └───────────────────────┴───────────────────────┴───────────────────────┘   unused mass during integration)
   
   SoA (Pebble SoAVector<x, y, vx, vy, m>):
   ┌────────────────────────────────────────────────────────┐
   │ x0  | x1  | x2  | x3  | x4  | x5  | x6  | x7  | ...    │  <-- 100% Contiguous SIMD Load
   ├────────────────────────────────────────────────────────┤
   │ y0  | y1  | y2  | y3  | y4  | y5  | y6  | y7  | ...    │  <-- 100% Contiguous SIMD Load
   ├────────────────────────────────────────────────────────┤
   │ vx0 | vx1 | vx2 | vx3 | vx4 | vx5 | vx6 | vx7 | ...    │  <-- 100% Contiguous SIMD Load
   ├────────────────────────────────────────────────────────┤
   │ vy0 | vy1 | vy2 | vy3 | vy4 | vy5 | vy6 | vy7 | ...    │  <-- 100% Contiguous SIMD Load
   ├────────────────────────────────────────────────────────┤
   │ m0  | m1  | m2  | m3  | m4  | m5  | m6  | m7  | ...    │
   └────────────────────────────────────────────────────────┘
   All columns 64-byte aligned (AlignedSoAVector) → vmovaps, no store-split penalty
```

---

## 2. Storage Policies

| Storage Policy | Memory Location | Alignment | Best Use Case |
|:---|:---|:---|:---|
| **`StaticStoragePolicy<N>`** | Stack | Default | Small particle clusters, contact manifolds ($N \le 64$) |
| **`SmallVectorStoragePolicy<N>`** | Inline SBO → Heap | Default | Dynamic physics islands, local query results |
| **`DynamicStoragePolicy<Alloc>`** | Heap / PMR Arena | Default | Global particle systems, massive entity pools |
| **`Aligned32StoragePolicy`** | Heap | **32-byte** | AVX2 — enables `vmovaps`, eliminates unaligned load penalties |
| **`Aligned64StoragePolicy`** | Heap | **64-byte** | AVX-512 / cache-line — optimal store alignment |

`AlignedStoragePolicy` is an alias for `Aligned64StoragePolicy`.

---

## 3. SIMD Vectorized Kinematics & Velocity Verlet

Built-in `integrate_verlet_simd(dt)` (float or double) on columns [0]=x [1]=y [2]=vx [3]=vy [4]=ax [5]=ay:
$$\mathbf{p}_{n+1} = \mathbf{p}_n + \mathbf{v}_n \Delta t + \frac{1}{2} \mathbf{a}_n \Delta t^2 \qquad \mathbf{v}_{n+1} = \mathbf{v}_n + \mathbf{a}_n \Delta t$$

`transform_columns<Is...>(fn)` dispatches any user-supplied SIMD kernel with aligned raw pointers — the kernel owns the loop, `SoAVector` guarantees contiguous aligned storage.

Portable `SOA_VECTORIZE` macro (Clang / GCC / MSVC) enables auto-vectorization on all compilers.

---

## 4. Full API Reference

| Method | Description |
|:---|:---|
| `push_back(vals...)` | Append one row |
| `pop_back()` | Remove last row |
| `resize(n)` | Fill to n, default-construct |
| `resize(n, vals...)` | Fill to n with provided values |
| `swap_pop_back(i)` | O(1) removal — swap with last, pop |
| `erase_if(pred)` | Bulk O(N) removal; returns count |
| `append_range(other)` | O(M) batch merge (any storage policy) |
| `get_column<I>()` | `std::span` over column I |
| `data<I>()` | Raw pointer to column I data |
| `get<I>(i)` | Single element access at row i |
| `row(i)` | All columns at row i as `std::tuple` |
| `transform_columns<Is...>(fn)` | Kernel dispatch: `fn(ptr<I0>, ptr<I1>, ..., n)` |
| `scale_column<I>(scale, bias)` | col[i] = col[i]*scale + bias (vectorized) |
| `clamp_column<I>(lo, hi)` | Element-wise clamp (vectorized) |
| `integrate_verlet_simd(dt)` | Built-in Verlet: float or double |
| `size()` / `empty()` / `clear()` / `reserve(n)` | Standard capacity ops |

---

## 5. End-to-End API Examples

### 5.1 Aligned SIMD Integration

```cpp
#include "containers/dynamic/soa_vector.hpp"

// 64-byte aligned columns: vmovaps instead of vmovups
containers::dynamic::AlignedSoAVector<float, float, float, float, float, float> particles;
particles.reserve(1024);

for (int i = 0; i < 128; ++i)
    particles.push_back(float(i)*10, 0.f, 0.f, 5.f, 0.f, -9.81f);

particles.integrate_verlet_simd(0.016f);
```

### 5.2 Custom SIMD Kernel via `transform_columns`

```cpp
// Apply drag: vx[i] *= 0.99, vy[i] *= 0.99
particles.transform_columns<2, 3>([](float* vx, float* vy, std::size_t n) {
    SOA_VECTORIZE
    for (std::size_t i = 0; i < n; ++i) {
        vx[i] *= 0.99f;
        vy[i] *= 0.99f;
    }
});
```

### 5.3 Bulk Removal with `erase_if`

```cpp
// Remove all dead particles (col<5> == 0)
const std::size_t removed = particles.erase_if(
    [](float, float, float, float, float, float lifetime) {
        return lifetime <= 0.f;
    });
```

### 5.4 Merge Two Particle Systems

```cpp
containers::dynamic::AlignedSoAVector<float, float, float, float, float, float> burst;
// ... fill burst ...
particles.append_range(burst); // O(M), no per-element overhead
```

### 5.5 Never-Allocating `static_vector`

```cpp
#include "containers/static/static_vector.hpp"

containers::static_vector<int, 16> vec;
vec.push_back(10);
vec.push_back(20);
for (int val : vec) std::cout << val << " ";
```

Pebble provides high-performance data-oriented array containers in `include/containers/dynamic/soa_vector.hpp` and `include/containers/static/static_vector.hpp`. They are engineered for SIMD vectorization, zero cache pollution, and predictable memory footprints in physics engines and numerical simulations.

---

## 1. Architectural Overview & Memory Layouts

```
                      Array of Structures (AoS) vs Structure of Arrays (SoA)
                      
   AoS (Traditional std::vector<Particle>):
   ┌───────────────────────┬───────────────────────┬───────────────────────┐
   │ x | y | vx | vy | m   │ x | y | vx | vy | m   │ x | y | vx | vy | m   │  (Cache lines polluted with
   └───────────────────────┴───────────────────────┴───────────────────────┘   unused mass during integration)
   
   SoA (Pebble SoAVector<Particle>):
   ┌────────────────────────────────────────────────────────┐
   │ x0  | x1  | x2  | x3  | x4  | x5  | x6  | x7  | ...    │  <-- 100% Contiguous SIMD Load
   ├────────────────────────────────────────────────────────┤
   │ y0  | y1  | y2  | y3  | y4  | y5  | y6  | y7  | ...    │  <-- 100% Contiguous SIMD Load
   ├────────────────────────────────────────────────────────┤
   │ vx0 | vx1 | vx2 | vx3 | vx4 | vx5 | vx6 | vx7 | ...    │  <-- 100% Contiguous SIMD Load
   ├────────────────────────────────────────────────────────┤
   │ vy0 | vy1 | vy2 | vy3 | vy4 | vy5 | vy6 | vy7 | ...    │  <-- 100% Contiguous SIMD Load
   ├────────────────────────────────────────────────────────┤
   │ m0  | m1  | m2  | m3  | m4  | m5  | m6  | m7  | ...    │
   └────────────────────────────────────────────────────────┘
```

---

## 2. Storage Policies for `SoAVector`

`containers::dynamic::SoAVector<T, StoragePolicy>` provides compile-time storage policy injection:

| Storage Policy | Memory Location | Reallocation Behavior | Best Use Case |
|:---|:---|:---|:---|
| **`StaticStoragePolicy<N>`** | Stack (`std::array`) | Never allocates; compile-time fixed capacity $N$ | Small particle clusters, contact manifolds ($N \le 64$) |
| **`SmallVectorStoragePolicy<InlineBytes>`** | Inline SBO $\to$ Heap | Inline until capacity exceeded, then spills to arena/heap | Dynamic physics islands, local query results |
| **`DynamicStoragePolicy<Alloc>`** | Heap / PMR Arena | Dynamically grows with $1.5\times$ expansion | Global particle systems, massive entity pools |

---

## 3. SIMD Vectorized Kinematics & Velocity Verlet

`SoAVector` implements an 8-wide unrolled SIMD loop for Velocity-Verlet integration:
$$\mathbf{p}_{n+1} = \mathbf{p}_n + \mathbf{v}_n \Delta t + \frac{1}{2} \mathbf{a}_n \Delta t^2$$
$$\mathbf{v}_{n+1} = \mathbf{v}_n + \mathbf{a}_n \Delta t$$

Because contiguous floats are loaded 8 elements at a time directly into vector registers (AVX2 / ARM NEON), memory bandwidth utilization approaches $100\%$ of theoretical hardware maximums.

---

## 4. End-to-End API Examples

### 4.1 SoAVector SIMD Integration
```cpp
#include "containers/dynamic/soa_vector.hpp"
#include <iostream>

struct Particle {
    float x, y;
    float vx, vy;
    float ax, ay;
    float mass;
};

int main() {
    // Dynamic SoAVector with 1024 capacity
    containers::dynamic::SoAVector<Particle> particles;
    particles.reserve(1024);

    // Push elements using tuple/aggregate interface
    for (int i = 0; i < 128; ++i) {
        particles.push_back({
            .x = static_cast<float>(i * 10), .y = 0.0f,
            .vx = 0.0f, .vy = 5.0f,
            .ax = 0.0f, .ay = -9.81f,
            .mass = 1.0f
        });
    }

    // High-performance 8-wide SIMD integration step
    constexpr float dt = 0.016f;
    particles.integrate_verlet_simd(dt);

    std::cout << "Updated particle 0 position: (" << particles.x(0) << ", " << particles.y(0) << ")\n";
    std::cout << "Updated particle 0 velocity: (" << particles.vx(0) << ", " << particles.vy(0) << ")\n";
}
```

### 4.2 Never-Allocating `static_vector`
```cpp
#include "containers/static/static_vector.hpp"
#include <iostream>

int main() {
    // Compile-time fixed capacity: 16 integers on stack
    containers::static_vector<int, 16> vec;

    vec.push_back(10);
    vec.push_back(20);
    vec.push_back(30);

    for (int val : vec) {
        std::cout << val << " ";
    }
    std::cout << "\nSize: " << vec.size() << ", Max Capacity: " << vec.max_size() << "\n";
}
```
