# `SymbolTable` & `NamespaceIndex` — Concurrent Symbol Registry

> **Header:** `include/containers/symbol/SymbolTable.hpp`
>
> **Namespace:** `symtab`
> **Standard required:** C++23 (`-std=c++23`)
> **Depends on:** `InternPool.hpp`, `containers/tree/NAryTree.hpp`

---

## Table of Contents

1. [Architecture Overview](#1-architecture-overview)
2. [Three-Component Model](#2-three-component-model)
3. [Lock Acquisition Order](#3-lock-acquisition-order)
4. [SymbolTable API](#4-symboltable-api)
5. [Snapshot / Rollback Semantics](#5-snapshot--rollback-semantics)
6. [NamespaceIndex](#6-namespaceindex)
7. [NamespaceIndex Raw-Pointer Ownership](#7-namespaceindex-raw-pointer-ownership)
8. [Versioning Semantics](#8-versioning-semantics)
9. [Usage Examples](#9-usage-examples)
10. [Performance Characteristics](#10-performance-characteristics)
11. [Design Notes](#11-design-notes)

---

## 1. Architecture Overview

The symbol subsystem is built from three cooperating components:

```
 ┌─────────────────────────────────────────────────────────────┐
 │  SymbolTable<Mutex>                                         │
 │                                                             │
 │  map_: unordered_map<string_view, symbol_entry>             │
 │        keys are stable string_views from InternPool         │
 │                                                             │
 │  insertion_order_: vector<string_view>  — for rollback      │
 │  pool_: InternPool  — owns backing string storage           │
 └─────────────────────────────────────────────────────────────┘

 ┌─────────────────────────────────────────────────────────────┐
 │  NamespaceIndex  (secondary, optional)                      │
 │                                                             │
 │  NAryTree<string_view, symbol_entry*>                       │
 │        non-owning trie of namespace components              │
 │  node_cache_: map<string_view, TNode*>  — O(1) prefix find  │
 └─────────────────────────────────────────────────────────────┘

 ┌─────────────────────────────────────────────────────────────┐
 │  symbol_entry  (plain data)                                 │
 │    name: string_view  — stable, owned by InternPool         │
 │    address: void*                                           │
 │    version: uint32_t                                        │
 │    id: uint32_t  — assigned by SymbolTable                  │
 └─────────────────────────────────────────────────────────────┘
```

`SymbolTable` is the **primary O (1) lookup store**. `NamespaceIndex` is a **secondary non-owning index** for
namespace-scoped enumeration and LCA path queries. The two are independent: `NamespaceIndex` is not automatically
updated when `SymbolTable` changes.

---

## 2. Three-Component Model

### `basic_symbol_entry<Value>`

Plain data; carries no ownership. `name` is a `string_view` into `InternPool` storage and is stable for the pool's
lifetime.

```cpp
template <typename Value = void*>
struct basic_symbol_entry {
    std::string_view name;      // stable; owned by InternPool
    Value            address{};
    std::uint32_t    version{0};
    std::uint32_t    id{0};     // 0 = not yet assigned by SymbolTable
};

using symbol_entry = basic_symbol_entry<void*>;
```

### `SymbolTable<Value, Mutex, Map, Vector>`

Primary lookup store.
- `Value` defaults to `void*`.
- `Mutex` defaults to `std::shared_mutex`.
- `Map` defaults to `std::unordered_map<std::string_view, basic_symbol_entry<Value>, StringHash, StringEqual>`.
- `Vector` defaults to `containers::SmallVector<std::string_view, 64>` (4 elements inline without heap allocation).

Keys are interned `string_view` values — `resolve()` performs a hash lookup with no heap allocation under a shared lock.
Move operations (`SymbolTable(SymbolTable&&)` and `operator=(SymbolTable&&)`) are fully supported and `noexcept`. Copy operations remain deleted.

### `NamespaceIndex`

Wraps `NAryTree<string_view, symbol_entry*>`. Iterates namespace components on `"::"` via `DelimiterScanner` with zero heap allocation and maintains a trie
of namespace components. Used for `enumerate()` and LCA-based `path()` queries. **Not used for the O (1) resolve path.**

---

## 3. Lock Acquisition & Concurrency

String interning inside `register_symbol()` occurs outside the table's exclusive write lock. Because `InternPool` is self-synchronized,
concurrent registrations of distinct symbols across multiple threads do not block each other during string interning.

| Site                            | Lock Scope                     |
|---------------------------------|--------------------------------|
| `register_symbol()`             | pool::intern() (shared/unique) $\to$ SymbolTable::mtx_ (unique) |
| `register_range()`              | pre-interns $\to$ SymbolTable::mtx_ (unique) |
| `register_symbol_locked_()`     | caller holds SymbolTable::mtx_ |
| `resolve()`, `contains()`, etc. | SymbolTable::mtx_ (shared)     |

---

## 4. SymbolTable API

### Errors

```cpp
enum class SymError : std::uint8_t {
    AlreadyRegistered, // name exists at same or newer version
    NotFound,          // symbol not found
    InvalidName,       // empty name
    SnapshotUnderflow, // rollback target > current size
};
```

### SymbolId

```cpp
struct SymbolId {
    std::uint32_t value{0};
    [[nodiscard]] constexpr bool is_valid() const noexcept;
};
static constexpr SymbolId INVALID_SYMBOL_ID{};
```

### Core Operations

```cpp
// Register a symbol. Returns SymbolId on success.
[[nodiscard]] SymResult<SymbolId>
register_symbol(std::string_view name, Value addr, std::uint32_t version = 0);

// O(1) shared-lock resolve. Returns Value{} if not found.
[[nodiscard]] Value resolve(std::string_view name) const;

// Resolve with exact version check. Returns Value{} on mismatch.
[[nodiscard]] Value resolve_versioned(std::string_view name, std::uint32_t version) const;

// Membership test.
[[nodiscard]] bool contains(std::string_view name) const;

// Count of registered symbols.
[[nodiscard]] std::size_t size() const;

// Remove symbol. Returns true if existed.
bool unregister(std::string_view name);

// Bulk insert from any input range of symbol entries.
// Acquires write lock once for entire range.
template <std::ranges::input_range R>
    requires std::same_as<std::ranges::range_value_t<R>, entry_type>
std::size_t register_range(R&& entries);

// Copy of entry for inspection.
[[nodiscard]] SymResult<entry_type> lookup_entry(std::string_view name) const;
```

---

## 5. Snapshot / Rollback Semantics

`snapshot()` returns the current registered-symbol count. `rollback(n)` removes all symbols registered after position
`n` (in reverse insertion order).

```cpp
auto snap = tbl.snapshot();          // e.g. returns 2
(void)tbl.register_symbol("c", ...);
tbl.rollback(snap);                  // removes "c"; size back to 2
```

### Critical: InternPool is NOT rolled back

Strings interned into `pool_` for rolled-back symbols **persist for the pool's lifetime**. This is intentional — the
pool grows monotonically. Callers must not rely on `pool_.size()` matching `map_.size()` after a rollback.

Rationale: rolling back the pool would invalidate stable `string_view` pointers held by other data structures (e.g., a
`NamespaceIndex` sharing the same pool).

```
After rollback(2) with initial size 3:
  map_.size()  == 2  ← rolled back
  pool_.size() == 3  ← NOT rolled back; "c" still interned
```

---

## 6. NamespaceIndex

`NamespaceIndex` provides namespace-scoped symbol enumeration and LCA-based path queries over a trie built from `"::"`
-delimited name components.

### Constructors

```cpp
NamespaceIndex();                       // owns internal InternPool for prefix strings
explicit NamespaceIndex(InternPool&);   // caller-supplied pool (must outlive index)
NamespaceIndex(const NamespaceIndex&) = delete;
NamespaceIndex& operator=(const NamespaceIndex&) = delete;
```

### Operations

```cpp
// Insert a symbol entry into the trie. entry->name must be non-empty.
// Duplicate inserts (same entry->name) are idempotent — no second leaf is created.
void insert(symbol_entry* entry);

// Return all symbol_entry* under a namespace prefix.
// ns_prefix="" returns everything.
[[nodiscard]] std::vector<symbol_entry*> enumerate(std::string_view ns_prefix) const;

// Depth of a namespace node (1 = top-level, 2 = one level nested, ...).
// Returns 0 for the root ("") or an unknown prefix.
[[nodiscard]] std::size_t depth(std::string_view ns_prefix) const;

// Namespace component path between two prefixes via LCA.
// Returns nullopt if either prefix is not in the trie.
[[nodiscard]] std::optional<std::vector<std::string_view>>
path(std::string_view from_ns, std::string_view to_ns) const;
```

### Thread Safety

`insert` holds an exclusive write lock. `enumerate`, `depth`, and `path` hold a shared read lock. Safe for concurrent
readers + single writer.

---

## 7. NamespaceIndex Raw-Pointer Ownership

`NamespaceIndex` stores `symbol_entry*` — **raw non-owning pointers**. The entries must remain live for the entire
lifetime of the index.

**When using `NamespaceIndex` alongside `SymbolTable`:**

- `symbol_entry` objects live inside `SymbolTable::map_`.
- `SymbolTable::map_` may reallocate when new entries are inserted (unordered_map node-based, but pointer stability is
  for the node, not the map itself on resize — note: `std::unordered_map` is node-based and pointers/references to
  elements are stable).
- `rollback()` **erases** entries from `map_`, destroying the `symbol_entry` objects. Any `NamespaceIndex` pointers to
  those entries become dangling.

**Safe usage pattern:**

```
1. Populate SymbolTable fully (or take a snapshot).
2. Build NamespaceIndex from resolved entries — use lookup_entry() to get stable
   copies, or insert pointers only to entries you know will not be rolled back.
3. Do not call rollback() while NamespaceIndex holds pointers to affected entries.
```

**Lifetime rule: NamespaceIndex must not outlive the SymbolTable whose entries it indexes.**

---

## 8. Versioning Semantics

Each `symbol_entry` carries a `version` field (`uint32_t`, default `0`).

| Scenario                   | Result                                             |
|----------------------------|----------------------------------------------------|
| Register at same version   | `SymError::AlreadyRegistered`                      |
| Register at lower version  | `SymError::AlreadyRegistered` (downgrade rejected) |
| Register at higher version | Success — address and version updated in place     |
| `resolve()`                | Returns current address regardless of version      |
| `resolve_versioned(n)`     | Returns address only if stored version == n        |

A version upgrade preserves the original `SymbolId` — the id is stable across upgrades.

```cpp
(void)tbl.register_symbol("foo", &v1, 1);
(void)tbl.register_symbol("foo", &v2, 2);  // upgrade: ok, id unchanged
tbl.resolve("foo") == &v2;                 // latest address
tbl.resolve_versioned("foo", 1) == nullptr; // exact match only
tbl.resolve_versioned("foo", 2) == &v2;
```

---

## 9. Usage Examples

### Basic register and resolve

```cpp
#include "containers/symbol/SymbolTable.hpp"
using namespace symtab;

SymbolTable<> tbl;

void my_func() {}
auto id = tbl.register_symbol("my::module::my_func", reinterpret_cast<void*>(&my_func));
assert(id.has_value() && id->is_valid());

void* p = tbl.resolve("my::module::my_func");
assert(p == reinterpret_cast<void*>(&my_func));
```

### Version upgrade

```cpp
SymbolTable<> tbl;
int v1 = 1, v2 = 2;
(void)tbl.register_symbol("plugin::fn", &v1, 1);
(void)tbl.register_symbol("plugin::fn", &v2, 2);  // upgrade
assert(tbl.resolve("plugin::fn") == &v2);
```

### Snapshot and rollback

```cpp
SymbolTable<> tbl;
(void)tbl.register_symbol("core::a", &obj_a);
(void)tbl.register_symbol("core::b", &obj_b);
auto snap = tbl.snapshot();           // == 2

(void)tbl.register_symbol("temp::x", &obj_x);
assert(tbl.size() == 3);

tbl.rollback(snap);                   // removes temp::x
assert(tbl.size() == 2);
assert(tbl.resolve("temp::x") == nullptr);
// InternPool still contains "temp::x" — that is expected and safe.
```

### Bulk registration

```cpp
SymbolTable<> tbl;
std::vector<symbol_entry> batch = {
    {"a::fn1", &fn1, 0},
    {"a::fn2", &fn2, 0},
    {"b::fn3", &fn3, 0},
};
std::size_t count = tbl.register_range(batch);
assert(count == 3);
```

### NamespaceIndex enumeration

```cpp
NamespaceIndex idx;
symbol_entry e1{"lithe::runtime::foo", &foo, 0};
symbol_entry e2{"lithe::runtime::bar", &bar, 0};
symbol_entry e3{"lithe::codegen::baz", &baz, 0};
idx.insert(&e1);
idx.insert(&e2);
idx.insert(&e3);

auto runtime = idx.enumerate("lithe::runtime");
assert(runtime.size() == 2);  // foo, bar

auto all = idx.enumerate("");
assert(all.size() == 3);
```

### NamespaceIndex LCA path

```cpp
NamespaceIndex idx;
symbol_entry e1{"lithe::runtime::foo", &foo, 0};
symbol_entry e2{"lithe::codegen::bar", &bar, 0};
idx.insert(&e1);
idx.insert(&e2);

auto p = idx.path("lithe::runtime", "lithe::codegen");
assert(p.has_value());
// p contains the component names traversed: runtime -> lithe -> codegen
```

### Concurrent access

```cpp
SymbolTable<> tbl;
(void)tbl.register_symbol("stable::sym", &obj);

// Multiple readers, one writer — safe
std::thread writer([&] {
    for (int i = 0; i < 100; ++i)
        (void)tbl.register_symbol("w::sym_" + std::to_string(i), &obj);
});
std::thread reader([&] {
    for (int i = 0; i < 500; ++i)
        assert(tbl.resolve("stable::sym") == &obj);
});
writer.join();
reader.join();
```

---

## 10. Performance Characteristics

### SymbolTable

| Operation             | Complexity  | Notes                                               |
|-----------------------|-------------|-----------------------------------------------------|
| `register_symbol()`   | O(1) amort. | Exclusive lock; one intern + one hash insert        |
| `resolve()`           | O(1)        | Shared lock; hash lookup, no allocation             |
| `resolve_versioned()` | O(1)        | Shared lock; same as resolve + version check        |
| `contains()`          | O(1)        | Shared lock                                         |
| `unregister()`        | O(n)        | Exclusive lock; `std::erase` scans insertion_order_ |
| `register_range()`    | O(k)        | One exclusive lock for k entries                    |
| `snapshot()`          | O(1)        | Shared lock                                         |
| `rollback(d)`         | O(d)        | Exclusive lock; d = number of entries removed       |
| `lookup_entry()`      | O(1)        | Shared lock; returns copy                           |

### NamespaceIndex

| Operation           | Complexity | Notes                                         |
|---------------------|------------|-----------------------------------------------|
| `insert()`          | O(d)       | d = number of namespace components; trie walk |
| `enumerate(prefix)` | O(s)       | s = size of subtree under prefix; BFS         |
| `depth(prefix)`     | O(d)       | Parent-pointer walk; d = depth                |
| `path(a, b)`        | O(h)       | h = height of LCA path                        |

---

## 11. Design Notes

### Why SymbolTable is not movable or copyable

`SymbolTable::map_` keys are `string_view` into `pool_` (an embedded `InternPool`). Moving the table would move `pool_`,
which relocates its `std::unordered_set<std::string>` storage — **invalidating all existing `string_view` keys**. The
move constructor is therefore `= delete` to prevent silent UB.

```cpp
static_assert(!std::is_move_constructible_v<SymbolTable<>>);
static_assert(!std::is_copy_constructible_v<SymbolTable<>>);
```

### Interned keys on the hot resolve path

`resolve()` takes a `std::string_view` and does a transparent hash lookup — no temporary `std::string` allocation, no
string copy. On a cache hit the only work is: acquire shared lock, one hash + pointer comparison, release lock.

### `insertion_order_` for rollback

`insertion_order_` is a `std::vector<string_view>` that mirrors registration order. `rollback()` pops from the back and
erases from `map_` until the target size is reached. This makes rollback O (d) in the number of removed entries.
`unregister()` also removes from `insertion_order_` via `std::erase`, which is O (n) — prefer `rollback()` for batch
removals.

### Duplicate-safe NamespaceIndex::insert

`insert()` checks `node_cache_` before creating a leaf node. Calling `insert()` with the same `entry->name` twice is
idempotent: the second call returns without adding a second leaf, preventing `enumerate()` from returning duplicate
results.
