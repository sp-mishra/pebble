# Smriti — Universal Memory Resource Framework

**Status:** Implemented. Header-only, C++23, no virtual, no macros, no RTTI.
**Layer:** Leaf substrate. Higher layers (Tarka `Context`, Vākya, Lithe codegen, Kosha) consume Smriti; Smriti has zero
upward dependency.
**Headers:** `include/mem/smriti.hpp` (core), `include/mem/arena.hpp` (arena pools), `include/mem/buddy.hpp` (buddy
allocator), `include/mem/mmap_domain.hpp` (mmap & NUMA domains).

Smriti is a compositional allocator kernel. Instead of one monolithic allocator it exposes a **layered algebra of
orthogonal concerns** — *where* bytes come from (Domain), *how* they are carved (Pool), *what invariants* wrap each
request (Policy), *who owns* a live object (Handle), and *how residency is governed* over time (Manager). Each layer is
a concept, so any conforming type composes with any other at compile time with zero indirection. You pay only for the
layers you name.

---

## 1. Architecture

```
Domain          where raw bytes originate (heap / stack / mmap / NUMA / null)
  │             concept: acquire(n,a)->void*, release(p,n) noexcept, ::alignment
  ▼
Pool            how a byte range is carved into allocations
  │             concept: allocate(n,a)->void*, deallocate(p,n) noexcept, reset() noexcept
  │             Bump (O(1) LIFO), Fixed (free-list slab), Buddy (power-of-two split/merge),
  │             ScopedArena / LinearArena (checkpoint) / TwoPhaseArena (primary+overflow)
  ▼
Policy          decorator over a Pool — same concept in, same concept out
  │             Unsafe (pass-through), ThreadSafe (mutex), LockFree (atomic bump),
  │             BoundsCheck (canary+header), Audit (leak ledger)
  ▼
Handle          RAII ownership of one live object
  │             OwnerHandle<T> (deleter-typed), PinnedPageHandle<T> (generational, resolve→expected)
  ▼
Manager         residency governor over a PageTable (opt-in; NullManager folds to nothing)
  │             LRUCacheManager, AsyncMigrationManager, RecoveryManager
  ▼
ManagedResource<Domain,Pool,Manager=NullManager>
                the assembled façade: make<T>()/destroy(), allocate()/deallocate(),
                eviction hook, PageTable ownership
  │
  ▼
SmritiAllocator<T,Resource>   std::-conforming adaptor (rebind, allocator_traits)
```

**Zero-overhead invariant.** `ManagedResource<Domain, Pool>` with the default `NullManager` compiles to a plain pool
call plus a generational page-table insert; the manager's five hooks are empty and inline away. `NullManager`,
`UnsafePolicy`, and the `NullDomain` are the identity elements of their respective layers. You are charged only for what
you compose.

**Design forces.** Every layer is a *concept*, not a base class — no vtable, no `dynamic_cast`. Composition is by
template parameter, so the full type of a resource (e.g.
`ManagedResource<SystemRAMDomain, ThreadSafePolicy<BumpPool<SystemRAMDomain>>, LRUCacheManager>`) is known at compile
time and fully inlinable. Fallible operations return `std::expected` / `std::optional`; no exceptions on the hot path (
the sole `throw` is `SmritiAllocator::allocate`, mandated by the standard allocator contract).

---

## 2. Core utilities (`detail`)

- **`align_up(n, a)`** — `(n + a - 1) & ~(a - 1)`; branch-free round-up, requires `a` power-of-two.
- **`cache_line = 64`**, **`padded<T>`** — `alignas(64)` wrapper that isolates a hot atomic onto its own cache line,
  eliminating false sharing on the lock-free bump path.
- **`GenId {uint32 id; uint32 gen}`** — a generational handle. `id` indexes a slot; `gen` disambiguates reuse.
  `valid()` ⇔ `id != 0`, so `null_genid = {0,0}` is the canonical empty handle. This is the ABA-defeating primitive
  underneath `PageTable`: a stale `GenId` (same `id`, old `gen`) resolves to an error instead of dangling.
