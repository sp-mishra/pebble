# Math Vectors & Game Graphics Primitives (`include/containers/numeric/math_vector.hpp`)

Pebble's Math Vector library (`pebble::math` / `ts::math`) is a header-only, zero-heap-allocation, `constexpr`-enabled
linear algebra and geometry engine. Built directly on top of `ts::static_tensor`, all types guarantee a fixed cache-line
footprint, SIMD vectorizability, and compile-time evaluation.

---

## 1. Type Matrix & Memory Layouts

| Type Alias           | Dimensions | Underlying Storage                |  Memory Size   |   Alignment    | Primary Use Case                                   |
|:---------------------|:----------:|:----------------------------------|:--------------:|:--------------:|:---------------------------------------------------|
| **`vec2` / `vec2d`** |     2      | `static_tensor<T, ..., 2>`        |  8 / 16 bytes  |  8 / 16 bytes  | 2D positions, UV texture coords, planar velocities |
| **`vec3` / `vec3d`** |     3      | `static_tensor<T, ..., 3>`        | 12 / 24 bytes  | 16 / 32 bytes  | 3D positions, surface normals, forces              |
| **`vec4` / `vec4d`** |     4      | `static_tensor<T, ..., 4>`        | 16 / 32 bytes  | 16 / 32 bytes  | Homogeneous coordinates, RGBA color vectors        |
| **`quat`**           |     4      | `static_tensor<float, ..., 4>`    |    16 bytes    |    16 bytes    | $[x, y, z, w]$ Unit rotation quaternions           |
| **`mat2`**           |    2x2     | `static_tensor<float, ..., 2, 2>` |    16 bytes    |    16 bytes    | 2D rotation & scaling matrices                     |
| **`mat3`**           |    3x3     | `static_tensor<float, ..., 3, 3>` |    36 bytes    |    16 bytes    | 3D rotations, inertia tensors                      |
| **`mat4` / `mat4d`** |    4x4     | `static_tensor<T, ..., 4, 4>`     | 64 / 128 bytes | 64 / 128 bytes | 3D Model-View-Projection (MVP) affine transforms   |

---

## 2. Mathematical Formulations

### 2.1 Vector Geometry & Optics

- **Dot Product & Length**:
  $$\mathbf{a} \cdot \mathbf{b} = \sum a_k b_k, \quad \|\mathbf{a}\| = \sqrt{\mathbf{a} \cdot \mathbf{a}}$$
- **Ray Reflection**:
  $$\mathbf{R} = \mathbf{I} - 2 (\mathbf{I} \cdot \mathbf{N})\mathbf{N}$$
- **Snell's Law Refraction**:
  $$\mathbf{T} = \eta \mathbf{I} + \left (\eta (\mathbf{N} \cdot \mathbf{I}) - \sqrt{1 - \eta^2 (1 - (\mathbf{N} \cdot \mathbf{I})^2)}\right) \mathbf{N}$$

### 2.2 Quaternions & Rotations

- **Hamilton Product**:
  $$q_1 \otimes q_2 = (w_1 w_2 - \mathbf{v}_1 \cdot \mathbf{v}_2, \; w_1 \mathbf{v}_2 + w_2 \mathbf{v}_1 + \mathbf{v}_1 \times \mathbf{v}_2)$$
- **Spherical Linear Interpolation (Slerp)**:
  $$\text{Slerp} (q_1, q_2, t) = \frac{\sin ((1-t)\theta)}{\sin\theta} q_1 + \frac{\sin (t\theta)}{\sin\theta} q_2$$

---

## 3. End-to-End API Guide

### 3.1 3D Camera Look-At & Perspective Projection

```cpp
#include "containers/numeric/math_vector.hpp"
#include <iostream>

using namespace pebble::math;

int main() {
    // 1. Camera Look-At View Matrix
    vec3 eye(0.0f, 10.0f, 25.0f);
    vec3 target(0.0f, 0.0f, 0.0f);
    vec3 up(0.0f, 1.0f, 0.0f);
    mat4 V = look_at(eye, target, up);

    // 2. Perspective Projection Matrix
    mat4 P = perspective(
        static_cast<float>(M_PI / 3.0), // 60 deg vertical FOV
        16.0f / 9.0f,                   // Aspect ratio
        0.1f,                           // Near plane
        1000.0f                         // Far plane
    );

    // 3. Model World Transform
    mat4 M = mul(translation(vec3(0.0f, 2.0f, 0.0f)), scaling(vec3(1.5f, 1.5f, 1.5f)));

    // Combined MVP Matrix
    mat4 MVP = mul(P, mul(V, M));

    // Transform a model-space point into clip-space
    vec3 model_pt(1.0f, 0.0f, 0.0f);
    vec3 clip_pt = mul_point(MVP, model_pt);

    std::cout << "Projected Clip Point: (" << clip_pt[0] << ", " << clip_pt[1] << ", " << clip_pt[2] << ")\n";
}
```

### 3.2 Physics Ray Reflection & Normal Calculations

```cpp
#include "containers/numeric/math_vector.hpp"
#include <iostream>

using namespace pebble::math;

void trace_laser_bounce() {
    vec3 incident(1.0f, -1.0f, 0.0f);
    vec3 surface_normal(0.0f, 1.0f, 0.0f);

    vec3 reflected = reflect(normalize(incident), surface_normal);
    std::cout << "Reflected Vector: (" << reflected[0] << ", " << reflected[1] << ", " << reflected[2] << ")\n";
}
```
