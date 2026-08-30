# Anukrama (अनुक्रम) — Versioned-State & MVCC Substrate

**Anukrama** (`include/containers/anukrama/anukrama.hpp`) is Pebble's header-only, static-composition versioned-state substrate in modern C++23. It supplies immutable per-key version chains, stable point-in-time snapshots, and optimistic transaction validation with zero virtual dispatch and zero background thread overhead. Point reads are wait-free with respect to writers and allocation-free; concurrent readers share a lock (or take none under `null_lock`).

---

## 1. Architectural Architecture & Version Chains

```
                           ANUKRAMA MVCC VERSION TOPOLOGY
                           
   Key Index (SkipList / Map)
   ┌─────────────┬────────────────────────────────────────────────────────┐
   │ "user_1001" │ Head ──► [V3: val=85, ts=12] ──► [V2: val=60, ts=8] ──► [V1: val=20, ts=2] ──► NULL
   ├─────────────┼────────────────────────────────────────────────────────┤
   │ "user_1002" │ Head ──► [V2: TOMB, ts=15]   ──► [V1: val=100, ts=5]  ──► NULL
   └─────────────┴────────────────────────────────────────────────────────┘
                               ▲                         ▲
                               │                         │
                        Snapshot @ ts=10          Snapshot @ ts=4
                        (Reads V2 = 60)           (Reads V1 = 20)
```

---

## 2. Core Mechanics & Algorithmic Guarantees

### 2.1 Snapshot Isolation & Monotonic Clocks
- **Snapshot Creation**: Grabs the current monotonic clock timestamp $T_{\text{snap}} = \text{clock.now()}$ in $O(1)$.
- **Read Path**: Point read `snapshot.get(key)` traverses the singly-linked version chain from head to find the newest node with timestamp $T_{\text{node}} \le T_{\text{snap}}$.
- **Reads**: Pure pointer traversals with no allocation. Under the default `global_shared_lock` readers hold a shared lock and never block each other; only a committing writer excludes them. `null_lock` drops the lock entirely for single-threaded tiers.

### 2.2 First-Writer-Wins Optimistic Validation
When a transaction commits:
1. Validates that no key in its write set has been updated at a timestamp $T_{\text{written}} > T_{\text{txn\_snapshot}}$.
2. If a conflict is detected, commit fails immediately with `anukrama::error::conflict`.
3. If valid, atomically publishes new immutable version nodes at commit timestamp $T_{\text{commit}} = \text{clock.next()}$.

---

## 3. End-to-End API Guide

### 3.1 Basic Snapshots & Version Visibility
```cpp
#include "containers/anukrama/anukrama.hpp"
#include <cassert>
#include <iostream>
#include <string>

int main() {
    anukrama::store<std::string, int> store;

    // 1. Transaction 1 writes "visits" = 100
    store.begin().put("visits", 100).commit();

    // 2. Capture Point-in-Time Snapshot
    auto snap1 = store.snapshot_at_current();

    // 3. Transaction 2 writes "visits" = 250
    store.begin().put("visits", 250).commit();

    // Snapshot 1 retains immutable historical state
    assert(snap1.get("visits").value() == 100);

    // Latest store state reflects Transaction 2
    auto snap2 = store.snapshot_at_current();
    assert(snap2.get("visits").value() == 250);

    std::cout << "Snap1: " << *snap1.get("visits") << " | Snap2: " << *snap2.get("visits") << "\n";
}
```

### 3.2 Optimistic Transaction Validation & Conflict Handling
```cpp
#include "containers/anukrama/anukrama.hpp"
#include <iostream>

void demonstrate_conflict() {
    anukrama::store<std::string, float> accounts;
    accounts.begin().put("acc_A", 1000.0f).commit();

    // Begin concurrent transactions T1 and T2 at same snapshot
    auto t1 = accounts.begin();
    auto t2 = accounts.begin();

    // T1 modifies acc_A and commits
    t1.put("acc_A", 900.0f);
    auto res1 = t1.commit();
    std::cout << "T1 Commit: " << (res1.has_value() ? "SUCCESS" : "FAILED") << "\n";

    // T2 tries to modify acc_A based on stale snapshot -> Conflict!
    t2.put("acc_A", 850.0f);
    auto res2 = t2.commit();
    if (!res2) {
        std::cout << "T2 Commit: CONFLICT DETECTED (First-writer-wins validated!)\n";
    }
}
```