- **`ref_counted<Derived>`** — CRTP intrusive refcount; `dec()` calls `Derived::destroy()` on the `1→0` edge (acq_rel).

---

## 3. Domains — byte sources

Concept `Domain<D>`: `static constexpr size_t alignment`, `acquire(n,a)->void*`, `release(p,n) noexcept`.
`DomainWithContext<D>` additionally exposes `context_type` + `context()` for domains carrying mutable OS state (fd, mmap
base).

| Domain                 | Backing                                    | Alignment     | Semantics                                                                                                                                   |
|------------------------|--------------------------------------------|---------------|---------------------------------------------------------------------------------------------------------------------------------------------|
| `SystemRAMDomain`      | `::operator new/delete` (aligned, nothrow) | `max_align_t` | General heap.                                                                                                                               |
| `StackDomain<N,Align>` | inline `byte buf[N]`                       | `Align`       | Bump within a stack buffer; non-movable; `release` is a no-op.                                                                              |
| `NullDomain`           | —                                          | 1             | Always returns `nullptr`. Identity domain for testing/OOM paths.                                                                            |
| `MappedFileDomain`     | `mmap` (`MAP_ANON` or `MAP_SHARED`)        | 4096          | File- or anon-backed; bump cursor within the mapped region; `msync` on release, `munmap` on destruction. `DomainWithContext`.               |
| `NumaDomain`           | `mmap` page-aligned (macOS/Linux)          | `max_align_t` | Memory-tier aware; degrades to `operator new` if `mmap` fails, and aliases `SystemRAMDomain` on unsupported platforms. `DomainWithContext`. |

`MappedFileDomain` is constructed through named factories, not a public ctor:

```cpp
auto anon = domains::MappedFileDomain::anonymous(64 * 1024);
auto file = domains::MappedFileDomain::from_file("/tmp/store.bin", size, /*writeable=*/true);
if (!anon.valid()) { /* mmap failed */ }
```

`MappedRegion` is a **non-owning** `std::span<std::byte>` view into a sub-range with a `flush()` (`msync`, sync or
async). The owning domain must outlive every region carved from it.

---

## 4. Pools — carving strategies

Concept `Pool<P>`: `allocate(n,a)->void*`, `deallocate(p,n) noexcept`, `reset() noexcept`. `BulkPool<P>` adds
`used_bytes()`.

### 4.1 BumpPool `<Domain>`

Monotonic cursor over one domain-acquired slab. **Lock-free** allocate via CAS on a cache-line-padded `atomic<size_t>`
offset:

```
allocate(n,a):
  old ← offset.load(relaxed)
  loop:
    aligned ← align_up(old, a)
    if aligned + n > capacity: return nullptr        // OOM, no mutation
    if offset.CAS(old → aligned + n, acq_rel): return base + aligned
    // CAS failed: old refreshed with current value, retry
```

`deallocate` is a no-op; reclamation is all-or-nothing via `reset()`. Exposes `atomic_offset()` so `LinearArena` can
implement checkpoint/rollback. O(1) amortized, wait-free-ish under low contention.

### 4.2 FixedPool `<BlockSize, Domain>` (`BlockSize ≥ sizeof(void*)`)

Segregated free-list of fixed blocks over grow-on-demand slabs. Free list is intrusive (next-pointer stored in the free
block itself) and lock-free on the fast path; slab growth takes a mutex with a double-checked re-probe to avoid a
spurious grow after a concurrent free:

```
allocate:
  fast: pop head off lock-free free list (CAS)         → hit: return block
  slow: lock; re-probe free list (a concurrent free may have refilled it)
        still empty → grow() one slab, thread its blocks onto the free list
        pop and return
deallocate(p):
  CAS-push p onto free-list head (store old head into *p first)
```

### 4.3 Arenas (`arena.hpp`)

- **`ScopedArena<N,Align>`** — stack-buffer bump, zero heap, non-movable. The lightest possible allocator; ideal for
  per-call scratch.
