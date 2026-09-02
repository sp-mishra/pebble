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
- **`replicated_lsn`**: Maximum byte offset explicitly acknowledged as durable by downstream replicas or CDC
  subscribers.

Direct O (1) segment and offset translation:

- `segment_id = lsn / segment_size`
- `segment_offset = lsn % segment_size`
- **Segment Metadata Range**: `[segment_base, segment_base + sizeof(segment_header))` (44 bytes).
- **First Usable Record LSN**: `segment_base + sizeof(segment_header)`.

### 2. Durability Contract

- **`append(payload)`**: Fast path. Reserves and publishes into mapped memory without forcing `msync`. Advances
  `published_lsn`.
- **`append_sync(payload)`**: Reserves, publishes, and blocks until the record is durably flushed to disk.
- **`sync()`**: Blocks until all currently published records up to `published_lsn` are durably flushed.
- **`wait_durable(target_lsn)`**: Waits until `flushed_lsn >= target_lsn`. Under the default
  `flush_gate_concurrency` the first waiter flushes the covering range; under the opt-in
  `group_commit_concurrency` it routes through the ticket-queue leader/follower protocol (§3). Fails with
  `LogError::InvalidArg` if `target_lsn > published_lsn`, or with the sticky durability error after a failed flush.
- **`flush_to(target_lsn)`**: Low-level durability primitive; validates `target_lsn <= published_lsn` and directly
  flushes segment ranges under `flush_mutex_`. Preferred public API is `wait_durable` or `sync`.

### 3. Durability Path: First-Waiter Flush (default) + optional Batched Group Commit

The default `ConcurrencyPolicy` is `flush_gate_concurrency`: `wait_durable()` uses a short durability mutex rather than
a spinning ticket protocol.

- The first waiter flushes the current contiguous `published_lsn` watermark in one batched `msync`.
- Concurrent waiters covered by that watermark return after acquiring the mutex and observing `flushed_lsn`.
- This is intentionally I/O-serialized: it guarantees progress and avoids a lock-free hand-off becoming the liveness
  dependency of the durability path.

**Opt-in batched group commit.** For high-fan-in workloads (many concurrent `append_sync` callers), select
`group_commit_concurrency<Capacity>` instead. It advertises `uses_ticket_queue == true`, and
`wait_durable()` dispatches (via `if constexpr`) into a genuine leader/follower protocol: a waiter enqueues a
`commit_ticket`, then either wins the flush mutex to become the leader — draining every waiting ticket and flushing the
single covering range once, marking each ticket's completion — or waits as a follower on its completion / the
`flushed_lsn` watermark. The ticket-queue members are only instantiated when this policy is selected, so the default
path pays nothing for them.

### 4. Background Flusher

When `opts.background_flush = true`, an asynchronous worker flushes pending writes triggered by:

- `group_commit_interval` (time threshold)
- `group_commit_bytes` (watermark gap threshold)
- Explicit `sync()` calls or shutdown.

Failures are never silently converted into successful durability. `health()` exposes the first sticky background or
leader-flush error and its target LSN; `metrics()` exposes publish, group-commit, flush, and replication counters for
Nadi/exporter integration.

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
  Resynchronisation jumps directly to the next frame-magic sentinel using a vectorised sweep (Google Highway, gated on
  `PEBBLE_HAS_HIGHWAY` exactly like the B+Tree module) with a scalar fallback — behaviour is identical either way, only
  the resync is faster than a one-byte-at-a-time crawl.

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

All pluggable policies are constrained with C++20/23 concepts. The full `wal` template parameter list is:

```cpp
template <
    StoragePolicyLike        StoragePolicy   = setu_storage,
    MemoryPolicyLike         MemoryPolicy    = smriti_memory,
    ConcurrencyPolicyLike    ConcurrencyPolicy = flush_gate_concurrency,
    FramingPolicyLike        FramingPolicy   = default_framing,
    DurabilityPolicyLike     DurabilityPolicy = sync_durability,
    TelemetryPolicyLike      TelemetryPolicy = nadi_telemetry,
    ClockPolicyLike          ClockPolicy     = system_clock_source,
    std::size_t              PublishTrackerCapacity = 1024,
    PublishTrackerPolicyLike PublishTrackerPolicy   = static_vector_publish_tracker<PublishTrackerCapacity>>
class wal;
```

- **`StoragePolicyLike`**: Manages segment file mappings and flush ranges (`setu_storage`). Owns segment naming via a
  member `format_segment_name` and a policy-provided filename format (default `"{:010d}.log"`), so sharded/tiered
  storage can namespace segments.
- **`MemoryPolicyLike`**: Zero-allocation scratch (`smriti_memory`). **Wired into recovery/replication**:
  `recovery_stream`/`replication_stream` stage each record's payload through `allocate()` + per-record
  `reset()`, giving an eviction-stable, zero-alloc scratch copy. The append fast path stays direct-to-mmap and never
  touches it.
