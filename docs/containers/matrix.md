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

## Tests

`src/tests/matrix/test_mat.cpp` — core matrix tests  
`src/tests/akruti/test_akruti_matrix.cpp` — akruti::Mat2 alias, rotation, bridge operators  
`src/tests/gati/test_gati_matrix.cpp` — gati::Mat2, quad_form_2d, axpy physics correctness  
`src/tests/containers/test_prakriti_matrix.cpp` — prakriti nrm2_sq, axpy XPBD pattern, backend static_assert