- **`LinearArena`** — heap-backed `BumpPool<SystemRAMDomain>` + `checkpoint()`/`rollback(cp)`. A checkpoint is just the
  current offset; rollback stores it back into the atomic cursor. **Rollback is safe only with no concurrent allocations
  in flight** — it is a single-writer scoped-scratch primitive, not a concurrent free.
- **`TwoPhaseArena`** — primary arena with a lazily-consulted overflow arena; `allocate` spills to overflow only when
  primary is exhausted. Two-tier capacity without over-provisioning the hot region.

### 4.4 BuddyPool `<MinOrder=5, MaxOrder=20, Domain>` (`buddy.hpp`)

Classic binary buddy allocator over a single `2^MaxOrder`-byte slab. `kLevels = MaxOrder − MinOrder + 1` free lists; a
per-level bitmap marks free blocks. Sizes round up to the next power-of-two ≥ `2^MinOrder`.

```
level(n)   = bit_width(roundup_pow2(max(n, MinBlock))) − 1 − MinOrder

allocate(n):
  L ← level(n);  scan free lists L..top for the lowest non-empty level F
  pop block from F
  while F > L:                       // split down, pushing each upper half free
    F ← F − 1;  push (block + 2^(MinOrder+F)) onto free_list[F]
  return block

buddy(p, L) = base + ((p − base) XOR (MinBlock << L))   // sibling by one XOR

deallocate(p, n):
  L ← level(n)
  while L < top and bitmap.test(buddy(p,L)):   // sibling free → coalesce
    fl_remove(buddy(p,L), L);  p ← min(p, buddy(p,L));  ++L
  push p onto free_list[L]
```

The XOR-buddy identity gives O(1) sibling lookup; coalescing is O(levels). External fragmentation is bounded by the
power-of-two rounding; internal fragmentation ≤ 2× worst case.

---

## 5. Policies — pool decorators

A Policy wraps a `Pool` and *is itself* a `Pool` (`inner_pool_type` exposes the wrapped type; `PolicyWrapper` concept
detects this). Policies stack.

| Policy              | Adds                                                                         | Cost when unused                      |
|---------------------|------------------------------------------------------------------------------|---------------------------------------|
| `UnsafePolicy`      | nothing (identity)                                                           | zero                                  |
| `ThreadSafePolicy`  | `std::mutex` around every op                                                 | — (opt-in)                            |
| `LockFreePolicy`    | constrained to pools exposing `atomic_offset()`                              | zero (pass-through; documents intent) |
| `BoundsCheckPolicy` | head magic + tail canary + `source_location`; `std::terminate` on corruption | header + canary bytes per alloc       |
| `AuditPolicy`       | `flat_map<void*, {size, source_location}>` live-ledger + `report()` of leaks | ledger memory (debug builds)          |

`BoundsCheckPolicy` lays out `[AllocHeader | user bytes | Canary]`; `deallocate` verifies both magics (`0xDEADBEEF`,
`0xCAFEBABE`) and prints the capturing `source_location` on overrun before terminating — a zero-config heap-corruption
tripwire.

---

## 6. PageTable — generational residency map

`PageTable` is the residency substrate managers govern. It maps
`id → PageEntry {GenId, ptr, pin_count, PageState, dirty}` (values held via `unique_ptr` for pointer stability, since
`PageEntry` contains atomics and is non-movable). A `shared_mutex` gives concurrent readers (`resolve`, `pin`, `state`)
with exclusive writers (`alloc_page`, `free_page`).

**State machine** (`transition` enforces the edges; illegal edges return `false`):

```
Cold ──▶ Loading ──▶ Resident ──▶ Evicting ──▶ Cold
                        └──▶ Flushing ──▶ Evicting
```

Invariants:

- `Resident → Evicting` and `Resident → Flushing` **require `pin_count == 0`** — you cannot evict a pinned page.
- `pin` fails on `Cold`/`Evicting`/`Flushing` pages (nothing to pin).
- `resolve(id)` returns `std::expected<void*, PageError>`: `NullPage` (invalid id), `Stale` (generation mismatch — slot
  reused), `Evicted` (`Cold`/`Evicting`). This is the safety net: a dangling handle degrades to a typed error, never a
  wild pointer.
- `for_each_evictable(fn)` visits every unpinned `Resident` page — the manager's candidate scan.

---

## 7. Handles — RAII ownership

- **`OwnerHandle<T>`** — owns a `T*` plus a type-erased deleter (`function<void(void*,size_t)>`) and size. Dtor runs
  `~T()` then the deleter. Move-only; `release()` relinquishes without destroying. Use when the object lives in a pool
  and you want scope-bound cleanup.
- **`PinnedPageHandle<T>`** — owns a `GenId` + `PageTable*`; the page stays pinned for the handle's lifetime, dtor
  unpins. `get()` returns `std::expected<T*, PageError>` — access is **re-validated through the table** every call, so a
  page evicted out from under you surfaces as an error rather than a dangling deref. `release_ownership()` detaches
  without unpinning (used by `ManagedResource::destroy`).

---

## 8. Managers — residency governors

Concept `Manager<M>`: `attach(PageTable&)`, `on_alloc(GenId)`, `on_access(GenId)`, `evict_one()->optional<GenId>`,
`shutdown()` — all `noexcept`.

- **`NullManager`** — every hook empty; the zero-cost default. `ManagedResource` with it never spends a cycle on
  residency.
- **`LRUCacheManager {capacity}`** — intrusive doubly-linked recency list (`std::map` node storage for pointer stability
  of prev/next links). `on_access` splices to front; `on_alloc` over capacity triggers `evict_one`, which walks from the
  LRU tail, skipping pinned pages via a failed `Resident→Evicting` transition, then `Cold` + `free_page` on the first
  evictable victim. Mutex heap-allocated so the manager stays movable.
- **`AsyncMigrationManager<Src,Dst> {threshold}`** — hot-page tiering. Counts accesses; when a page crosses `threshold`,
  enqueues it on a lock-free ring; a `jthread` worker `pin`s, `memcpy`s `Src→Dst`, releases the source. Backpressure by
  dropping when the ring is full. `shutdown()` requests stop; the `jthread` auto-joins.
- **`RecoveryManager {interval}`** — periodic dirty-page flush. `mark_dirty` records the id; a `jthread` flushes on
  `interval` (and on `shutdown`). The flush is a state-transition scaffold today (real `msync`-per-page is the intended
  fill-in).

---

## 9. ManagedResource — the assembled façade

```cpp
template <Domain D, Pool P, Manager M = NullManager>
class ManagedResource;
```

Owns `{Domain, Pool, PageTable, Manager}` and wires `manager.attach(table)` at construction. API:

- **`make<T>(args...) -> PinnedPageHandle<T>`** — pool-allocate, placement-`new` the `T`, register a page, notify the
  manager, pin, and hand back a pinned handle. On any failure the object is destroyed and memory returned, yielding an
  empty handle.
- **`destroy(PinnedPageHandle<T>&&)`** — `~T()`, unpin, `Resident→Evicting→Cold`, `free_page`, `deallocate`, detach the
  handle. The explicit teardown path.
- **`allocate(n,a)`** — raw bytes; on pool OOM it asks the manager to `evict_one()` and retries once (the eviction
  feedback loop).
- **`deallocate`, `notify_access(id)`, `evict_one()`, `page_table()`, `manager()`**.

```cpp
using Domain = domains::SystemRAMDomain;
using Pool   = pools::BumpPool<Domain>;
ManagedResource<Domain, Pool> res{Domain{}, Pool{64 * 1024}};

auto h = res.make<Widget>(arg1, arg2);   // pinned handle
if (h) use(*h);                          // operator*/-> resolve through the table
res.destroy(std::move(h));               // or let the handle's dtor unpin
```

---

## 10. SmritiAllocator — std-conforming adaptor