### 3.3 Explicit History Pruning
```cpp
// Reclaim history no longer visible to any live snapshot. The retention boundary
// is the oldest active snapshot (or clock.now() if none), taken from the registry.
store.prune();
```

---

## 4. Policy Reference

`store` is fully static-composition. Every parameter has a default equal to today's
behaviour, so `anukrama::store<Key, Value>` pays for nothing it does not use.

```cpp
template <class Key, class Value, class Compare = std::less<>,
          template <class,class,class> class IndexPolicy = skip_list_index,
          externally_advanceable_clock Clock = atomic_clock,
          conflict_policy ConflictPolicy = snapshot_isolation,
          template <class> class NodeAllocatorPolicy = heap_node_pool,
          store_sync SynchronizationPolicy = global_shared_lock,
          snapshot_registry SnapshotRegistryPolicy = multiset_snapshot_registry>
class store;
```

| Parameter | Default | Purpose / Alternatives |
|---|---|---|
| `Key`, `Value` | — | Copy-constructible key and value types. |
| `Compare` | `std::less<>` | Ordering over keys; `[[no_unique_address]]`. |
| `IndexPolicy` | `skip_list_index` | Ordered key→version-chain map. Must model `ordered_version_index` (find / `lower_bound` / begin / end / `insert_or_assign` / `erase`, `value_type` with `.first`/`.second`). |
| `Clock` | `atomic_clock` | Commit clock; must model `externally_advanceable_clock`. Exposes `timestamp_type` (default `uint64_t`) — the seam for a future 128-bit HLC. |
| `ConflictPolicy` | `snapshot_isolation` | `optimistic_point_serializable` additionally validates the read-set at commit. |
| `NodeAllocatorPolicy` | `heap_node_pool` | Version-node lifetime. `heap_node_pool` = `new`/`delete` (identical to prior behaviour). `smriti_node_pool<Domain>` draws fixed-size slabs from a Smriti `FixedPool` with a free-list, so `prune()` recycles rather than frees — **recommended in production**. |
| `SynchronizationPolicy` | `global_shared_lock` | Locking granularity. `global_shared_lock` = one `std::shared_mutex`. `striped_lock<N>` = N-way key-hash striping so disjoint-key commits proceed concurrently. `null_lock` = zero-overhead single-thread. |
| `SnapshotRegistryPolicy` | `multiset_snapshot_registry` | Active-snapshot bookkeeping for GC. Default `std::multiset<timestamp>` gives O(log k) insert/erase and O(1) minimum; swap in an epoch reclaimer without touching store internals. |

## 5. Concurrency Model

- **Readers** hold a shared lock under `global_shared_lock`, so any number read concurrently
  and only a committing writer excludes them. `null_lock` takes no lock at all.
- **Writers** serialise against readers and each other under `global_shared_lock`. With
  `striped_lock<N>`, a commit locks only the stripes its write-set keys hash to — acquired in
  ascending stripe order so lock ordering is total and deadlock-free — letting disjoint-key
  transactions commit in parallel.
- **Snapshot creation** never takes the store lock: it captures `clock.now()` and inserts into
  the registry under a dedicated registry mutex, so opening a snapshot does not block readers.
- **`size()`** is O(1), backed by an atomic live-key counter maintained on publish.
- **Durability binding**: `apply_at(writes, lsn)` installs a prepared batch at an externally
  owned commit timestamp — bind a Nitya WAL record's commit LSN directly as the Anukrama
  commit clock for crash-consistent replay.