- **`ConcurrencyPolicyLike`**: Advertises `uses_ticket_queue`. The default `flush_gate_concurrency` supplies only the
  flush-gate surface (first-waiter path, §3); `group_commit_concurrency<Cap>` opts into the ticket queue for batched
  leader/follower group commit. Ticket-queue members are only instantiated when selected.
- **`FramingPolicyLike`**: Checksums, encoding, header/trailer validation, and **on-disk format ownership**
  (`default_framing`). The policy owns `format_version`, the header/trailer/segment-header sizes (the 44/28/8-byte
  `static_assert`s live inside it), and `supports_version()`. **Both** the frame CRC and the segment-header CRC route
  through `FramingPolicy::calculate_checksum32` — a custom framing (xxHash, HW-CRC-off) applies uniformly.
- **`DurabilityPolicyLike`**: Synchronous or asynchronous flush mode (`sync_durability`, `async_durability`).
- **`TelemetryPolicyLike`**: Compile-time zero-overhead NADI trace scopes (`nadi_telemetry`).
- **`ClockPolicyLike`**: Supplies `now_unix_ns()` (`system_clock_source`, wrapping `std::chrono::system_clock`). Segment
  stamping (`created_at_unix_ns`) and retention age consult it, so tests can inject a deterministic clock and
  simulation-time deployments can drive logical time.
- **`PublishTrackerPolicyLike`**: Out-of-order publish interval tracker (`static_vector_publish_tracker`). The common
  contiguous / extends-the-last-interval case is an O (1) append-tail fast path; a high-gap workload can swap an
  interval-tree structure without touching the WAL.

The publish tracker capacity is a `wal` template parameter (`PublishTrackerCapacity`, default 1024); batched group-drain
capacity derives from the selected `ConcurrencyPolicy` instead of an unrelated fixed constant.

### Format evolution

The physical format is owned by the `FramingPolicy`, not frozen by free-standing global asserts.
`default_framing` is v1 and byte-identical to prior releases. Recovery reads the persisted
`segment_header.version` and consults `FramingPolicy::supports_version(v)` rather than hard-comparing against a single
constant — so a future v2 framing policy (its own version, sizes, encode/decode) can accept several versions and coexist
for forward/backward compatibility on long-lived WAL files.

### Clock policy

`ClockPolicy::now_unix_ns()` is the single source of "now" for segment stamping and retention. Injecting a manual clock
makes retention fully deterministic:

```cpp
struct ManualClock {
    static inline std::atomic<std::uint64_t> now_ns{0};
    static std::uint64_t now_unix_ns() noexcept { return now_ns.load(); }
};
using DeterministicWal = nitya::wal<
    nitya::setu_storage_t<nitya::default_framing, ManualClock>, // stamp segments with the clock
    nitya::smriti_memory, nitya::flush_gate_concurrency, nitya::default_framing,
    nitya::sync_durability, nitya::nadi_telemetry, ManualClock>;                 // age segments with the clock
```

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

### 3. Replication acknowledgement

Reading a record is not an acknowledgement. A subscriber advances its private cursor on `next()` and only moves the
WAL-wide retention watermark after the receiver has made the data durable:

```cpp
auto stream = log.subscribe(0);
while (auto record = stream.next()) {
    // send and persist *record at the replica
}
if (auto ack = stream.acknowledge(stream.next_lsn()); !ack) {
    // never acknowledge beyond the delivered cursor
}
```

### 4. Optional Pravaha maintenance adapter

Pravaha is intentionally optional. The durability hot path does not depend on a scheduler; include the adapter only for
recovery or retention maintenance. The `Runner` is constrained by a `pravaha_runner<R>` concept (`submit`,
`backend_ref().drain()`) so misuse is a clear concept error rather than a deep template failure.

Pravaha's `Runner::submit` executes the submitted task graph inline (there is no deferred completion handle yet), so
these adapters are honestly *blocking*: `recover_blocking` / `apply_retention_rules_blocking` submit the work, drain the
backend, and return the result. The `_blocking` names are canonical; the historical `recover_async` /
`apply_retention_rules_async` names are retained as identical-behaviour aliases.

```cpp
#include "pravaha/pravaha.hpp"
#include "nitya/adapters/pravaha.hpp"

pravaha::Runner<pravaha::JThreadBackend> runner;

// Evaluate segment retention & archival policies through Pravaha (blocking)
auto retention = nitya::pravaha_adapter::apply_retention_rules_blocking(
    log, runner,
    std::chrono::seconds(86400),
    [](const nitya::segment_descriptor& seg) {
        std::cout << "Archiving segment " << seg.segment_id << "\n";
    },
    [](const nitya::segment_descriptor& seg) {
        std::cout << "Deleting expired segment " << seg.segment_id << "\n";
    }
);
```
