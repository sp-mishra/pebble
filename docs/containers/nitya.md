# Nitya: A Generic Durable Log Engine (DLE)

## Overview

**Nitya** is a header-only, C++23+, policy-based Durable Log Engine designed to serve as a low-latency, zero-allocation substrate for:
- Database Write-Ahead Logs (PostgreSQL / MySQL style WALs)
- Event Stores and Command-Sourcing engines
- Raft & Consensus logs
- Change Data Capture (CDC) streams
- Distributed state machines and replication pipelines

Nitya strictly separates **ordering, durability, recovery, replication, segmentation, and retention** from transaction semantics, MVCC, index structures (B-Trees / LSMs), and business logic.

---

## Core Principles & Design

### 1. Byte Offset LSN
`lsn_t` is defined as:
```cpp
using lsn_t = uint64_t;
```
`lsn_t` represents the exact physical byte offset in the continuous logical log stream across segments.
- Direct O(1) segment and offset translation:
  - `segment_id = lsn / segment_size`
  - `segment_offset = lsn % segment_size`
- Fast recovery and replay without translation indexes.
- Transparent replication offsets and watermark tracking.

### 2. Reserve → Publish → Sync Pipeline
Appending follows a three-phase asynchronous pipeline:
1. **Reserve**: Acquire a slice in the log stream and allocate an LSN range.
   ```cpp
   auto slot = wal.reserve(payload_size);
   // returns reservation { lsn, buffer }
   ```
2. **Publish**: Writer encodes its record directly into `slot.buffer` and marks the reservation ready.
   ```cpp
   wal.publish(slot);
   ```
3. **Sync / Group Commit**: Flusher thread collects published ranges, commits mapped segments to durable storage using `Setu` (`msync`), and advances the durability watermark.
   ```cpp
   lsn_t durable_lsn = wal.sync();
   ```

### 3. Zero-Allocation Hot Path
- Memory buffers and encoders leverage **Smriti** bump pools / linear arenas.
- Lock-free wait-free / low-spin concurrency via **Containers** MPMC queues.
- Compile-time zero-overhead observability via **NADI** scopes.
- Operational automation (retention, archival) via **EasyRules**.
- No virtual dispatch, no RTTI, no heap allocation on the critical append path.

---

## Binary Frame Layout

Each entry in the log segment is framed as follows:

```
┌─────────────────────────────────────────────────────────────┐
│                       frame_header                          │
│  - magic:        uint32_t (0x4E495459 "NITY")               │
│  - size:         uint32_t (payload size in bytes)           │
│  - lsn:          uint64_t (physical byte offset)            │
│  - header_crc:   uint32_t (CRC/FNV1a-32 of header fields)   │
│  - payload_crc:  uint32_t (CRC/FNV1a-32 of payload bytes)   │
├─────────────────────────────────────────────────────────────┤
│                          Payload                            │
│  - span<const std::byte> (opaque payload of length `size`)  │
├─────────────────────────────────────────────────────────────┤
│                       frame_trailer                         │
│  - size:         uint32_t (payload size in bytes)           │
│  - payload_crc:  uint32_t (CRC/FNV1a-32 of payload bytes)   │
└─────────────────────────────────────────────────────────────┘
```

The trailer enables backward scan during recovery and sanity check validation.

---

## Policy Architecture

`nitya::wal` is parameterized by modular policies:

```cpp
template <
    typename StoragePolicy      = nitya::setu_storage,
    typename MemoryPolicy       = nitya::smriti_memory,
    typename ConcurrencyPolicy  = nitya::group_commit_concurrency<1024>,
    typename FramingPolicy      = nitya::default_framing,
    typename DurabilityPolicy   = nitya::sync_durability,
    typename TelemetryPolicy    = nitya::nadi_telemetry
>
class wal;
```

### Policy Roles
1. **StoragePolicy (`setu_storage`)**:
   - Manages memory-mapped segment files via Setu (`mapping<read_write>`).
   - Handles file creation, expansion, unmapping, sequential remapping, and `flush()`.
2. **MemoryPolicy (`smriti_memory`)**:
   - Manages scratch buffers and arenas using Smriti (`LinearArena` / `BumpPool`).
3. **ConcurrencyPolicy (`group_commit_concurrency`)**:
   - Uses lockfree `MPMCQueue` for atomic reservation, publication ticketing, and group commit dispatch.
4. **FramingPolicy (`default_framing`)**:
   - Computes header and trailer checksums, validates frame headers and trailers during recovery and replay.
5. **DurabilityPolicy (`sync_durability` / `async_durability`)**:
   - Dictates whether flush operations issue synchronous `msync(MS_SYNC)` or background flushes.
6. **TelemetryPolicy (`nadi_telemetry`)**:
   - Integrates with NADI pulse scopes for latency and throughput metrics (`wal_reserve`, `wal_publish`, `wal_flush`, `recovery_scan`, `replication_send`).

---

## Retention & Archival Policies with EasyRules

Nitya integrates **EasyRules** for administrative rule-driven maintenance:
- **Retention**: Segments older than a defined threshold (or already replicated) can be marked for pruning.
- **Archival**: Automatically archive segments when `replicated == true` and `segment_age > threshold`.
- **Alerting**: Alert when flush latency or replication lag exceeds thresholds.

---

## Usage Examples

### 1. Basic Append and Durability
```cpp
#include "nitya/nitya.hpp"

namespace fs = std::filesystem;

nitya::wal_options opts{
    .wal_dir = "/tmp/nitya_wal",
    .segment_size = 4 * 1024 * 1024 // 4 MB
};

nitya::wal<> log{opts};

// Write a record
std::string msg = "TRANSACTION_COMMIT_TX100";
auto res = log.append(std::span{reinterpret_cast<const std::byte*>(msg.data()), msg.size()});

if (res) {
    std::cout << "Appended record at LSN: " << *res << "\n";
}

// Ensure all reservations are flushed to disk
log.sync();
```

### 2. Recovery Scanning
```cpp
auto recovery = log.recover();
for (const auto& record : recovery) {
    std::string_view text{reinterpret_cast<const char*>(record.payload.data()), record.payload.size()};
    std::cout << "Replaying LSN " << record.lsn << ": " << text << "\n";
}
```

### 3. Replication Streaming
```cpp
// Subscribe from a given LSN (e.g. 0)
auto stream = log.subscribe(0);
while (auto frame = stream.next()) {
    // Send frame to remote replica or CDC subscriber
    send_to_replica(*frame);
}
```

### 4. Kosha Caching Adapter (`kosha::adapter::NityaAdapter`)
```cpp
#include "containers/cache/kosha.hpp"
#include "containers/cache/adapters/nitya.hpp"

using StrCache = kosha::LRUCache<std::string, std::string>;
kosha::adapter::NityaAdapter<StrCache> cache{"/tmp/nitya_cache_wal", StrCache{1024}};

// Write-through: appends to Nitya WAL and commits to in-memory LRU
cache.put("user:101", "Alice");

// Read-through: serves from in-memory cache or recovers from Nitya WAL
auto user = cache.get("user:101");

// Warm cache on startup
cache.load_all();
```
