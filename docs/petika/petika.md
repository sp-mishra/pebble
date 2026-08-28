# Petika: Unified Engine-Agnostic Key-Value & Storage Engine Platform

**Petika** (`include/petika/`) is Pebble's high-performance, crash-consistent, engine-agnostic storage and key-value platform in modern C++23. It decouples high-level application transactional storage APIs from low-level storage engines (SkipList, B+Tree, LSM-Tree, In-Memory Hash), backed by lock-free persistence workers and write-ahead logging (Nitya).

---

## 1. Architectural Architecture & Engine Decoupling

```
                                PETIKA STORAGE ARCHITECTURE
                                
  ┌────────────────────────────────────────────────────────────────────────────────────────┐
  │                                    APPLICATION LAYER                                   │
  │     put(), get(), erase(), contains(), scan(), range_query(), txn(), snapshot()        │
  └──────────────────────────────────────────┬─────────────────────────────────────────────┘
                                             │
  ┌──────────────────────────────────────────┴─────────────────────────────────────────────┐
  │                                 petika::Store<K, V, Engine>                            │
  │                                                                                        │
  │  Storage Engines (Zero-Virtual Concepts):    Common Infrastructure Substrate:          │
  │  - JournaledSkipEngine (O(log N) SkipList)   - petika::AsyncPersistenceWorker (SPSC)   │
  │  - MVCCJournaledSkipEngine (SI Isolation)    - nitya::Nitya (Durable WAL & Segmented)  │
  │  - BTreeEngine (B+Tree Block Store)          - utils::setu (Zero-Copy MMap Windows)    │
  │  - LSMEngine (SSTable MemTable / Compaction) - mem::smriti (Arena Allocation Pools)    │
  │                                              - observability::nadi (Pulse Telemetry)   │
  └────────────────────────────────────────────────────────────────────────────────────────┘
```

---

## 2. Core Subsystems & Algorithmic Guarantees

### 2.1 Multi-Version Concurrency Control (MVCC) Engine
`petika::engines::MVCCJournaledSkipEngine` implements snapshot isolation:
- **Lock-Free Read Visibility**: Readers acquire a read timestamp $T_{\text{read}}$ and read the latest version $V \le T_{\text{read}}$ without acquiring locks or blocking concurrent writers.
- **Write-Conflict Detection**: Optimistic commit validation aborts conflicting transactions attempting to modify the same key at timestamp $T_{\text{write}} > T_{\text{read}}$.
- **Garbage Collection (GC)**: Old versions behind the global minimum active snapshot are reclaimed by background epoch reclamation.

### 2.2 Lock-Free Background Persistence (`petika::AsyncPersistenceWorker`)
- **Header**: `#include <petika/async_persistence_worker.hpp>`
- **Architecture**: Single-Producer Single-Consumer (SPSC) lock-free ring buffer offloading synchronous disk serialization and Glaze JSON encoding from the main simulation thread.
- **Throughput**: Zero main-thread blocking latency; handles bursts of $>100,000\,\text{ops/sec}$ with background asynchronous OS `fsync` calls.

---

## 3. End-to-End API Guide

### 3.1 Synchronous Key-Value Store with Write-Ahead Logging
```cpp
#include "petika/petika.hpp"
#include <iostream>

int main() {
    petika::PetikaOptions options{
        .db_dir = "./galaxy_db",
        .segment_size = 32 * 1024 * 1024, // 32MB WAL segments
        .sync_on_write = false             // Group commit via background flusher
    };

    // Instantiate high-performance Journaled SkipStore
    petika::SkipStore<std::string, std::string> store(options);

    // 1. Put
    store.put("sector_0_0", "{\"mass\": 5000, \"bodies\": 1200}");
    store.put("sector_0_1", "{\"mass\": 3400, \"bodies\": 850}");

    // 2. Get
    if (auto res = store.get("sector_0_0")) {
        std::cout << "Read: " << *res << "\n";
    }

    // 3. Scan Prefix / Range
    store.scan("sector_", [](std::string_view k, std::string_view v) {
        std::cout << "Key: " << k << " -> Value: " << v << "\n";
        return true; // continue scan
    });

    // 4. Erase
    store.erase("sector_0_1");
}
```

