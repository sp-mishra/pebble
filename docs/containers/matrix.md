# containers/matrix — `ga::StaticMatrix<T,R,C>`

**Header**: `#include <containers/matrix/static.hpp>`  
**Namespace**: `ga`  
**Language standard**: C++23/26, header-only, zero-heap, zero-virtual, zero macros

## Overview

`ga::StaticMatrix<T,R,C>` is a compile-time fixed-size row-major matrix backed by `std::array<T,R*C>`. It serves as the canonical matrix type across pebble's physics subsystems, replacing prior hand-rolled structs and `pebble::math::mat2`.

## Aliases

| Alias | Resolves to |
|:---|:---|
| `ga::Mat2<T>` | `ga::StaticMatrix<T,2,2>` |
| `ga::Mat3<T>` | `ga::StaticMatrix<T,3,3>` |
| `ga::Mat4<T>` | `ga::StaticMatrix<T,4,4>` |
| `ga::Vec2<T>` | `ga::StaticMatrix<T,2,1>` |
| `ga::Vec3<T>` | `ga::StaticMatrix<T,3,1>` |
| `ga::Vec4<T>` | `ga::StaticMatrix<T,4,1>` |
| `akruti::Mat2<T>` | `ga::StaticMatrix<T,2,2>` |
| `gati::Mat2` | `ga::StaticMatrix<float,2,2>` |

## Core API

```cpp
StaticMatrix<T,R,C> m{...};   // initializer_list, row-major
m(r, c)                        // element access
StaticMatrix::identity()       // requires R == C
m.transpose()
m.det()
m.inv()
m.trace()
A * B                          // matrix multiply A(R×C) * B(C×K) → result(R×K)
A + B, A - B
m * scalar, scalar * m
```

## Free Functions (column vectors)

```cpp
ga::dot(a, b)            // dot product of column vectors
ga::nrm2(v)              // L2 norm
ga::nrm2_sq(v)           // squared L2 norm — use instead of pebble::math::length_sq
ga::axpy(alpha, x, y)    // y += alpha * x  (BLAS axpy, in-place)
ga::cross2d(a, b)        // scalar 2D cross: a.x*b.y - a.y*b.x
ga::quad_form_2d(inv_mass, inv_inertia, r, n)
                         // J·diag(M⁻¹)·Jᵀ for 2D rigid body contact
                         // = inv_mass + cross2d(r,n)² * inv_inertia
```

## Physics Consumers

| Subsystem | Type used | Purpose |
|:---|:---|:---|
| `akruti` | `Mat2<T>` (alias) | OBB rotation, CSG transforms, gradient/narrowphase math |
| `gati` | `Mat2` (alias) | Rigid body basis, transform hierarchy, island solver |
| `gati::SequentialImpulseSolver` | `ga::Vec2<float>`, `ga::quad_form_2d`, `ga::axpy` | Contact effective mass, position correction |
| `prakriti::XpbdSolver` | `ga::Vec2<Scalar>`, `ga::axpy` | XPBD edge constraint position correction |
| `prakriti::DensitySolver` | `ga::Vec2<Scalar>`, `ga::nrm2_sq`, `ga::axpy` | PBF density projection, interfacial tension, correction application |
| `prakriti::kernels` | `ga::nrm2_sq` | SPH spiky gradient kernel distance |
| `prakriti::ThermalSolver` | `ga::nrm2_sq` | Heat diffusion neighbor kernel |

## Interop Helpers

Available when `<containers/numeric/math_vector.hpp>` is on the include path:

```cpp
ga::to_static_matrix(pebble::math::mat2) → ga::Mat2<float>
ga::from_static_matrix(ga::Mat2<float>)  → pebble::math::mat2
ga::to_static_vec(pebble::math::vec2)    → ga::Vec2<float>
ga::from_static_vec(ga::Vec2<float>)     → pebble::math::vec2
```

These are transitional — removed once all `pebble::math::mat2` call sites migrate.

## Specializations

`det()` and `inv()` are specialized for 2×2, 3×3, and 4×4 for optimal inline expansion.

## Forward-Mode Autodiff & Curvature (`<containers/matrix/dual.hpp>`)

`ga::Dual<T,N>` carries a value `.v` and `N` derivative slots `.d[k]` through the usual
arithmetic and free functions (`sqrt/exp/log/sin/cos/…`). It is **forward-mode only** —
`Dual<Dual<…>>` does *not* compile (a `static_assert` requires a floating-point base). Gradients
come from a single sweep:

```cpp
// f: (const std::array<Dual<T,1>,N>&) -> Dual<T,1>
std::array<T,N> g = ga::grad_vec<T,N>(f, x);          // ∇f(x), exact (no truncation error)
```

Second-order curvature is available **without** forming a nested dual, via forward-over-central-
difference on that exact gradient:

```cpp
ga::hessian_vec(f, x, v, eps = 0)   // ∇²f(x)·v  — matrix-free HVP, std::array<T,N>
ga::hessian(f, x, eps = 0)          // dense ∇²f(x) — std::array<std::array<T,N>,N>, symmetrized
```

- **`hessian_vec`** — `(∇f(x+εv) − ∇f(x−εv)) / 2ε`. The inner gradients are exact (`grad_vec`), so
  the only error is the `O(ε²)` outer directional step. Default `ε = ∛(machine-eps)` balances
  truncation against round-off for the central rule. Cost: two `grad_vec` calls, no `N×N` storage —
  this is the operator consumed by matrix-free Newton-CG / trust-region solvers (see `kalpa`).
- **`hessian`** — assembles the dense Hessian as `N` `hessian_vec` columns against the canonical
  basis, then symmetrizes (`H[i][j]=H[j][i]`) to cancel difference noise. `O(N)` gradient
  evaluations; **small `N` only**. Row-major: `H[i][j] = ∂²f/∂xᵢ∂xⱼ`.

Both are `ga::`-namespace, header-only, templated on floating `T` and static extent `N`; unused
paths are never instantiated, so a value-only call pays nothing for the curvature machinery.

## Tests

`src/tests/matrix/test_mat.cpp` — core matrix tests  
`src/tests/akruti/test_akruti_matrix.cpp` — akruti::Mat2 alias, rotation, bridge operators  
`src/tests/gati/test_gati_matrix.cpp` — gati::Mat2, quad_form_2d, axpy physics correctness  
`src/tests/containers/test_prakriti_matrix.cpp` — prakriti nrm2_sq, axpy XPBD pattern, backend static_assert  
`src/tests/matrix/test_dual.cpp` — forward-mode dual gradients; `hessian_vec` vs analytic Hessian·v, symmetry, and parity with dense `hessian`
