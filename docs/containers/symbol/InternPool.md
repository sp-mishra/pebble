# `InternPool` — Thread-Safe String Intern Pool

> **Header:** `include/containers/symbol/InternPool.hpp`
>
> **Namespace:** `symtab`
> **Standard required:** C++23 (`-std=c++23`)
> **Optional dependency:** Google Highway (`SYMTAB_ENABLE_HIGHWAY`) for `batch_intern`

---

## Table of Contents

1. [Purpose](#1-purpose)
2. [Core Guarantee: Pointer Stability](#2-core-guarantee-pointer-stability)
3. [Thread Safety Model](#3-thread-safety-model)
4. [Error Handling](#4-error-handling)
5. [API Reference](#5-api-reference)
6. [SYMTAB_ENABLE_HIGHWAY Flag](#6-symtab_enable_highway-flag)
7. [clear() Invalidation Warning](#7-clear-invalidation-warning)
8. [Usage Examples](#8-usage-examples)
9. [Performance Characteristics](#9-performance-characteristics)
10. [Design Notes](#10-design-notes)

---

## 1. Purpose

`InternPool` stores each unique string **exactly once**. Every `intern()` call for the same content returns a
`std::string_view` pointing into the same underlying node — callers use **pointer equality** instead of full string
comparison on hot paths.

Primary use case: interning symbol names so that `SymbolTable` can key its hash map on `string_view` pointer identity,
reducing hash and comparison cost on the resolve path.

---

## 2. Core Guarantee: Pointer Stability

`InternPool` uses `std::unordered_set<std::string>`, which is node-based. `insert()` never relocates existing nodes.
Therefore:

- All `string_view` values returned by `intern()` remain valid **for the entire lifetime of the pool**, or until
  `clear()` is called.
- Two `intern()` calls for the same string content will return views with **identical `.data()` pointers**.

```cpp
symtab::InternPool pool;
auto sv1 = pool.intern("hello").value();
auto sv2 = pool.intern("hello").value();
assert(sv1.data() == sv2.data());  // same pointer
```

---

## 3. Thread Safety Model

| Operation             | Lock type       | Notes                                                         |
|-----------------------|-----------------|---------------------------------------------------------------|
| `intern()`            | Shared → Unique | Fast path: shared read. Miss: upgrades to exclusive write.    |
| `intern_or_throw()`   | Same as intern  | Asserts non-empty; throws `std::bad_expected_access` on empty |
| `contains()`          | Shared read     | Non-mutating                                                  |
| `size()`              | Shared read     | Non-mutating                                                  |
| `reserve()`           | Exclusive write | Pre-sizes internal hash set                                   |
| `clear()`             | Exclusive write | Invalidates all previously returned `string_view` values      |
| `all()`               | Shared read     | Returns unlocked snapshot copy; safe after lock release       |
| `intern_call_count()` | None (atomic)   | `memory_order_relaxed` read; counts every `intern()` call     |

Multiple threads may call `intern()` concurrently without external synchronization.

---

## 4. Error Handling

```cpp
enum class InternError : std::uint8_t {
    EmptyString, // intern("") rejected
    PoolCleared, // reserved for future use; returned view invalidated by clear()
};

template <typename T>
using InternResult = std::expected<T, InternError>;
```

`intern()` returns `std::expected<std::string_view, InternError>`. An empty string returns
`std::unexpected(InternError::EmptyString)`.

`intern_or_throw()` calls `.value()` — throws `std::bad_expected_access<InternError>` on empty input. Use only where
empty string is a programming error.

---

## 5. API Reference

### Constructors

```cpp
InternPool();                                    // default
explicit InternPool(std::size_t initial_capacity); // pre-sizes hash set
InternPool(const InternPool&) = delete;
InternPool& operator=(const InternPool&) = delete;
InternPool(InternPool&&) noexcept;               // locks both; transfers store_
InternPool& operator=(InternPool&&) noexcept;    // same
```

Move locks both source and destination under `std::scoped_lock` to prevent races with concurrent `intern()` callers.

### Core Operations

```cpp
[[nodiscard]] InternResult<std::string_view> intern(std::string_view s);
[[nodiscard]] std::string_view               intern_or_throw(std::string_view s);

[[nodiscard]] bool        contains(std::string_view s) const;
[[nodiscard]] std::size_t size() const;
[[nodiscard]] std::size_t intern_call_count() const noexcept; // total intern() calls
void                      reserve(std::size_t n);
void                      clear();
[[nodiscard]] std::vector<std::string> all() const;          // snapshot copy
```

### Batch Intern (Highway only)

```cpp
#ifdef SYMTAB_ENABLE_HIGHWAY
[[nodiscard]] std::size_t batch_intern(
    std::span<const char* const> names,
    std::span<std::string_view>  out);
#endif
```

See [§6](#6-symtab_enable_highway-flag).

---

## 6. `SYMTAB_ENABLE_HIGHWAY` Flag

Define `SYMTAB_ENABLE_HIGHWAY` before including the header (or via CMake) to unlock `batch_intern()`.

```cmake
target_compile_definitions(my_target PRIVATE SYMTAB_ENABLE_HIGHWAY)
```

`batch_intern(names, out)` interns each null-terminated pointer in `names` and writes the resulting `string_view` into
`out`. `names.size()` must equal `out.size()`. Returns the count of failed interns (empty strings).

Without the flag, only single-string `intern()` is available.

---

## 7. `clear()` Invalidation Warning

`clear()` **invalidates all previously returned `string_view` values**. Any pointer obtained from `intern()` before the
call becomes dangling.

```cpp
symtab::InternPool pool;
auto sv = pool.intern("foo").value();
pool.clear();
// sv.data() is now dangling — DO NOT dereference
```

`SymbolTable` depends on pool stability. Do **not** call `pool_.clear()` directly while a `SymbolTable` instance is
live — the table's hash-map keys would become dangling pointers.

After `clear()`, `intern_call_count()` resets to zero.

---

## 8. Usage Examples

### Basic interning

```cpp
#include "containers/symbol/InternPool.hpp"
using namespace symtab;

InternPool pool;

auto r1 = pool.intern("lithe::runtime::foo");
auto r2 = pool.intern("lithe::runtime::foo");

assert(r1.has_value());
assert(r1->data() == r2->data());  // same pointer, no copy
```

### Error handling

```cpp
auto r = pool.intern("");
if (!r) {
    // r.error() == InternError::EmptyString
}
```

### Pre-sizing for bulk load

```cpp
InternPool pool(10'000);  // pre-allocate buckets
for (const auto& name : symbol_names)
    (void)pool.intern(name);
```

### Move semantics

```cpp
InternPool src(100);
(void)src.intern("alpha");

InternPool dst(std::move(src));
// dst has "alpha"; src is empty
assert(dst.contains("alpha"));
assert(src.size() == 0);
```

### Concurrent use

```cpp
InternPool pool;

std::vector<std::thread> threads;
for (int i = 0; i < 8; ++i) {
    threads.emplace_back([&pool, i] {
        auto sv = pool.intern("shared_key").value();
        (void)sv;
        auto priv = pool.intern("thread_" + std::to_string(i)).value();
        (void)priv;
    });
}
for (auto& t : threads) t.join();
```

---

## 9. Performance Characteristics

| Operation             | Complexity  | Notes                                       |
|-----------------------|-------------|---------------------------------------------|
| `intern()` hit        | O(1)        | Shared lock + hash lookup                   |
| `intern()` miss       | O(1) amort. | Exclusive lock + hash insert                |
| `contains()`          | O(1)        | Shared lock + hash lookup                   |
| `size()`              | O(1)        | Shared lock                                 |
| `intern_call_count()` | O(1)        | Atomic load, no lock                        |
| `all()`               | O(n)        | Copies all strings under shared lock        |
| `clear()`             | O(n)        | Exclusive lock; destroys all stored strings |

Lock contention is minimized by the two-phase intern: shared read lock on cache hit, exclusive write only on miss.

---

## 10. Design Notes

- **Transparent hash/equal** (`StringHash`, `StringEqual` with `is_transparent`) allows `contains()` and `find()` to
  accept `string_view` without constructing a `std::string`.
- **Atomic `intern_count_`**: counts every `intern()` call including fast-path cache hits; useful for profiling symbol
  registration pressure.
- **Move constructor locks both objects** under `std::scoped_lock(mtx_, other.mtx_)` to safely interleave with
  concurrent `intern()` calls on either instance.
- **Copy is deleted**: copying a pool would duplicate the backing store but the copy's string pointers would be
  distinct — defeating the single-pointer-identity guarantee.
