# `SmallVector` — Small-Buffer-Optimised Dynamic Array

> **Header:** `include/containers/dynamic/SmallVector.hpp`
> **Namespace:** `containers::dynamic`
> **Standard required:** C++23 (`-std=c++2b`)
> **Dependencies:** Standard library only

---

## Table of Contents

1. [Purpose](#1-purpose)
2. [InlineBytes Semantics — Bytes, Not Count](#2-inlinebytes-semantics--bytes-not-count)
3. [kInlineCap Formula](#3-kinlinecap-formula)
4. [Spill Semantics](#4-spill-semantics)
5. [Allocator Integration](#5-allocator-integration)
6. [API Reference](#6-api-reference)
7. [Known Limitations](#7-known-limitations)
8. [Comparison with llvm::SmallVector and absl::InlinedVector](#8-comparison-with-llvmsmallvector-and-abslinlinedvector)
9. [Examples](#9-examples)

---

## 1. Purpose

`SmallVector<T, InlineBytes, Alloc>` is a drop-in replacement for `std::vector<T>` that avoids heap allocation when the
number of elements is small. Elements up to `kInlineCap` live entirely inside the object's inline buffer; only when that
threshold is exceeded does the container spill to the allocator.

Design goals:

- **Zero overhead** for the common case (elements fit inline).
- **No virtual functions, no RTTI, no macros** — zero-cost pay-per-use design.
- **Full allocator-traits compliance** — integrates with custom memory pools.
- **`std::vector`-compatible subset** — iterators are raw pointers; all standard algorithms work.

---

## 2. InlineBytes Semantics — Bytes, Not Count

`InlineBytes` is a **byte budget**, not an element count. This is intentional: it lets the caller express the budget in
cache-line terms (e.g. 64 bytes = one cache line) without knowing `sizeof(T)` at the call site.

```cpp
SmallVector<int,    64>  // budget=64B → 16 ints inline
SmallVector<double, 64>  // budget=64B →  8 doubles inline
SmallVector<char,   32>  // budget=32B → 32 chars inline
SmallVector<int,     2>  // budget<sizeof(int) → 0 ints inline (heap-only)
```

The inline storage is declared as `alignas(T) std::byte inline_[kBufBytes]`, where
`kBufBytes = kInlineCap > 0 ? InlineBytes : 1`. The single-byte fallback avoids zero-length arrays (which are a GCC
extension, not standard C++).

---

## 3. kInlineCap Formula

```cpp
static constexpr size_type kInlineCap = detail::inline_cap<T, InlineBytes>;
// where:
//   inline_cap<T, N> = (N >= sizeof(T)) ? (N / sizeof(T)) : 0
```

- Result is `0` when `T` is larger than the byte budget → heap-only mode.
- Truncating integer division: partial elements do not fit.
- Compile-time constant — no runtime branch on the inline/heap path.

---

## 4. Spill Semantics

`data_` always points to the live element range — either into `inline_` or into allocator-owned heap storage. No
separate "spilled" flag is stored; `is_inline()` compares `data_ == inline_ptr()`, which is branch-free.

**Spill conditions:**

- `push_back` / `emplace_back` when `size_ == cap_`.
- `reserve(n)` when `n > cap_`.
- `resize(n)` when `n > cap_`.

**Spill does not happen automatically on shrink.** After removing elements, the vector remains spilled until
`shrink_to_fit()` is called. `shrink_to_fit()` is a no-op if `T` is not nothrow-move-constructible (refuses to leave a
partially-moved mess).

**Growth policy:** `cap_ = cap_ + cap_/2 + 1` (≈ 1.5×), overflow-safe near `SIZE_MAX`.

---

## 5. Allocator Integration

`Alloc` defaults to `std::allocator<T>`. Custom allocators follow the standard `std::allocator_traits` protocol.

Propagation behaviour is fully respected:
| Trait | Effect | |---|---| | `propagate_on_container_copy_assignment` | Allocator propagated on copy-assign if
different and trait is true | | `propagate_on_container_move_assignment` | Heap pointer stolen on move-assign when
allocators match or POCMA is true | | `propagate_on_container_swap` | Allocators swapped (or `assert` fires if unequal
and trait is false) |

**Smriti allocator example** (bump pool spill):

```cpp
pools::BumpPool<domains::SystemRAMDomain> pool{4096};
SmritiAllocator<int, decltype(pool)> alloc{pool};
SmallVector<int, 64, decltype(alloc)> v{alloc};
```

When allocators differ on move construction/assignment, the container falls back to element-wise moves (cannot steal
heap storage).

---

## 6. API Reference

### Types

| Name                                          | Description                                    |
|-----------------------------------------------|------------------------------------------------|
| `value_type`                                  | `T`                                            |
| `size_type`                                   | `std::size_t`                                  |
| `iterator` / `const_iterator`                 | `T*` / `const T*` — raw pointer, zero overhead |
| `reverse_iterator` / `const_reverse_iterator` | `std::reverse_iterator<...>`                   |
| `kInlineCap`                                  | Compile-time inline element capacity           |

### Construction

| Signature                                | Notes                                                                           |
|------------------------------------------|---------------------------------------------------------------------------------|
| `SmallVector()`                          | Empty, inline storage active                                                    |
| `SmallVector(const Alloc&)`              | With explicit allocator                                                         |
| `SmallVector(size_type n)`               | Default-constructed elements                                                    |
| `SmallVector(size_type n, const T& val)` | Fill constructor                                                                |
| `SmallVector(initializer_list<T>)`       | Element list                                                                    |
| `SmallVector(It first, It last)`         | Range constructor; reserves ahead for forward iterators                         |
| `SmallVector(const SmallVector&)`        | Deep copy; uses `select_on_container_copy_construction`                         |
| `SmallVector(SmallVector&&)`             | Steals heap or element-moves inline; noexcept when T and Alloc are nothrow-move |

### Element Access

`operator[]`, `at` (bounds-checked, throws `std::out_of_range`), `front`, `back`, `data`.

### Iterators

`begin`/`end`, `cbegin`/`cend`, `rbegin`/`rend`, `crbegin`/`crend`.

### Capacity

| Method            | Notes                                                              |
|-------------------|--------------------------------------------------------------------|
| `size()`          | Current element count                                              |
| `capacity()`      | Current storage capacity                                           |
| `empty()`         | True if size == 0                                                  |
| `spilled()`       | True if storage is on the heap (not inline)                        |
| `reserve(n)`      | Grow to at least n; no shrink                                      |
| `shrink_to_fit()` | Collapse back to inline if size ≤ kInlineCap and T is nothrow-move |

### Modifiers

| Method                         | Exception guarantee                                           |
|--------------------------------|---------------------------------------------------------------|
| `push_back(const T&)`          | Strong                                                        |
| `push_back(T&&)`               | Strong                                                        |
| `emplace_back(Args&&...)`      | Strong; returns `reference` to new element                    |
| `pop_back()`                   | noexcept                                                      |
| `resize(n)` / `resize(n, val)` | Basic                                                         |
| `clear()`                      | noexcept                                                      |
| `erase(pos)`                   | noexcept(nothrow-move-assign)                                 |
| `erase(first, last)`           | noexcept(nothrow-move-assign)                                 |
| `insert(pos, val)`             | Precondition: val must not alias any element (self-insert UB) |
| `insert(pos, T&&)`             | Strong                                                        |
| `swap(SmallVector&)`           | noexcept(nothrow-move-construct and nothrow-swappable-alloc)  |

### Comparison

`operator==` (lexicographic element-wise; requires `std::equality_comparable<T>`), `operator<=>` (requires
`std::three_way_comparable<T>`).

---

## 7. Known Limitations

| Limitation                                                  | Detail                                                                                                                                                                                                                                                                       |
|-------------------------------------------------------------|------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| **Self-insert UB**                                          | `insert(pos, val)` has a precondition that `val` does not alias any element in `[data_, data_+size_)`. Inserting a reference to an existing element while the buffer is full will trigger a grow-then-use-dangling-reference bug. Enforced by `assert` in debug builds only. |
| **No `insert(pos, count, val)`**                            | Multi-copy insert is not implemented.                                                                                                                                                                                                                                        |
| **No `assign(n, val)`**                                     | Fill-assign is not implemented; use `resize` + fill.                                                                                                                                                                                                                         |
| **No `insert(pos, first, last)`**                           | Range-insert is not implemented.                                                                                                                                                                                                                                             |
| **`shrink_to_fit` is a no-op for non-nothrow-move T**       | Prevents leaving elements in a partially-moved-from state on exception.                                                                                                                                                                                                      |
| **`swap` asserts on unequal allocators when POCS is false** | Standard-conforming behaviour; mismatched allocator swap is undefined.                                                                                                                                                                                                       |

---

## 8. Comparison with `llvm::SmallVector` and `absl::InlinedVector`

| Feature                   | `SmallVector` (this)           | `llvm::SmallVector`      | `absl::InlinedVector`             |
|---------------------------|--------------------------------|--------------------------|-----------------------------------|
| Inline budget parameter   | Bytes (`InlineBytes`)          | Element count (`N`)      | Element count (`N`)               |
| Allocator support         | Full traits (POCCA/POCMA/POCS) | `std::allocator` only    | Custom allocator                  |
| `std::expected` errors    | No (uses exceptions / assert)  | No                       | No                                |
| Self-insert safe          | No (assert in debug)           | Yes (copies first)       | Yes                               |
| `shrink_to_fit` to inline | Yes (nothrow-move only)        | No                       | No                                |
| Namespace / header        | `containers::dynamic`          | `llvm/ADT/SmallVector.h` | `absl/container/inlined_vector.h` |
| Dependencies              | Standard library only          | LLVM                     | Abseil                            |
| `spilled()` query         | Yes                            | No                       | No                                |

Key design difference: expressing the budget in **bytes** rather than counts lets the user reason about cache-line
alignment without knowing `sizeof(T)`. Expressing it in counts requires a separate decision per element type.

---

## 9. Examples

### Basic usage (inline stays on stack)

```cpp
#include "containers/dynamic/SmallVector.hpp"
using namespace containers::dynamic;

SmallVector<int, 64> v;      // 16 ints inline
v.push_back(1);
v.push_back(2);
assert(!v.spilled());        // still inline

v.reserve(100);              // now on heap
assert(v.spilled());
```

### Custom allocator (smriti bump pool)

```cpp
pools::BumpPool<domains::SystemRAMDomain> pool{4096};
SmritiAllocator<int, decltype(pool)> alloc{pool};

SmallVector<int, 64, decltype(alloc)> v{alloc};
for (int i = 0; i < 1000; ++i) v.push_back(i);
// all heap allocations served from pool
```

### Heap-only mode (`InlineBytes < sizeof(T)`)

```cpp
SmallVector<int, 2> v;       // kInlineCap == 0
v.push_back(42);
assert(v.spilled());         // immediately on heap
```

### Shrink back to inline

```cpp
SmallVector<int, 64> v;
for (int i = 0; i < 50; ++i) v.push_back(i);
assert(v.spilled());

while (v.size() > 10) v.pop_back();
v.shrink_to_fit();
assert(!v.spilled());        // collapsed back to inline
```
