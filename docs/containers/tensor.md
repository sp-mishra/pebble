# Tensor: High-Performance C++23/C++26 Multidimensional Tensor Engine & EDSL

The `Tensor` subsystem (`include/containers/tensor/tensor.hpp`, `include/containers/tensor/tensor_edsl.hpp`, namespace `ts` / `containers::tensor` / `ts::edsl`) is Pebble's header-only, policy-based multidimensional tensor computing framework and symbolic Embedded Domain-Specific Language (EDSL). It offers zero-overhead abstractions, decoupling storage layouts from execution architectures with compile-time policy injection and Sūtra/Vākya-inspired expression graph compilation.

---

## 📑 Table of Contents

1. [Key Highlights](#-key-highlights)
2. [File Structure](#-file-structure)
3. [Quick Start: Core Tensor Engine](#-quick-start-core-tensor-engine)
4. [Quick Start: Symbolic Tensor EDSL](#-quick-start-symbolic-tensor-edsl)
   - [Level 1 — One-Shot Scalar & Tensor Evaluation](#level-1--one-shot-scalar--tensor-evaluation)
   - [Level 2 — Compile Once, Run Many (Symbolic Model)](#level-2--compile-once-run-many-symbolic-model)
5. [Game Math & Graphics Vectors (Constexpr Stack Math)](#-game-math--graphics-vectors-constexpr-stack-math)
6. [Structure-of-Arrays (SoA) Reflection Storage](#-structure-of-arrays-soa-reflection-storage)
7. [Apple Silicon GPU Acceleration (`mlx`)](#-apple-silicon-gpu-acceleration-mlx)
8. [Arrow-Style String Storage](#-arrow-style-string-storage)

---

## 🚀 Key Highlights

1. **Clean Policy-Based Architecture**:
   - **Storage Policies (Memory)**: Standard heap (`default_storage_policy`), Small-Buffer-Optimized inline buffer (`small_tensor_storage_policy<InlineBytes>`), Smriti Arena / Pool (`smriti_storage_policy<ResourceT>`), Aligned SIMD storage (`highway_storage_policy`), Apple Silicon GPU memory (`mlx_storage_policy`), and Arrow-style zero-copy string columns (`arrow_string_storage`).
   - **Computation Policies (Execution)**: Pure CPU reference (`default_computation_policy`), Portable Highway SIMD (`highway_computation_policy`), and Apple MLX Metal GPU acceleration (`mlx_computation_policy`).
2. **Small & Big Tensor Optimization**:
   - **Static Compile-Time Tensors (`static_tensor`)**: Fixed-dimension stack arrays with zero heap allocation.
   - **Small-Buffer Dynamic Tensors (`small_tensor`)**: Uses Pebble's `SmallVector` to keep tensors under 64/128 bytes strictly on the cache line.
   - **Pebble Ecosystem Synergy**:
     - `containers::dynamic::SmallVector`: Used for `tensor_shape` and `tensor_strides` (eliminating heap allocation for rank 1–4 tensors).
     - `mem::smriti`: Direct arena and memory-pool allocation with `smriti_tensor` and `SmritiAllocator`.
     - `observability::nadi`: Distributed tracing / pulse scopes for zero-overhead profiling.
3. **True Lazy Evaluation via Expression Templates**:
   - Uses C++23 *Deducing this* for zero-cost static polymorphism.
   - Expressions (`binary_expression`, `unary_expression`) build compile-time evaluation trees without creating intermediate heap allocations.
   - `scalar_wrapper` uses value-capture traits while heavy tensors use const reference capture.
4. **Symbolic Tensor EDSL (Sūtra & Vākya Inspired)**:
   - **User-Defined Parameter Literals (`_p` and `_t`)**: `"learning_rate"_p = 0.001f` for scalar parameters and `"weights"_t = W` for tensors.
   - **Symbolic Leaves (`sym_tensor<Rank>`)**: Pure metadata carriers (Rank, Shape) building DAGs without allocating heap buffers.
   - **Eager Shape Calculus**: Shape deduction and validation at graph construction time for `matmul`, reductions, broadcasts, and transpositions.
   - **Multi-Tier Execution**:
     - **Level 1 (`ts::eval`)**: Immediate one-shot evaluation with named bindings.
     - **Level 2 (`ts::compile`)**: Compile-once, run-many execution pipelines with hardware target dispatch (`ts::target::cpu`, `ts::target::simd`, `ts::target::gpu`).
5. **C++23 Modern Interfaces**:
   - Multidimensional subscripting: `tensor[i, j, k]`.
   - Strided slicing and views: `tensor(all, {0, 2})`.
6. **Rich Mathematical & Neural Kernel Suite**:
   - Arithmetic: `+`, `-`, `*`, `/`, `%`, `fma`
   - Reductions & Stats: `sum`, `mean`, `max`, `min`, `variance`, `std_dev`, `normalize`, `reduce_sum`, `reduce_mean`, `reduce_max`
   - Linear Algebra: `dot`, `matmul`, `transpose`, `reshape`, `flatten`
   - Neural Activations & Math: `relu`, `sigmoid`, `gelu`, `softmax`, `clip`, `power`, trigonometric (`sin`, `cos`, `tan`), exponential (`exp`, `log`, `sqrt`, `abs`).

---

## 📁 File Structure

```
include/containers/tensor/
├── tensor.hpp                    # Core tensor, static_tensor, tensor_view, expressions, policies
├── tensor_edsl.hpp               # Symbolic Tensor EDSL master umbrella header
├── edsl/                         # EDSL components
│   ├── sym_leaf.hpp              # Parameter literals (_p, _t), sym_tensor, bindings
│   ├── shape_inference.hpp       # Eager shape calculus & verification
│   ├── operators.hpp             # AST nodes, neural activations & math operators
│   └── eval.hpp                  # L1/L2 evaluation & backend execution engine
├── highway_computation_policy.hpp# Google Highway portable SIMD vectorization
├── mlx_storage_policy.hpp        # Apple Silicon MLX GPU memory storage
├── mlx_computation_policy.hpp    # Apple Silicon MLX GPU execution policy
└── tensor_utils.hpp              # Pretty-printing and debugging utilities
```

---

## 💡 Quick Start: Core Tensor Engine

```cpp
#include <containers/tensor/tensor.hpp>
#include <containers/tensor/highway_computation_policy.hpp>

using namespace ts;

// 1. Create a standard CPU tensor
tensor<float> A({2, 3}, {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f});
tensor<float> B({3, 2}, {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f});

// 2. Perform matrix multiplication
auto C = dot(A, B); // Result shape: {2, 2}

// 3. Small-Buffer-Optimized Tensor (SBO on stack / cache line)
small_tensor<float, 64> small_t({2, 4}); // Lives inline if <= 64 bytes

// 4. Smriti Arena Memory Pool Allocation
smriti::BumpPool<smriti::SystemRAMDomain> pool{4096};
smriti_tensor<float, decltype(pool)> arena_t({3, 3}, pool);

// 5. Highway SIMD accelerated tensor
using simd_tensor = tensor<float, highway_storage_policy, highway_computation_policy>;
simd_tensor X({1024});
simd_tensor Y({1024});

// Element-wise lazy evaluation executed with SIMD
auto Z = (X * 2.0f + Y) / 3.0f;
float total = sum(Z);

// 6. Apple Silicon MLX GPU / Metal accelerated tensor (macOS)
#include <containers/tensor/mlx_storage_policy.hpp>
#include <containers/tensor/mlx_computation_policy.hpp>

gpu_tensor<float> G1({4096, 4096});
gpu_tensor<float> G2({4096, 4096});

// GPU-accelerated matrix multiplication & reductions
auto G3 = dot(G1, G2);
float gpu_total = sum(G3);
```

---

## 🧠 Quick Start: Symbolic Tensor EDSL

### Level 1 — One-Shot Scalar & Tensor Evaluation

```cpp
#include <containers/tensor/tensor_edsl.hpp>

using namespace ts;
using namespace ts::edsl;
using namespace ts::edsl::literals;

// 1. Scalar polynomial: x^2 + 2x + 1
auto x = "x"_p;
auto expr = (x * x) + (2.0f * x) + 1.0f;
float y = ts::eval_scalar(expr, "x"_p = 3.0f); // 16.0f

// 2. Concrete Linear Layer: y = relu(X * W + b)
tensor<float> X({2, 3}, {1.0f, -2.0f, 3.0f, -4.0f, 5.0f, -6.0f});
tensor<float> W({3, 2}, {1.0f, 2.0f, 0.5f, -1.0f, 2.0f, 1.0f});
tensor<float> b({2}, {0.5f, -0.5f});

auto layer = relu(matmul("X"_t, "W"_t) + "b"_t);
tensor<float> out = ts::eval(layer, "X"_t = X, "W"_t = W, "b"_t = b);
```

### Level 2 — Compile Once, Run Many (Symbolic Model)

```cpp
// Declare symbolic input & weights with shape metadata
auto in = sym_tensor<2>("in", {32, 128});
auto W1 = sym_tensor<2>("W1", {128, 64});
auto b1 = sym_tensor<1>("b1", {64});

// Build Computation Graph (MLP + Softmax)
auto hidden = relu(matmul(in, W1) + b1);
auto probs  = softmax(hidden);

// Compile model for execution
auto model = ts::compile(probs, ts::target::cpu);

// Execute across iterations
for (const auto& batch : data_loader) {
    tensor<float> predictions = model("in"_t = batch, "W1"_t = trained_W, "b1"_t = trained_b);
}
```

---

## 🎮 Game Math & Graphics Vectors (Constexpr Stack Math)

For graphics, physics engines, and game math, `pebble::math` (or `ts::math` from `include/containers/numeric/math_vector.hpp`) provides specialized zero-overhead, stack-allocated vector and matrix aliases powered by `ts::static_tensor`:
- Types: `vec2`, `vec3`, `vec4`, `quat`, `mat2`, `mat3`, `mat4` (and double/integer variants: `vec3d`, `vec4i`, `mat4d`).
- **Fully `constexpr` enabled**: construct, index (`v[0]`, `m[0, 1]`), and perform arithmetic during compilation.
- Geometric algorithms: `dot`, `cross`, `length`, `length_sq`, `normalize`, `distance`, `lerp`, `reflect`.

```cpp
#include <containers/numeric/math_vector.hpp>

using namespace pebble::math;

// 1. Compile-time vector geometry
constexpr vec3 right(1.0f, 0.0f, 0.0f);
constexpr vec3 up(0.0f, 1.0f, 0.0f);
constexpr vec3 forward = cross(right, up); // (0, 0, 1)

static_assert(forward[2] == 1.0f);
static_assert(dot(right, up) == 0.0f);

// 2. Normalization & transforms
vec3 velocity(0.0f, 3.0f, 4.0f);
vec3 dir = normalize(velocity); // (0.0, 0.6, 0.8)
```

---

## 🧱 Structure-of-Arrays (SoA) Reflection Storage

For high-throughput cache locality, data-oriented design (DOD), and SIMD-friendly vectorization, `ts::soa_storage_policy<StructT>` integrates with Pebble's `meta` compile-time reflection system to decompose custom structs into parallel column arrays:

```cpp
struct Particle {
    float x, y, z;
    float vx, vy, vz;
    int id;
};

// Decomposes Particle into parallel contiguous column arrays
meta::soa_storage<Particle, 1024> particles;
particles.push_back(Particle{0.0f, 1.0f, 2.0f, 0.1f, 0.2f, 0.3f, 1});

// Direct column access (perfect SIMD cache lines)
float first_x = particles.column<0>()[0]; 

// Reconstruct AoS element on demand
Particle p = particles.get(0);
```

---

## 🍏 Apple Silicon GPU Acceleration (`mlx`)

When compiling on macOS with `HAS_MLX=1`, `ts::gpu_tensor<T>` (or `ts::tensor<T, mlx_storage_policy, mlx_computation_policy>`) and EDSL target `ts::target::gpu` leverage Apple's MLX unified memory architecture and Metal GPU execution backend:
- **Unified Memory Architecture**: Zero host-to-device PCI transfer latency (CPU and GPU share the unified memory pool).
- **Metal GPU Dispatch**: Matrix operations (`dot`, `matmul`), reductions (`sum`, `mean`, `max`), and elementwise arithmetic are dispatched directly to Apple Metal GPU cores.
- **Heterogeneous Interop**: Seamlessly construct or assign tensors between CPU reference storage, Highway SIMD buffers, and MLX GPU storage.

```cpp
auto A = sym_tensor<2>("A", {2048, 2048});
auto B = sym_tensor<2>("B", {2048, 2048});
auto graph = matmul(A, B) + "bias"_p;

// Compile targeting Apple Silicon GPU
auto gpu_model = ts::compile(graph, ts::target::gpu);

// Dispatched to Metal GPU Unified Memory
auto gpu_result = gpu_model("A"_t = tA, "B"_t = tB, "bias"_p = 1.5f);
```

---

## ⚡ Arrow-Style String Storage

For tabular and dataframe applications, `ts::ArrowStringStorage` stores strings in a contiguous `std::vector<char>` buffer with an offset array:
- Zero pointer chasing ($O(1)$ flat cache locality).
- Returns zero-copy `std::string_view` on `operator[]`.
- Up to 10x faster for bulk scanning, sorting, and grouping.
