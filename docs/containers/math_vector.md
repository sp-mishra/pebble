# Math Vectors & Game Graphics Primitives

The Math Vector library (`include/containers/numeric/math_vector.hpp`, namespaces `pebble::math` and `ts::math`) is Pebble's header-only, zero-heap-allocation, `constexpr`-enabled linear algebra and geometry library designed for game development, physics engines, computer graphics, and spatial simulations.

It is built directly on top of Pebble's `ts::static_tensor` engine, guaranteeing fixed cache-line footprint, SIMD vectorizability, and compile-time evaluation.

---

## 📑 Table of Contents

1. [Type Definitions](#-type-definitions)
2. [Vector Operations & Geometry](#-vector-operations--geometry)
3. [Physics & Ray Optics](#-physics--ray-optics)
4. [Matrix Transforms & Projections](#-matrix-transforms--projections)
5. [Quaternions & Rotations](#-quaternions--rotations)
6. [Compile-Time / Constexpr Verification](#-compile-time--constexpr-verification)

---

## 📐 Type Definitions

All vector and matrix types are zero-overhead stack allocations leveraging `static_tensor`:

| Alias | Underlying Type | Dimensions | Description |
| :--- | :--- | :--- | :--- |
| `vec2` / `vec2d` / `vec2i` | `static_tensor<T, ..., 2>` | 2D Vector | 2D coordinates, UV texture maps, planar velocity |
| `vec3` / `vec3d` / `vec3i` | `static_tensor<T, ..., 3>` | 3D Vector | 3D positions, normals, velocities, forces |
| `vec4` / `vec4d` / `vec4i` | `static_tensor<T, ..., 4>` | 4D Vector | Homogeneous coordinates, RGBA colors |
| `quat` | `static_tensor<float, ..., 4>` | 4D Unit Vector | $[x, y, z, w]$ orientation quaternions |
| `mat2` | `static_tensor<float, ..., 2, 2>` | 2x2 Matrix | 2D rotations and scaling |
| `mat3` | `static_tensor<float, ..., 3, 3>` | 3x3 Matrix | 3D rotation matrices, inertia tensors |
| `mat4` / `mat4d` | `static_tensor<T, ..., 4, 4>` | 4x4 Matrix | 3D affine transforms, view & projection matrices |

---

## 🏹 Vector Operations & Geometry

```cpp
#include <containers/numeric/math_vector.hpp>

using namespace pebble::math;

// Vector creation (constexpr)
constexpr vec3 a(1.0f, 2.0f, 3.0f);
constexpr vec3 b(4.0f, 5.0f, 6.0f);

// Dot Product & Cross Product
constexpr float d = dot(a, b);               // 32.0f
constexpr vec3 c  = cross(a, b);             // (-3, 6, -3)

// Length & Normalization
float len_sq = length_sq(a);                 // 14.0f
float len    = length(a);                    // 3.7416f
vec3 unit_a  = normalize(a);

// Distance & Interpolation
float dist = distance(a, b);
vec3 mid   = lerp(a, b, 0.5f);

// Projections & Angle
vec3 proj = project(a, b);                   // Component of a parallel to b
vec3 rej  = reject(a, b);                    // Component of a perpendicular to b
double rad = angle(a, b);                    // Angle between vectors in radians
```

---

## ⚡ Physics & Ray Optics

Ideal for physics engines, rigid body contacts, and ray tracing:

```cpp
// Ray reflection off a surface: R = I - 2 * (N . I) * N
vec3 ray(1.0f, -1.0f, 0.0f);
vec3 normal(0.0f, 1.0f, 0.0f);
vec3 reflected = reflect(ray, normal);        // (1.0f, 1.0f, 0.0f)

// Snell's Law Refraction: refract(I, N, eta)
vec3 incoming(0.0f, -1.0f, 0.0f);
vec3 refracted = refract(incoming, normal, 1.33f); // Refract into water
```

---

## 🎥 Matrix Transforms & Projections

```cpp
// Affine transformations
mat4 T = translation(vec3(10.0f, 0.0f, -5.0f));
mat4 S = scaling(vec3(2.0f, 2.0f, 2.0f));
mat4 M = mul(T, S);

// Transform points and direction vectors
vec3 world_pos = mul_point(M, vec3(1.0f, 1.0f, 1.0f));
vec3 world_dir = mul_direction(M, vec3(0.0f, 0.0f, 1.0f));

// Camera View Matrix (Look-At)
mat4 view = look_at(
    vec3(0.0f, 5.0f, 10.0f),  // Eye
    vec3(0.0f, 0.0f, 0.0f),   // Target
    vec3(0.0f, 1.0f, 0.0f)    // Up
);

// Perspective Projection Matrix
mat4 proj = perspective(
    static_cast<float>(M_PI / 3.0), // FOV (60 deg)
    16.0f / 9.0f,                   // Aspect Ratio
    0.1f,                           // Near plane
    1000.0f                         // Far plane
);
```

---

## 🔄 Quaternions & Rotations

Quaternions represent 3D orientations without gimbal lock:

```cpp
// Create quaternion from Axis-Angle (90 deg around Z)
quat q = quat_axis_angle(vec3(0.0f, 0.0f, 1.0f), static_cast<float>(M_PI / 2.0));

// Rotate a 3D vector
vec3 v(1.0f, 0.0f, 0.0f);
vec3 rotated = quat_rotate(q, v);             // (0.0f, 1.0f, 0.0f)

// Spherical Linear Interpolation (Slerp)
quat q_start = quat_identity();
quat q_half  = quat_slerp(q_start, q, 0.5f); // 45 deg rotation
```

---

## ⏱️ Compile-Time / Constexpr Verification

All vector constructors, indexing (`v[0]`, `m[i, j]`), cross product, dot product, and matrix multiplications can execute at compile-time:

```cpp
constexpr vec3 right(1.0f, 0.0f, 0.0f);
constexpr vec3 up(0.0f, 1.0f, 0.0f);
constexpr vec3 forward = cross(right, up);

static_assert(forward[0] == 0.0f);
static_assert(forward[1] == 0.0f);
static_assert(forward[2] == 1.0f);
static_assert(dot(right, up) == 0.0f);
```
