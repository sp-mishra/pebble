# Nitya: A Generic Durable Log Engine (DLE)

## Overview

**Nitya** is a header-only, C++23+, policy-based Durable Log Engine designed to serve as a low-latency, zero-allocation
substrate for:

- Database Write-Ahead Logs (PostgreSQL / MySQL style WALs)
- Event Stores and Command-Sourcing engines
- Raft & Consensus logs
- Change Data Capture (CDC) streams
- Distributed state machines and replication pipelines

Nitya strictly separates **ordering, durability, recovery, replication, segmentation, and retention** from transaction
semantics, MVCC, index structures (B-Trees / LSMs), and business logic.

---

## Core Principles & Design

### 1. Watermarks & Byte Offset LSN

`lsn_t` is defined as:

```cpp
using lsn_t = uint64_t;
```

`lsn_t` represents the exact physical byte offset in the continuous logical log stream across segments:

- **`tail_lsn`**: Maximum byte offset reserved so far by writers (starts at `sizeof(segment_header)` = 44 bytes).
- **`published_lsn`**: Maximum contiguous byte offset where frames (payload + header + trailer) are completely written
  and valid.
- **`flushed_lsn`**: Maximum byte offset committed durably to non-volatile storage via `Setu` (`msync`).
- **`replicated_lsn`**: Maximum byte offset confirmed processed by downstream replicas or CDC subscribers.

Direct O(1) segment and offset translation:

- `segment_id = lsn / segment_size`
- `segment_offset = lsn % segment_size`
- **Segment Metadata Range**: `[segment_base, segment_base + sizeof(segment_header))` (44 bytes).
- **First Usable Record LSN**: `segment_base + sizeof(segment_header)`.

### 2. Durability Contract

- **`append(payload)`**: Fast path. Reserves and publishes into mapped memory without forcing `msync`. Advances
  `published_lsn`.
- **`append_sync(payload)`**: Reserves, publishes, and blocks until the record is durably flushed to disk.
- **`sync()`**: Blocks until all currently published records up to `published_lsn` are durably flushed.
- **`wait_durable(target_lsn)`**: Enqueues into the group commit coordinator and waits until
  `flushed_lsn >= target_lsn`. Fails with `LogError::InvalidArg` if `target_lsn > published_lsn`.
- **`flush_to(target_lsn)`**: Low-level durability primitive; validates `target_lsn <= published_lsn` and directly
  flushes segment ranges under `flush_mutex_`. Preferred public API is `wait_durable` or `sync`.

### 3. Leader / Follower Group Commit

`wait_durable()` leverages lock-free MPMC queue ticketing:

- When multiple threads request durability concurrently, one thread acquires the flush leader role.
- The leader drains all queued tickets, computes `max(target_lsn)`, performs a single batched `msync` across affected
  segments, and propagates the result (`LogError::Success` or `LogError::FlushFailed`) to all waiting followers.
- If the MPMC queue is full, `enqueue_commit()` returns `LogError::QueueFull`.

### 4. Background Flusher

When `opts.background_flush = true`, an asynchronous worker flushes pending writes triggered by:

- `group_commit_interval` (time threshold)
- `group_commit_bytes` (watermark gap threshold)
- Explicit `sync()` calls or shutdown.

---

## Binary Frame & Segment Header Layout

### Segment Header (`segment_header` — 44 bytes)

Persisted at offset 0 of every `.log` segment file:

- `magic`: `0x4E534547` ("NSEG")
- `version`: `uint16_t` (format version, default 1)
- `flags`: `uint16_t` (`k_segment_archived = 1 << 0`, `k_segment_sealed = 1 << 1`)
- `segment_id`: `uint64_t`
- `begin_lsn`: `uint64_t`
- `sealed_lsn`: `uint64_t` (persisted on rotation / seal)
- `created_at_unix_ns`: `uint64_t`
- `header_crc`: `uint32_t` (CRC32-C)

### Frame Layout (Overhead: 36 bytes)

```
┌─────────────────────────────────────────────────────────────┐
│                       frame_header (28 B)                   │
│  - magic:        uint32_t (0x4E495459 "NITY")               │
│  - version:      uint16_t (v1 = 1)                          │
│  - flags:        uint16_t (record control flags)            │
│  - size:         uint32_t (payload size in bytes)           │
│  - lsn:          uint64_t (physical byte offset)            │
│  - header_crc:   uint32_t (CRC32-C of header fields)        │
│  - payload_crc:  uint32_t (CRC32-C of payload bytes)       │
├─────────────────────────────────────────────────────────────┤
│                          Payload                            │
│  - span<const std::byte> (opaque payload of length `size`)  │
├─────────────────────────────────────────────────────────────┤
│                       frame_trailer (8 B)                   │
│  - size:         uint32_t (payload size in bytes)           │
│  - payload_crc:  uint32_t (CRC32-C of payload bytes)        │
└─────────────────────────────────────────────────────────────┘
```

---

## Recovery Modes & Diagnostics

Recovery provides fine-grained control over corruption handling:

- **`recovery_mode::strict`**: Corrupt header or checksum immediately terminates scan with error.
- **`recovery_mode::stop_at_first_error`**: Returns all valid records preceding the first corrupted entry.
- **`recovery_mode::salvage`**: Attempts to scan past corrupted byte ranges to salvage downstream valid records.

Each `recovery_stream` maintains an authoritative per-stream `status()`:

```cpp
struct recovery_status {
    lsn_t last_valid_lsn;
    lsn_t first_bad_lsn;
    LogError error;
    std::size_t records_recovered;
    std::size_t bytes_recovered;
    std::uint64_t segment_id;
};
```

---

## Policy Concepts

All pluggable policies are constrained with C++20/23 concepts:

- **`StoragePolicyLike`**: Manages segment file mappings and flush ranges (`setu_storage`).
- **`MemoryPolicyLike`**: Provides zero-allocation scratch buffers (`smriti_memory`).
- **`ConcurrencyPolicyLike`**: MPMC-backed commit coordinator (`group_commit_concurrency`).
- **`FramingPolicyLike`**: Checksums, encoding, header/trailer validation (`default_framing`).
- **`DurabilityPolicyLike`**: Synchronous or asynchronous flush mode (`sync_durability`, `async_durability`).
- **`TelemetryPolicyLike`**: Compile-time zero-overhead NADI trace scopes (`nadi_telemetry`).

---

## Usage Examples

### 1. Basic Append and Durability

```cpp
#include "nitya/nitya.hpp"

nitya::wal_options opts{
    .wal_dir = "/tmp/nitya_wal",
    .segment_size = 4 * 1024 * 1024,
    .background_flush = true
};

nitya::wal<> log{opts};

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
auto recovery = log.recover(0, nitya::recovery_mode::stop_at_first_error);
for (const auto& record : recovery) {
    std::string_view text{reinterpret_cast<const char*>(record.payload.data()), record.payload.size()};
    std::cout << "Replaying LSN " << record.lsn << ": " << text << "\n";
}
auto st = recovery.status();
std::cout << "Recovered " << st.records_recovered << " records, " << st.bytes_recovered << " bytes.\n";
```