`SmritiAllocator<T, Resource>` (any `MemoryResource` — a pool, an arena, or a `ManagedResource`) is a standard
allocator: `value_type`, `rebind<U>::other`, `rebind_alloc<U>`, `operator==` (identity by backing-resource pointer).
`allocate` throws `std::bad_alloc` on failure (standard contract); `deallocate` is `noexcept`.

```cpp
ManagedResource<domains::SystemRAMDomain,
                pools::BumpPool<domains::SystemRAMDomain>> res{{}, Pool{1 << 17}};
auto alloc = smriti::make_allocator<int>(res);          // deduces Resource
std::vector<int, decltype(alloc)> v{alloc};
std::list<int, decltype(alloc)>   l{alloc};             // rebinds to list_node<int>
std::map<int,int,std::less<>, decltype(alloc)> m{{}, alloc};
```

`make_allocator<T>(res)` is the ergonomic factory (deduces `Resource`). Pools and arenas satisfy `MemoryResource`
directly, so you can back a container with a bare `ScopedArena` or `FixedPool` without a full `ManagedResource`.

---

## 11. Selection guide

| Need                                 | Use                                                           |
|--------------------------------------|---------------------------------------------------------------|
| Per-call scratch, no heap            | `ScopedArena<N>`                                              |
| Fast bulk alloc, free-all-at-once    | `BumpPool` / `LinearArena`                                    |
| Scoped scratch with undo             | `LinearArena` + `checkpoint`/`rollback`                       |
| Uniform fixed-size objects, recycled | `FixedPool<BlockSize>`                                        |
| Mixed sizes with coalescing          | `BuddyPool<MinOrder, MaxOrder>`                               |
| Shared / persistent memory           | `MappedFileDomain` (+ any pool)                               |
| NUMA / tiered placement              | `NumaDomain`, `AsyncMigrationManager`                         |
| Cache with eviction                  | `ManagedResource<…, LRUCacheManager>`                         |
| Concurrent access                    | wrap pool in `ThreadSafePolicy` (or use lock-free `BumpPool`) |
| Debug heap-corruption / leaks        | `BoundsCheckPolicy` / `AuditPolicy`                           |
| Drop-in for STL containers           | `SmritiAllocator` / `make_allocator`                          |

---

## 12. Extending Smriti (plug-and-play)

Add a new layer by conforming to its concept — no edits to existing code:

- **New Domain:** provide `static constexpr size_t alignment`, `acquire`, `release`. It immediately composes under any
  pool.
- **New Pool:** provide `allocate`/`deallocate`/`reset`; add `used_bytes()` for `BulkPool`, `atomic_offset()` to unlock
  `LockFreePolicy`.
- **New Policy:** template on `Pool`, expose `inner_pool_type`, forward the three ops with your invariant wrapped around
  them.
- **New Manager:** implement the five `Manager` hooks over the `PageTable` state machine.

Because every layer is a concept satisfied structurally, a new type drops into `ManagedResource<…>` and
`SmritiAllocator<…>` with zero glue and zero runtime cost for the paths it does not touch.

---

## 13. Known limitations / future work

- `RecoveryManager::flush_all_dirty` is a state scaffold; real durability needs `msync`-per-page against the backing
  domain.
- `AsyncMigrationManager` migration tasks carry `size == 0` from `on_access`, so `migrate_page` (guarded by `size > 0`)
  is not yet exercised end-to-end — page size must be threaded from the table before migration is live.
- `BuddyPool::fl_remove` performs a linear free-list scan; fine for shallow lists but O(list length) on hot coalescing
  paths.
- `LinearArena::rollback` is single-writer only; it is not a concurrent free.
- `NumaDomain` is `mmap`-based placement on macOS; true `mbind`/NUMA-node binding on Linux is a drop-in extension point.

---

**See also:** `docs/reference.md` (library index), `docs/tarka/tarka.md` (Tarka `Context` reuses `LinearArena`),
`docs/containers/cache/` (Kosha, which pairs with `ManagedResource` for arena-backed caches).
