# Petika: A Unified, Engine-Agnostic Storage Platform for C++23

## 1. Overview

**Petika** (Hindi/Sanskrit: *box*, *container*, *repository*) is a high-performance, durable, embedded storage platform built with modern C++23.

### Core Philosophy
Applications should depend on a unified **Storage API**, not on an underlying storage architecture.

Whether an application uses a **SkipList**, **B+Tree**, **LSM-Tree**, or **In-Memory Hash**, the Petika API remains uniform and stable while execution engines evolve underneath:

```cpp
using Store = petika::SkipStore<std::string, std::string>;
// Or later:
// using Store = petika::BTreeStore<std::string, std::string>;
// using Store = petika::LSMStore<std::string, std::string>;
```

---

## 2. Layered Architecture

```
┌────────────────────────────────────────────────────────┐
│                    Application API                     │
│  put(), get(), erase(), contains(), scan(), txn(), snap()
└───────────────────────────┬────────────────────────────┘
                            │
                            ▼
┌────────────────────────────────────────────────────────┐
│                   Petika Storage Hub                   │
│   (Transactions, Snapshots, Manifest, Recovery)        │
└───────────────┬────────────────────────┬───────────────┘
                │                        │
                ▼                        ▼
      ┌──────────────────┐     ┌──────────────────┐
      │ Engine Concept   │     │ Infrastructure   │
      │ ├─ SkipEngine    │     │ ├─ Nitya (WAL)   │
      │ ├─ BTreeEngine   │     │ ├─ Setu (mmap)   │
      │ └─ LSMEngine     │     │ ├─ Smriti (mem)  │
      └──────────────────┘     │ ├─ NADI (telemetry)
                               │ └─ EasyRules     │
                               └──────────────────┘
```

---

## 3. Infrastructure Subsystems

| Subsystem | Role | Petika Integration |
|---|---|---|
| **Nitya** | Durability Backbone | Write-Ahead Log (WAL), LSN offsets, recovery replay, replication streams |
| **Setu** | Persistent Memory Mapping | Safe memory-mapped file segments (`mapping<read_write>`), zero-copy buffers |
| **Smriti** | Memory Resource System | Fast linear arena (`LinearArena`) and pools for node allocation |
| **Containers** | Data Structures & Caching | `kosha::adapter::PetikaAdapter` for durable cache backends |
| **NADI** | Telemetry & Observability | Pulse scopes for `trace_publish()`, `trace_flush()`, `trace_recovery()` |
| **EasyRules** | Operational Policies | Administrative rules for compaction, archival, and alerts |

---

## 4. Practical First Production Engine: `JournaledSkipEngine`

Petika introduces `petika::JournaledSkipEngine`:
- **Skip List Index**: $O(\log n)$ point lookups, $O(\log n + k)$ ordered range scans without page-split overhead.
- **Log Authority**: The Nitya WAL is the durable source of truth. The index is fully in-memory and deterministic to rebuild.
- **Smriti Memory**: Fast linear arena node allocation.

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
tx.commit();
```

### Kosha Cache Adapter Integration
```cpp
#include "petika/adapters/kosha.hpp"

using StrCache = kosha::LRUCache<std::string, std::string>;
kosha::adapter::PetikaAdapter<StrCache> cache{"/tmp/petika_cache", StrCache{1024}};

cache.put("session_id", "auth_token_xyz");
auto token = cache.get("session_id");
```
