# Tutorial: Zero to Hero with Pebble Tensor & Symbolic EDSL

Welcome to the **Pebble Tensor Tutorial**. Whether you are developing deep learning neural networks, high-performance physical simulations, computer graphics algorithms, or scientific compute pipelines, this guide will take you from zero to mastering Pebble's modern multidimensional tensor engine and symbolic Embedded Domain-Specific Language (EDSL).

This tutorial assumes no prior knowledge of lazy expression templates or tensor compilers. Everything is explained step-by-step with practical, copy-pasteable modern C++23/C++26 examples.

---

## 📑 Table of Contents

1. [The Philosophy: Why Policy-Based Tensors?](#1-the-philosophy-why-policy-based-tensors)
2. [Step 1: Hello, Tensor! (Creating & Subscripting Tensors)](#step-1-hello-tensor-creating--subscripting-tensors)
3. [Step 2: Lazy Expressions & Zero-Allocation Math](#step-2-lazy-expressions--zero-allocation-math)
4. [Step 3: Compile-Time Static Tensors & Game Vectors](#step-3-compile-time-static-tensors--game-vectors)
5. [Step 4: Memory Storage Policies (SBO & Smriti Arenas)](#step-4-memory-storage-policies-sbo--smriti-arenas)
6. [Step 5: Structure-of-Arrays (SoA) Reflection Layouts](#step-5-structure-of-arrays-soa-reflection-layouts)
7. [Step 6: Symbolic EDSL Level 1 — One-Shot Evaluation (`ts::eval`)](#step-6-symbolic-edsl-level-1--one-shot-evaluation-tseval)
8. [Step 7: Symbolic EDSL Level 2 — Compile Once, Run Many (`ts::compile`)](#step-7-symbolic-edsl-level-2--compile-once-run-many-tscompile)
9. [Step 8: Apple Silicon GPU Acceleration (MLX & Metal)](#step-8-apple-silicon-gpu-acceleration-mlx--metal)
10. [Cheat Sheet & Architecture Reference](#10-cheat-sheet--architecture-reference)

---

## 1. The Philosophy: Why Policy-Based Tensors?

In traditional C++ tensor or matrix libraries (e.g. Eigen, Armadillo), memory allocation and math algorithms are often tightly coupled:
- An expression creates temporary heap allocations for intermediate steps.
- Switching from CPU to SIMD or GPU requires rewriting data structures.
- Small fixed vectors (like 3D points) use the same heavy abstractions as large matrices.

**Pebble Tensor (`ts::tensor`)** decouples **what** you compute from **where** it lives and **how** it executes:
$$\text{Storage Policy (Memory)} + \text{Computation Policy (Backend)} \longrightarrow \text{Zero-Overhead Specialized Tensor}$$

- **Storage Policies**: Standard Heap (`default_storage_policy`), Cache-line Small Buffer (`small_tensor_storage_policy<128>`), Bump/Arena Memory (`smriti_storage_policy`), Struct-of-Arrays (`soa_storage_policy`), or Apple GPU Unified Memory (`mlx_storage_policy`).
- **Computation Policies**: Pure CPU reference, Google Highway SIMD vectorization, or Apple Metal GPU.

---

## Step 1: Hello, Tensor! (Creating & Subscripting Tensors)

Let's start by creating a standard 2D matrix (dynamic shape $\{3, 3\}$) and accessing elements.

```cpp
#include <containers/tensor/tensor.hpp>
#include <iostream>

int main() {
    // 1. Create a 3x3 float tensor initialized to 0
    ts::tensor<float> mat({3, 3});

    // 2. Populate values using modern C++23 multidimensional indexing
    for (size_t r = 0; r < 3; ++r) {
        for (size_t c = 0; c < 3; ++c) {
            mat[r, c] = static_cast<float>(r * 3 + c + 1);
        }
    }

    // 3. Inspect properties
    std::cout << "Rank: " << mat.rank() << "\n";       // 2
    std::cout << "Total Elements: " << mat.size() << "\n"; // 9
    std::cout << "mat[1, 2] = " << mat[1, 2] << "\n";  // 6.0

    return 0;
}
```

---

## Step 2: Lazy Expressions & Zero-Allocation Math

When you write mathematical expressions like $D = A \times B + C$, traditional libraries might allocate an intermediate buffer for $(A \times B)$. Pebble uses **Expression Templates with C++23 Deducing This**:

```cpp
#include <containers/tensor/tensor.hpp>
#include <iostream>

void lazy_math_demo() {
    ts::tensor<float> A({4, 4}, 1.0f);
    ts::tensor<float> B({4, 4}, 2.0f);
    ts::tensor<float> C({4, 4}, 5.0f);

    // This creates an AST node in C++ type system with zero heap allocations!
    auto lazy_expr = (A * 2.0f) + (B * C);

    // Nothing has executed yet! Elements are computed only when evaluated:
    ts::tensor<float> result = lazy_expr; 

    // Built-in reductions
    std::cout << "Sum: " << ts::sum(result) << "\n";
    std::cout << "Mean: " << ts::mean(result) << "\n";
    std::cout << "Max: " << ts::max(result) << "\n";
}
```

---

## Step 3: Compile-Time Static Tensors & Game Vectors

For physics engines, robotics, and game graphics, you don't want heap allocations or dynamic shape vectors. Pebble provides compile-time fixed `ts::static_tensor` and the `pebble::math` library:

```cpp
#include <containers/numeric/math_vector.hpp>
#include <iostream>

using namespace pebble::math;

void game_math_demo() {
    // Zero-heap stack vectors (fully constexpr)
    constexpr vec3 right(1.0f, 0.0f, 0.0f);
    constexpr vec3 up(0.0f, 1.0f, 0.0f);
    constexpr vec3 forward = cross(right, up); // (0, 0, 1)

    static_assert(forward[2] == 1.0f);
    static_assert(dot(right, up) == 0.0f);

    // 4x4 Affine transformations & Camera Look-At
    mat4 T = translation(vec3(0.0f, 10.0f, -5.0f));
    vec3 player_pos(1.0f, 2.0f, 3.0f);
    vec3 world_pos = mul_point(T, player_pos); // (1.0f, 12.0f, -2.0f)

    // Quaternion rotation (90 degrees around Z axis)
    quat q = quat_axis_angle(vec3(0.0f, 0.0f, 1.0f), 3.14159265f / 2.0f);
    vec3 rotated = quat_rotate(q, right); // (0.0f, 1.0f, 0.0f)
}
```

---

## Step 4: Memory Storage Policies (SBO & Smriti Arenas)

You can customize where tensors allocate their memory by changing their Storage Policy:

### 1. Small-Buffer Optimization (SBO)
Keeps tensors $\le 128$ bytes strictly on the stack/cache line.
```cpp
// Fits up to 32 floats inline without calling malloc()
ts::small_tensor<float, 128> local_buf({4, 4});
```

### 2. High-Performance Smriti Arena Allocation
Perfect for tight game loops or frame allocators:
```cpp
#include <mem/smriti.hpp>

void arena_demo() {
    // 64 KB stack/heap bump allocator
    smriti::pools::BumpPool<smriti::domains::SystemRAMDomain> pool{65536};
    
    using arena_tensor = ts::smriti_tensor<float, decltype(pool)>;
    arena_tensor t({64, 64}, pool); // Allocates from pool instantaneously
}
```

---

## Step 5: Structure-of-Arrays (SoA) Reflection Layouts

When simulating thousands of entities (e.g. particles or game objects), Array-of-Structures (`std::vector<Particle>`) destroys CPU cache efficiency. Pebble automatically decomposes arbitrary structs into parallel column arrays using compile-time reflection:

```cpp
#include <containers/tensor/tensor.hpp>

struct Particle {
    float x, y, z;
    float vx, vy, vz;
    int id;
};

void soa_demo() {
    // Decomposes Particle fields into contiguous cache-line columns
    meta::soa_storage<Particle, 1024> particles;
    particles.push_back(Particle{0.0f, 1.0f, 2.0f, 0.1f, 0.2f, 0.3f, 1});

    // SIMD-friendly column access:
    float *x_coords = particles.column<0>(); 
    
    // Reconstruct struct when needed:
    Particle p0 = particles.get(0);
}
```

---

## Step 6: Symbolic EDSL Level 1 — One-Shot Evaluation (`ts::eval`)

Pebble features a symbolic EDSL with user-defined parameter literals (`"name"_p` for scalars and `"name"_t` for tensors):

```cpp
#include <containers/tensor/tensor_edsl.hpp>
#include <iostream>

using namespace ts::edsl;

void edsl_level1_demo() {
    // 1. Declare symbolic expressions
    auto X = "X"_t; // input tensor
    auto W = "W"_t; // weights
    auto b = "b"_p; // scalar bias

    auto graph = (X * 2.0f) + (W * "lr"_p) + b;

    // 2. Supply runtime data with named bindings
    ts::tensor<float> x_data({2, 2}, 1.0f);
    ts::tensor<float> w_data({2, 2}, 3.0f);

    ts::tensor<float> output = ts::eval(
        graph,
        "X"_t = x_data,
        "W"_t = w_data,
        "lr"_p = 0.5f,
        "b"_p = 10.0f
    );

    std::cout << "Result [0,0]: " << output[0, 0] << "\n"; // (1*2) + (3*0.5) + 10 = 13.5
}
```

---

## Step 7: Symbolic EDSL Level 2 — Compile Once, Run Many (`ts::compile`)

When running inferences across thousands of iterations, compile your graph into an optimized execution pipeline:

```cpp
#include <containers/tensor/tensor_edsl.hpp>

using namespace ts::edsl;

void edsl_level2_pipeline() {
    // 1. Declare symbolic graph with shape hints
    auto input  = sym_tensor<2>("input", {32, 128});
    auto weight = sym_tensor<2>("W", {128, 64});
    auto bias   = sym_tensor<1>("b", {64});

    // 2. Neural layer: ReLU(X * W + b)
    auto logits = relu(matmul(input, weight) + bias);
    auto output = softmax(logits);

    // 3. Compile once targeting Highway SIMD or CPU
    auto pipeline = ts::compile(output, ts::target::simd);

    // 4. Run fast inference loop
    ts::tensor<float> batch_x({32, 128});
    ts::tensor<float> trained_w({128, 64});
    ts::tensor<float> trained_b({64});

    for (int step = 0; step < 1000; ++step) {
        ts::tensor<float> predictions = pipeline(
            "input"_t = batch_x,
            "W"_t = trained_w,
            "b"_t = trained_b
        );
    }
}
```

---

## Step 8: Apple Silicon GPU Acceleration (MLX & Metal)

On Apple Silicon macOS (`HAS_MLX=1`), you can direct graph execution to the Apple Metal GPU:

```cpp
#include <containers/tensor/tensor_edsl.hpp>

void gpu_demo() {
    auto A = sym_tensor<2>("A", {2048, 2048});
    auto B = sym_tensor<2>("B", {2048, 2048});
    auto model = ts::compile(matmul(A, B), ts::target::gpu);

    ts::tensor<float> matA({2048, 2048}, 1.0f);
    ts::tensor<float> matB({2048, 2048}, 2.0f);

    // Dispatches matrix multiplication directly to Apple Metal GPU cores
    ts::tensor<float> result = model("A"_t = matA, "B"_t = matB);
}
```

---

## 10. Cheat Sheet & Architecture Reference

| Feature | Code Example |
| :--- | :--- |
| **Tensor Creation** | `ts::tensor<float> t({3, 4});` |
| **Multidimensional Index** | `t[row, col] = 5.0f;` |
| **Game 3D Vectors** | `pebble::math::vec3 v(1.0f, 2.0f, 3.0f);` |
| **Small Buffer (SBO)** | `ts::small_tensor<float, 64> t({2, 4});` |
| **Arena Allocation** | `ts::smriti_tensor<float, BumpPool> t({10, 10}, pool);` |
| **Symbolic Literals** | `"weight"_t = W, "learning_rate"_p = 0.01f` |
| **Level 1 Eval** | `ts::eval(expr, "X"_t = x_val);` |
| **Level 2 Compile** | `auto model = ts::compile(graph, ts::target::simd);` |
| **Reductions** | `ts::sum(t)`, `ts::mean(t)`, `ts::max(t)`, `ts::min(t)` |
| **Activations** | `relu(x)`, `sigmoid(x)`, `softmax(x)`, `gelu(x)` |