### 3.2 MVCC Snapshot Isolation Transactions
```cpp
#include "petika/petika.hpp"

petika::MVCCSkipStore<uint64_t, float> balances(options);

void transfer(uint64_t from, uint64_t to, float amount) {
    auto txn = balances.begin_transaction();
    
    float from_bal = txn.get(from).value_or(0.0f);
    float to_bal = txn.get(to).value_or(0.0f);

    if (from_bal >= amount) {
        txn.put(from, from_bal - amount);
        txn.put(to, to_bal + amount);
        if (txn.commit()) {
            std::cout << "Transfer committed successfully!\n";
            return;
        }
    }
    txn.rollback();
    std::cout << "Transfer failed or conflicted.\n";
}
```

### 3.3 Asynchronous Non-Blocking Persistence Worker
```cpp
#include "petika/async_persistence_worker.hpp"
#include <glaze/glaze.hpp>

struct SectorPayload {
    int64_t sector_x = 0;
    int64_t sector_y = 0;
    std::vector<float> masses;
};

int main() {
    petika::AsyncPersistenceWorker worker(
        "./cold_storage",
        [](const std::string& path, const std::string& json_data) {
            // Background write execution
            std::ofstream out(path, std::ios::binary);
            out << json_data;
        },
        512 // Ring buffer capacity
    );

    SectorPayload sec{.sector_x = 10, .sector_y = -5, .masses = {10.0f, 25.0f, 40.0f}};
    std::string serialized = glz::write_json(sec).value_or("{}");

    // Zero-latency enqueue from main simulation thread
    worker.enqueue("./cold_storage/sec_10_-5.json", std::move(serialized));

    // Clean drain and shutdown
    worker.stop();
}
```

---

## 4. Infrastructure Summary

| Subsystem      | Role                      | Petika Integration                                                          |
|----------------|---------------------------|-----------------------------------------------------------------------------|
| **Nitya**      | Durability Backbone       | Write-Ahead Log (WAL), LSN offsets, recovery replay, replication streams    |
| **Setu**       | Persistent Memory Mapping | Safe memory-mapped file segments (`mapping<read_write>`), zero-copy buffers |
| **Smriti**     | Memory Resource System    | Fast linear arena (`LinearArena`) and pools for node allocation             |
| **Containers** | Data Structures & Caching | `kosha::adapter::PetikaAdapter` for durable cache backends                  |
| **NADI**       | Telemetry & Observability | Pulse scopes for `trace_publish()`, `trace_flush()`, `trace_recovery()`     |
---

## 4. Default Production Engine: `MvccJournaledSkipEngine`

`petika::StringSkipStore` and `petika::SkipStore` select
`petika::MvccJournaledSkipEngine`, which composes Nitya durability with
Anukrama version chains:

- **Skip List Index**: $O(\log n)$ point lookups, $O(\log n + k)$ ordered range scans without page-split overhead.
- **Stable snapshots**: each snapshot captures the last published Nitya LSN and reads the latest Anukrama version at or before that boundary.
- **Optimistic writes**: a transaction captures an LSN boundary and rejects a same-key write changed after that boundary before appending its WAL batch.
- **Log authority**: the Nitya WAL is the durable source of truth; recovery replays batches in LSN order.

`JournaledSkipEngine` remains available via `SingleVersionSkipStore` when an
application deliberately chooses single-version, last-writer-wins semantics.

---

## 5. Usage Examples

### Basic CRUD & Range Scans

```cpp
#include "petika/petika.hpp"

petika::PetikaOptions opts{
    .db_dir = "/tmp/petika_store",
    .sync_on_write = true,
    .auto_recovery = true
};

petika::StringSkipStore store{opts};

// Write
store.put("user:100", "Alice");
store.put("user:200", "Bob");

// Read
auto user = store.get("user:100");
if (user) {
    std::cout << "User: " << *user << "\n";
}

// Range Scan
store.scan("user:100", "user:300", [](const auto& entry) {
    std::cout << entry.key << " => " << entry.value << "\n";
});
```

### Transactions (Atomic Batching)

```cpp
auto tx = store.transaction();
tx.put("account:1", "100");
tx.put("account:2", "200");
if (!tx.commit()) {
    // A concurrent same-key writer advanced the Anukrama version chain.
    // Retry the transaction from a fresh snapshot.
}
```

### Kosha Cache Adapter Integration

```cpp
#include "petika/adapters/kosha.hpp"

using StrCache = kosha::LRUCache<std::string, std::string>;
kosha::adapter::PetikaAdapter<StrCache> cache{"/tmp/petika_cache", StrCache{1024}};

cache.put("session_id", "auth_token_xyz");
auto token = cache.get("session_id");
```
