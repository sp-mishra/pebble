# Kosha (`include/containers/cache/kosha.hpp`)

**Kosha** is Pebble's header-only, single-file, zero-virtual-dispatch C++23 cache library. It delivers
hardware-optimized local caching, zero-cost policy composition, Robin-Hood open-addressing storage, lock-striped
sharding, TTL eviction, and a zero-overhead distributed cluster skeleton.

---

## 1. Architectural Architecture & Namespace Hierarchy

```
                                  KOSHA ARCHITECTURAL TOPOLOGY
                                  
  ┌────────────────────────────────────────────────────────────────────────────────────────┐
  │                           kosha::cluster::ClusterCache                                 │
  │     (Router ── Transport ── Serializer ── Replication ── Consistency ── Membership)     │
  └──────────────────────────────────────────┬─────────────────────────────────────────────┘
                                             │
  ┌──────────────────────────────────────────┴─────────────────────────────────────────────┐
  │                              kosha::adapter Layers                                     │
  │     ┌────────────────────────┐  ┌────────────────────────┐  ┌────────────────────────┐ │
  │     │   ThreadSafeCache      │  │     ShardedCache<N>    │  │       TTLCache         │ │
  │     │ (Shared-Mutex Locking) │  │  (False-Sharing Free)  │  │    (Lazy Expiry)       │ │
  │     └────────────────────────┘  └────────────────────────┘  └────────────────────────┘ │
  └──────────────────────────────────────────┬─────────────────────────────────────────────┘
                                             │
  ┌──────────────────────────────────────────┴─────────────────────────────────────────────┐
  │                         kosha::core::Cache<K, V, Policy, Storage>                      │
  │                                                                                        │
  │   Eviction Policies (Compile-Time Traits):     Storage Backends (PMR Arena Aware):     │
  │   - LRUPolicy  (O(1) Recency List)             - FlatHashStorage (Robin-Hood Table)    │
  │   - LFUPolicy  (O(1) Frequency Bucket List)    - NodeStorage     (std::pmr Node Map)   │
  │   - FIFOPolicy (mutates_on_hit = false)                                                │
  │   - ARCPolicy  (Adaptive Replacement Cache)                                            │
  └────────────────────────────────────────────────────────────────────────────────────────┘
```

---

## 2. Eviction Policies & Algorithmic Complexity

| Policy           | `get(k)` Cost | `put(k, v)` Cost |             Memory Overhead             | `mutates_on_hit` | Use Case                                                               |
|:-----------------|:-------------:|:----------------:|:---------------------------------------:|:----------------:|:-----------------------------------------------------------------------|
| **`LRUPolicy`**  |    $O(1)$     |      $O(1)$      |  2 pointers per entry (intrusive list)  |      `true`      | Temporal locality; general-purpose workloads                           |
| **`LFUPolicy`**  |    $O(1)$     |      $O(1)$      | Frequency bucket node + 2 list pointers |      `true`      | Frequency-skewed workloads; static hot keys                            |
| **`FIFOPolicy`** |    $O(1)$     |      $O(1)$      |         1 circular queue index          |     `false`      | Read-dominated workloads (safe under read lock without write mutation) |
| **`ARCPolicy`**  |    $O(1)$     |      $O(1)$      |     4 lists ($T_1, T_2, B_1, B_2$)      |      `true`      | Self-tuning workloads balancing recency and frequency                  |

### 2.1 Adaptive Replacement Cache (ARC)

ARC dynamically tunes the boundary parameter $p \in [0, c]$ between two cache lists:

- **$T_1$**: Recency list (items seen once recently).
- **$T_2$**: Frequency list (items seen at least twice).
- **$B_1, B_2$**: Ghost history lists tracking keys recently evicted from $T_1$ and $T_2$.
- When a hit occurs in ghost list $B_1$, $p$ increases ($p \leftarrow \min (p + \delta_1, c)$), allocating more space to
  recency.
- When a hit occurs in ghost list $B_2$, $p$ decreases ($p \leftarrow \max (p - \delta_2, 0)$), allocating more space to
  frequency.

---

## 3. Storage Backends

### 3.1 `FlatHashStorage<K, V>`

- **Layout**: Contiguous flat array with Robin-Hood backward-shift open addressing.
- **Probe Sequence**: Triangular / linear probing with distance-to-initial-bucket tracking.
- **Cache Locality**: 100% flat memory contiguous array, eliminating pointer indirection and maximizing CPU L1 data
  cache lines.

### 3.2 `NodeStorage<K, V>`

- **Layout**: Pointer-stable nodes backed by `std::pmr::polymorphic_allocator`.
- **Arena Compatibility**: Can be bound directly to `pebble::mem::Smriti` arenas or
  `std::pmr::monotonic_buffer_resource` for zero heap fragmentation.

---

## 4. End-to-End API Guide

### 4.1 Basic LRU Cache

```cpp
#include "containers/cache/kosha.hpp"
#include <iostream>
#include <string>

int main() {
    // Fixed capacity: 128 elements
    kosha::LRUCache<std::string, int> cache{128};

    cache.put("user_1001", 42);
    cache.put("user_1002", 99);

    if (auto val = cache.get("user_1001")) {
        std::cout << "Hit: " << *val << "\n";
    } else {
        std::cout << "Miss\n";
    }

    cache.erase("user_1002");
    std::cout << "Size: " << cache.size() << "\n";
}
```

### 4.2 Thread-Safe Sharded Cache (Lock Striping)

```cpp
#include "containers/cache/kosha.hpp"

// 16 hash-striped shards preventing thread contention across CPU cores
kosha::ShardedLRUCache<std::string, std::vector<float>, 16> sharded_cache{1024};

void worker_thread(const std::string& key, const std::vector<float>& data) {
    sharded_cache.put(key, data);
    if (auto res = sharded_cache.get(key)) {
        // Safe concurrent access
    }
}
```

### 4.3 Time-To-Live (TTL) Cache with Lazy Eviction

```cpp
#include "containers/cache/kosha.hpp"
#include <chrono>

using namespace std::chrono_literals;

kosha::TTLLRUCache<int, std::string> session_cache{512};

void create_session(int user_id, const std::string& token) {
    // Valid for 10 seconds
    session_cache.put(user_id, token, 10s);
}

void authenticate(int user_id) {
    if (auto token = session_cache.get(user_id)) {
        // Valid session
    } else {
        // Expired or missing
    }
}
```

### 4.4 Instrumented Metrics Cache

```cpp
#include "containers/cache/kosha.hpp"

kosha::InstrumentedLRUCache<uint64_t, std::string> metrics_cache{256};

metrics_cache.put(1, "alpha");
metrics_cache.get(1);
metrics_cache.get(2); // Miss

std::cout << "Hits: " << metrics_cache.hits() << "\n";
std::cout << "Misses: " << metrics_cache.misses() << "\n";
std::cout << "Hit Rate: " << (metrics_cache.hit_rate() * 100.0f) << "%\n";
```
