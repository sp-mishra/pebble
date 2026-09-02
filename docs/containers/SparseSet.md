# `SparseSet` — Briggs–Torczon Sparse Set with Optional Satellite Data

> **Header:** `include/containers/associative/SparseSet.hpp`
> **Namespace:** `sparseset`
> **Standard required:** C++23 (`-std=c++2b`)
> **Dependencies:** Standard library only

---

## Table of Contents

1. [Purpose](#1-purpose)
2. [Briggs–Torczon Algorithm](#2-briggstorczon-algorithm)
3. [O (1) Operations Explained](#3-o1-operations-explained)
4. [Universe Capacity Concept](#4-universe-capacity-concept)
5. [kInvalid Sentinel](#5-kinvalid-sentinel)
6. [insert vs insert_or_update Semantics](#6-insert-vs-insert_or_update-semantics)
7. [clear () O (n) vs reset () O (universe) Tradeoff](#7-clearon-vs-resetouniverse-tradeoff)
8. [Set-Theoretic Operations — Value-Dropping Behaviour](#8-set-theoretic-operations--value-dropping-behaviour)
9. [IndexT Overflow Risk](#9-indext-overflow-risk)
10. [has_value Compile-Time Dispatch](#10-has_value-compile-time-dispatch)
11. [Iterator Model](#11-iterator-model)
12. [Allocator Integration](#12-allocator-integration)
13. [API Reference](#13-api-reference)
14. [Examples](#14-examples)

---

## 1. Purpose

`SparseSet<Key, Value, IndexT, DenseAlloc, SparseAlloc>` is a high-performance set/map optimized for **integer or
enum-class keys with bounded universe**. Core operations (`insert`, `remove`, `contains`) are O (1) and branch-free on
the hot path. Dense iteration is O (n) with perfect cache locality.

Design goals:

- **O (1) insert/remove/contains** — no hashing, no probing, no tree rotations.
- **O (n) dense iteration** — elements stored packed, contiguous; ideal for ECS, bitmask-style workloads.
- **Optional satellite data** — acts as a pure set when `Value = std::monostate`, as a map otherwise.
- **No virtual functions, no macros** — zero-cost pay-per-use design.
- **`std::expected` error returns** — no exceptions thrown by the container.

---

## 2. Briggs–Torczon Algorithm

Originally described in "Sparse Sets" (Briggs & Torczon, 1993) for compiler liveness analysis. The structure uses two
arrays:

```
sparse[key]  → dense index (or kInvalid if not present)
dense[i]     → {key, value}
```

**Membership check:** `sparse[key] != kInvalid` and `sparse[key] < size` and `dense[sparse[key]].key == key`. The
cross-validation (`dense[sparse[key]].key == key`) is the classical Briggs–Torczon trick that makes membership testing
safe against stale values in `sparse` without initialising it. This implementation always initialises `sparse` to
`kInvalid`, so the cross-check is implicit rather than load-bearing — but the two-array invariant is maintained at all
times.

**Remove (swap-and-pop):** To remove key `k`:

1. Look up its dense index `pos = sparse[k]`.
2. Move `dense.back()` into `dense[pos]`.
3. Update `sparse[dense[pos].key] = pos`.
4. `dense.pop_back()`.
5. `sparse[k] = kInvalid`.

This keeps the dense array contiguous without a gap at O (1) cost. It changes the iteration order (the last element
moves to the removed slot).

---

## 3. O (1) Operations Explained

| Operation           | Complexity     | Why                                        |
|---------------------|----------------|--------------------------------------------|
| `contains(k)`       | O(1)           | Single array read: `sparse[k] != kInvalid` |
| `insert(k, v)`      | O(1) amortised | Append to dense; write to sparse           |
| `remove(k)`         | O(1)           | Swap-and-pop dense; two sparse writes      |
| `get(k)`            | O(1)           | Two array reads                            |
| `dense_index_of(k)` | O(1)           | One sparse read                            |

All of these are branch-free on the hot path (no hash collision resolution, no tree traversal). The only exception is
`insert_or_update`, which may call `sparse_.resize()` if the key exceeds the current universe capacity — that
reallocation is O (universe) but amortised over many calls.

---

## 4. Universe Capacity Concept

`capacity()` is the **universe bound**: keys must satisfy `0 ≤ key < capacity()`. This is not the number of elements
(use `size()` for that).

- `SparseSet(256)` creates a universe that can hold keys 0–255.
- `insert(k)` returns `SSError::KeyOutOfRange` if `k >= capacity()`.
- `insert_or_update(k, v)` auto-reserves: it silently grows the sparse array to `k+1` if needed. This is the only
  mutating operation that never rejects out-of-range keys.
- `reserve(n)` grows the universe without shrinking.

The sparse array is pre-allocated to `universe_capacity` elements, so a universe of 1M keys costs 1M × `sizeof(IndexT)`
bytes regardless of how many keys are actually inserted.

---

## 5. kInvalid Sentinel

```cpp
static constexpr IndexT kInvalid = std::numeric_limits<IndexT>::max();
```

Slots in the sparse array are initialised to `kInvalid`. A slot holding `kInvalid` means "key not present". Because
`kInvalid` equals the maximum value of `IndexT`, the set cannot hold more than `kInvalid` elements simultaneously (the
size guard in `insert` enforces this — see [IndexT Overflow Risk](#9-indext-overflow-risk)).

With the default `IndexT = uint32_t`, `kInvalid = 4 294 967 295`. A single SparseSet can hold up to 4 294 967 294
elements.

---

## 6. insert vs insert_or_update Semantics

| Method                   | Key present                           | Key absent                   | Out-of-range key                   |
|--------------------------|---------------------------------------|------------------------------|------------------------------------|
| `insert(k, v)`           | `SSError::KeyAlreadyExists`           | Inserts; returns dense index | `SSError::KeyOutOfRange`           |
| `insert_or_update(k, v)` | Overwrites value; returns dense index | Inserts; returns dense index | Auto-reserves (grows sparse array) |

Use `insert` when duplicate insertion is a programmer error you want to catch. Use `insert_or_update` as an upsert
primitive when re-insertion with a new value is intentional, or when you want to avoid a prior `contains` check.

For pure-set usage (`Value = std::monostate`), `insert(k)` and `insert_or_update(k)` behave identically on absent keys;
the distinction matters only for map usage.

---

## 7. clear () O (n) vs reset () O (universe) Tradeoff

| Method                    | Complexity                    | Cost                                                    |
|---------------------------|-------------------------------|---------------------------------------------------------|
| `clear()`                 | O(n) where n = `size()`       | Iterates dense array; resets only occupied sparse slots |
| `reset()` / `clear_all()` | O(universe) = O(`capacity()`) | Resets entire sparse array via `assign`                 |

**When to use each:**

- Use `clear()` when `size() << capacity()` — e.g. a universe of 1 000 000 keys with 100 live elements; clearing only
  touches 100 sparse slots.
- Use `reset()` when the universe is small, or when you need a guaranteed-clean sparse array (e.g. after bulk
  `insert_range` that may have left stale writes, though the current implementation never does).

`clear()` preserves `capacity()`. Neither method deallocates memory.

---

## 8. Set-Theoretic Operations — Value-Dropping Behaviour

`intersection`, `union_with`, and `difference` are restricted to pure-set usage:

```cpp
SparseSet::intersection(...) requires (!has_value)
SparseSet::union_with(...)   requires (!has_value)
SparseSet::difference(...)   requires (!has_value)
```

The `requires (!has_value)` constraint is a compile-time guard. It prevents silently dropping satellite data: if
`Value != std::monostate`, these operations would need to decide which value to keep when a key appears in both sets.
Rather than picking an arbitrary policy, the API forces map users to implement their own set operations.

All three operations produce a new `SparseSet` whose capacity is determined by the input sizes. They do not modify
`*this` or `other`.

---

## 9. IndexT Overflow Risk

`IndexT` is the internal index type (default `uint32_t`). It controls both memory footprint and the maximum element
count:

- `uint32_t` (default): 4 bytes/sparse slot, max ≈ 4B elements.
- `uint16_t`: 2 bytes/sparse slot, max 65 534 elements.
- `uint8_t`: 1 byte/sparse slot, max 254 elements (key 255 is reserved as `kInvalid`).

The `insert` implementation guards against overflow:

```cpp
if (dense_.size() >= static_cast<size_type>(std::numeric_limits<IndexT>::max()))
    return std::unexpected(SSError::KeyOutOfRange);
```

With `IndexT = uint8_t` and a universe of 256:

- Keys 0–254 can be inserted.
- Key 255 equals `kInvalid`, so `sparse_[255]` is always treated as "not present". The size guard fires before the
  sparse write, returning `KeyOutOfRange`.
- Effective maximum: 255 elements (not 256).

Choose `IndexT` based on the maximum expected set size, not the universe size.

---

## 10. has_value Compile-Time Dispatch

```cpp
static constexpr bool has_value = !std::is_same_v<Value, std::monostate>;
```

Methods gated by `requires has_value`: `get`, `all_values`, `all_pairs`, `insert_or_update` (value overloads).

Methods gated by `requires (!has_value)`: `intersection`, `union_with`, `difference`.

This dispatch is resolved entirely at compile time. There is no runtime branching on `has_value`. The
`[[no_unique_address]]` attribute on the `val` member of `Entry` ensures that `std::monostate` values consume no
storage.

---

## 11. Iterator Model

The default iterators (`begin`/`end`) yield **keys only**, in dense-array order (insertion order, modulo swap-and-pop on
remove):

```cpp
SparseSet<uint32_t> s(16);
s.insert(5u); s.insert(3u); s.insert(8u);
for (uint32_t k : s) { /* k = 5, 3, 8 */ }
```

`const_iterator` is a **random-access iterator** (category: `std::random_access_iterator_tag`) backed by a pointer into
the dense array. All pointer arithmetic and comparison operators are supported.

**Range views** (lazy, zero-copy):
| View | Type | Notes | |---|---|---| | `all_keys()` | `const Key&` | Same as iterating directly | | `all_values()` |
`Value&` / `const Value&` | Requires `has_value` | | `all_pairs()` | `pair<const Key&, Value&>` | Requires
`has_value` | | `dense_entries()` | `std::span<const Entry>` | Raw entry access; `Entry` has `.key` and `.val` | |
`sparse_array()` | `std::span<const IndexT>` | Introspection / debugging |

Iteration order is **not sorted by key**. If sorted iteration is needed, copy keys and `std::ranges::sort`.

---

## 12. Allocator Integration

Two independent allocator parameters:

- `DenseAlloc` — allocator for the `std::vector<Entry>` dense array.
- `SparseAlloc` — allocator for the `std::vector<IndexT>` sparse array.

Both default to `std::allocator`. Pass custom allocators (e.g. smriti pool allocators) for arena-backed usage:

```cpp
SparseSet<uint32_t, std::monostate, uint32_t,
          SmritiAllocator<std::pair<uint32_t,std::monostate>, Pool>,
          SmritiAllocator<uint32_t, Pool>> s{universe, dense_alloc, sparse_alloc};
```

---

## 13. API Reference

### Template Parameters

| Parameter     | Default                           | Description                                                                        |
|---------------|-----------------------------------|------------------------------------------------------------------------------------|
| `Key`         | —                                 | Must satisfy `SparseKey` (unsigned integer or enum class with unsigned underlying) |
| `Value`       | `std::monostate`                  | Satellite data per key; `monostate` = pure set                                     |
| `IndexT`      | `uint32_t`                        | Internal index type; affects memory and max element count                          |
| `DenseAlloc`  | `std::allocator<pair<Key,Value>>` | Allocator for dense array                                                          |
| `SparseAlloc` | `std::allocator<IndexT>`          | Allocator for sparse array                                                         |

### Constants

| Name        | Value                           | Meaning                                    |
|-------------|---------------------------------|--------------------------------------------|
| `kInvalid`  | `numeric_limits<IndexT>::max()` | Sentinel for "not present" in sparse array |
| `has_value` | `!is_same_v<Value, monostate>`  | True when the set carries satellite data   |

### Construction

| Signature                         | Notes                      |
|-----------------------------------|----------------------------|
| `SparseSet()`                     | Empty; capacity = 0        |
| `SparseSet(universe_capacity)`    | Pre-allocates sparse array |
| `SparseSet(const SparseSet&)`     | Deep copy                  |
| `SparseSet(SparseSet&&)` noexcept | Move                       |

### Capacity

| Method       | Returns     | Notes                                  |
|--------------|-------------|----------------------------------------|
| `capacity()` | `size_type` | Universe bound; keys must be < this    |
| `size()`     | `size_type` | Number of elements currently present   |
| `empty()`    | `bool`      | True if size == 0                      |
| `reserve(n)` | `void`      | Grow universe to at least n; no shrink |

### Mutating Operations

| Method                     | Returns                     | Notes                                   |
|----------------------------|-----------------------------|-----------------------------------------|
| `insert(k[, v])`           | `expected<IndexT, SSError>` | Fails if key exists or out-of-range     |
| `insert_or_update(k[, v])` | `IndexT`                    | Upsert; auto-reserves; never fails      |
| `remove(k)`                | `expected<void, SSError>`   | Swap-and-pop; fails if key absent       |
| `clear()`                  | `void`                      | O(size); resets occupied sparse slots   |
| `reset()` / `clear_all()`  | `void`                      | O(universe); resets entire sparse array |
| `insert_range(rng)`        | `void`                      | Insert from range; skips duplicates     |
| `remove_range(rng)`        | `void`                      | Remove from range; skips absent keys    |

### Lookup

| Method              | Returns                                             | Notes                                         |
|---------------------|-----------------------------------------------------|-----------------------------------------------|
| `contains(k)`       | `bool`                                              | O(1)                                          |
| `get(k)`            | `expected<reference_wrapper<Value>, SSError>`       | Requires `has_value`                          |
| `get(k) const`      | `expected<reference_wrapper<const Value>, SSError>` | Requires `has_value`                          |
| `dense_index_of(k)` | `expected<IndexT, SSError>`                         | Returns position in dense array               |
| `contains_all(rng)` | `bool`                                              | True iff every key in range is present        |
| `contains_any(rng)` | `bool`                                              | True iff at least one key in range is present |

### Set Operations (pure-set only)

| Method                | Notes                                                 |
|-----------------------|-------------------------------------------------------|
| `intersection(other)` | Keys in both sets; requires `!has_value`              |
| `union_with(other)`   | Keys in either set; requires `!has_value`             |
| `difference(other)`   | Keys in `*this` not in `other`; requires `!has_value` |

### Free Functions

| Function                                     | Notes                               |
|----------------------------------------------|-------------------------------------|
| `make_sparse_set<Key, Value>(universe, rng)` | Build from a key range              |
| `operator==(a, b)`                           | Key-set equality; order-independent |

---

## 14. Examples

### Pure set usage

```cpp
#include "containers/associative/SparseSet.hpp"
using namespace sparseset;

SparseSet<uint32_t> s(256);
s.insert(7u);
s.insert(42u);
assert(s.contains(7u));
assert(s.size() == 2);

s.remove(7u);
assert(!s.contains(7u));
assert(s.size() == 1);
```

### Map usage with satellite data

```cpp
SparseSet<uint32_t, std::string> m(64);
m.insert(1u, "entity_one");
m.insert(2u, "entity_two");

auto val = m.get(1u);
assert(val.has_value());
assert(val->get() == "entity_one");

m.insert_or_update(1u, "updated");
assert(m.get(1u)->get() == "updated");
```

### Enum class key (ECS pattern)

```cpp
enum class EntityId : uint32_t {};
SparseSet<EntityId> active(1024);

auto eid = static_cast<EntityId>(7u);
active.insert(eid);
assert(active.contains(eid));
```

### Set-theoretic operations

```cpp
SparseSet<uint32_t> a(16), b(16);
for (uint32_t i = 0; i < 6; ++i) a.insert(i);
for (uint32_t i = 3; i < 9; ++i) b.insert(i);

auto c = a.intersection(b); // {3, 4, 5}
auto u = a.union_with(b);   // {0, 1, 2, 3, 4, 5, 6, 7, 8}
auto d = a.difference(b);   // {0, 1, 2}
```

### small IndexT for memory-constrained environments

```cpp
// 1 byte per sparse slot; universe max 256; element max 255
SparseSet<uint8_t, std::monostate, uint8_t> s(256);
for (uint8_t i = 0; i < 255; ++i) s.insert(i);
auto r = s.insert(uint8_t{255}); // KeyOutOfRange: kInvalid sentinel
assert(!r.has_value());
```

### Dense iteration (cache-friendly)

```cpp
SparseSet<uint32_t, float> scores(1024);
scores.insert_or_update(0u, 1.0f);
scores.insert_or_update(5u, 2.5f);
scores.insert_or_update(999u, 0.1f);

for (auto [key, val] : scores.all_pairs()) {
    // sequential dense access — no sparse indirection
    val *= 2.0f;
}
```
