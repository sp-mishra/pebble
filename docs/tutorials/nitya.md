# Tutorial: The Immutable Chronicler of Pebble Island — Durable Logging with Nitya

Welcome to **Pebble Island**. As the Island Chronicler, you are charged with recording every financial transaction, legal treaty, vault access, and state transition in the realm. 

Disaster can strike at any moment — sudden power failures, earthquakes, or disk corruption. If a transaction is acknowledged to an island citizen, it must **never be lost**. Furthermore, recording records must never become a bottleneck for the island's bustling economy.

To achieve this, you will use **Nitya** (Sanskrit for *Eternal/Perpetual*) — Pebble's zero-allocation, lock-free, policy-driven Durable Log Engine (DLE) and Write-Ahead Log (WAL).

This tutorial assumes **zero prior knowledge of Write-Ahead Logs or storage engines**. We will build intuition from the ground up, starting with basic append operations and advancing to zero-copy group commits, background durability flushes, corruption recovery, replication streaming, and automated retention rules.

---

## Table of Contents
1. [What is a Write-Ahead Log (WAL)?](#1-what-is-a-write-ahead-log-wal)
2. [The One-File Compilation Blueprint](#2-the-one-file-compilation-blueprint)
3. [Act 1: The First Inscription (Basic Append & Sync)](#act-1-the-first-inscription-basic-append--sync)
4. [Act 2: The Multi-Volume Archive (Segment File Rotation)](#act-2-the-multi-volume-archive-segment-file-rotation)
5. [Act 3: High-Throughput Inscriptions (Zero-Copy Reserve-Publish Pipeline)](#act-3-high-throughput-inscriptions-zero-copy-reserve-publish-pipeline)
6. [Act 4: The Background Durability Daemon (Group Commit & Flusher)](#act-4-the-background-durability-daemon-group-commit--flusher)
7. [Act 5: Reconstructing History (Reading & Scanning the Log)](#act-5-reconstructing-history-reading--scanning-the-log)
8. [Act 6: Surviving the Storm (Crash Recovery Modes)](#act-6-surviving-the-storm-crash-recovery-modes)
9. [Act 7: The Messenger Birds (Replication Subscriptions)](#act-7-the-messenger-birds-replication-subscriptions)
10. [Act 8: The Ancient Scribe's Law (Automated Retention & Archival)](#act-8-the-ancient-scribes-law-automated-retention--archival)
11. [Quick API Reference & Cheat Sheet](#11-quick-api-reference--cheat-sheet)

---

## 1. What is a Write-Ahead Log (WAL)?

In database systems and distributed infrastructure, in-memory caches and complex B-Trees are fast but volatile. When a crash occurs, any unwritten state in RAM vanishes.

A **Write-Ahead Log (WAL)** is an append-only sequence of immutable records written sequentially to disk. Before mutating in-memory state, the system writes the intent to the log. 

### Why Nitya?
- **Zero Heap Allocations on Hot Path**: Memory-mapped (`Setu`) write buffers.
- **Physical Byte Offset LSNs**: Every record's Log Sequence Number (LSN) corresponds directly to its physical byte offset in the linear log space.
- **CRC32-C Integrity Validation**: Dual-checksum verification with packed headers and trailers protecting against torn writes.
- **Lock-Free Concurrency**: Concurrent workers reserve space concurrently without holding coarse-grained file locks.

---

## 2. The One-File Compilation Blueprint

Nitya is **header-only** and designed for **modern C++23**.

Save the following template as `main.cpp` and compile with any C++23 compiler:

```cpp
#include "nitya/nitya.hpp"

#include <iostream>
#include <string>
#include <string_view>
#include <span>
#include <vector>
#include <filesystem>

using namespace nitya;

// Helper: convert string to byte span
inline std::span<const std::byte> as_bytes(std::string_view s) {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

// Helper: convert byte span to string_view
inline std::string_view as_string(std::span<const std::byte> b) {
    return {reinterpret_cast<const char*>(b.data()), b.size()};
}

int main() {
    std::cout << "Nitya Durable Log Engine initialized!\n";
    return 0;
}
```

---

## Act 1: The First Inscription (Basic Append & Sync)

### The Story
The Harbor Master needs to record incoming ships. When a ship docks, its name is appended to the log. The Harbor Master calls `sync()` to ensure the bytes are physically flushed to the storage disk before granting the ship entrance.

### The Code
```cpp
void act1_basic_append() {
    std::filesystem::path wal_dir = "./harbor_wal";
    std::filesystem::create_directories(wal_dir);

    wal_options opts{
        .wal_dir = wal_dir,
        .segment_size = 1024 * 1024 // 1 MB segment files
    };

    // Open or create the WAL
    wal<> log{opts};

    std::cout << "[Act 1] Initial Tail LSN: " << log.tail_lsn() << "\n";

    // Append records
    auto lsn1 = log.append(as_bytes("SHIP_ARRIVED: Sea Breeze"));
    auto lsn2 = log.append(as_bytes("SHIP_ARRIVED: Northern Star"));

    if (lsn1 && lsn2) {
        std::cout << "Recorded entry 1 at LSN: " << *lsn1 << "\n";
        std::cout << "Recorded entry 2 at LSN: " << *lsn2 << "\n";
    }

    // Force flush to persistent disk
    auto sync_res = log.sync();
    if (sync_res) {
        std::cout << "Flushed to disk! Flushed LSN: " << log.flushed_lsn() << "\n";
    }
}
```

---

## Act 2: The Multi-Volume Archive (Segment File Rotation)

### The Concept
A single log file cannot grow infinitely. Nitya partitions the continuous LSN address space into **fixed-size segment files** (e.g. `seg_0000000000000000.wal`, `seg_0000000000000001.wal`).

- Each segment starts with a 44-byte packed `segment_header`.
- When a record exceeds the segment size, Nitya automatically **seals** the current segment and **rotates** to the next.

### The Code
```cpp
void act2_segment_rotation() {
    std::filesystem::path wal_dir = "./rotation_wal";
    std::filesystem::create_directories(wal_dir);

    // Use a small 256-byte segment size to observe auto-rotation
    wal_options opts{
        .wal_dir = wal_dir,
        .segment_size = 256,
        .auto_rotate = true
    };

    wal<> log{opts};

    std::string payload1(100, 'A');
    std::string payload2(100, 'B');

    auto lsn1 = log.append(as_bytes(payload1));
    std::cout << "[Act 2] Record 1 in Segment 0 at LSN: " << *lsn1 << "\n";

    // Record 2 exceeds the remaining space in Segment 0 -> Rotates to Segment 1
    auto lsn2 = log.append(as_bytes(payload2));
    std::cout << "[Act 2] Record 2 in Segment 1 at LSN: " << *lsn2 << "\n";

    log.sync();
}
```

---

## Act 3: High-Throughput Inscriptions (Zero-Copy Reserve-Publish Pipeline)

### The Concept
Calling `append()` copies payload buffers. For high-performance telemetry and database engines, Nitya provides a **zero-copy 2-phase pipeline**:
1. **`reserve(payload_size)`**: Atomically reserves a slice in the memory-mapped segment without taking locks.
2. **Direct Memory Write**: You format or serialize data directly into `reservation.payload_buffer()`.
3. **`publish(reservation)`**: Computes CRC32-C checksums and marks the record ready for commit.

### The Code
```cpp
void act3_zero_copy_pipeline() {
    std::filesystem::path wal_dir = "./zerocopy_wal";
    std::filesystem::create_directories(wal_dir);

    wal<> log{wal_options{.wal_dir = wal_dir, .segment_size = 1024 * 1024}};

    struct TradeRecord {
        uint64_t trade_id;
        double price;
        uint32_t quantity;
    } trade{10492, 142.50, 500};

    // 1. Reserve exact space in the mapped log
    auto res_expected = log.reserve(sizeof(TradeRecord));
    if (!res_expected) {
        std::cerr << "Failed to reserve WAL space!\n";
        return;
    }

    reservation res = std::move(*res_expected);

    // 2. Write directly into the mapped buffer (Zero Copy!)
    std::memcpy(res.payload_buffer().data(), &trade, sizeof(TradeRecord));

    // 3. Publish record
    auto pub_res = log.publish(res);
    if (pub_res) {
        std::cout << "[Act 3] Zero-copy trade published at LSN: " << *pub_res << "\n";
    }

    log.sync();
}
```

---

## Act 4: The Background Durability Daemon (Group Commit & Flusher)

### The Concept
Calling `fsync` after every individual record limits throughput to disk IOPS (~1,000 writes/sec).

Nitya features a background **Group Commit Durability Flusher**. Threads append records concurrently without blocking for disk sync. The background flusher periodically batches and flushes dirty bytes based on **time intervals** and **byte watermarks**.

### The Code
```cpp
void act4_background_flusher() {
    std::filesystem::path wal_dir = "./group_commit_wal";
    std::filesystem::create_directories(wal_dir);

    wal_options opts{
        .wal_dir = wal_dir,
        .segment_size = 16 * 1024 * 1024,
        .background_flush = true,
        .group_commit_interval = std::chrono::microseconds(1000), // Flush every 1ms
        .group_commit_bytes = 64 * 1024                           // Or every 64 KB
    };

    wal<> log{opts};

    // High throughput non-blocking appends
    for (int i = 0; i < 1000; ++i) {
        std::string msg = "TRANSACTION_ID_" + std::to_string(i);
        log.append(as_bytes(msg));
    }

    std::cout << "[Act 4] Appended 1000 records asynchronously!\n";

    // Wait until background flusher catches up
    log.sync();
    std::cout << "All 1000 records flushed durably to disk!\n";
}
```

---

## Act 5: Reconstructing History (Reading & Scanning the Log)

### The Concept
To recover state or stream transactions to consumers, Nitya provides zero-allocation point reads and sequential iterators across segment boundaries.

### The Code
```cpp
void act5_read_and_scan() {
    std::filesystem::path wal_dir = "./scan_wal";
    std::filesystem::create_directories(wal_dir);

    wal<> log{wal_options{.wal_dir = wal_dir, .segment_size = 1024 * 1024}};

    auto lsn1 = *log.append(as_bytes("RECORD_ALPHA"));
    auto lsn2 = *log.append(as_bytes("RECORD_BETA"));
    auto lsn3 = *log.append(as_bytes("RECORD_GAMMA"));
    log.sync();

    // 1. Point Read by exact LSN
    auto record1 = log.read(lsn1);
    if (record1) {
        std::cout << "[Act 5] Point Read @ " << lsn1 << ": " << as_string(record1->payload) << "\n";
    }

    // 2. Sequential Scan across the entire log
    std::cout << "Scanning all records in log:\n";
    auto iter = log.scan(k_segment_header_size); // Start at first valid record
    while (iter.has_next()) {
        auto rec = iter.next();
        if (!rec) break;
        std::cout << "  - LSN " << rec->lsn << ": " << as_string(rec->payload) << "\n";
    }
}
```

---

## Act 6: Surviving the Storm (Crash Recovery Modes)

### The Concept
When an application crashes mid-write, the end of the log may contain incomplete frames or corrupted bytes.

Nitya provides three recovery policies via `recovery_mode`:
1. **`recovery_mode::strict`**: Fails immediately if any CRC mismatch or corrupt header is encountered.
2. **`recovery_mode::stop_at_first_error`** (Standard): Recovers all valid records up to the crash point, truncates the torn write, and resumes cleanly.
3. **`recovery_mode::salvage`**: Skips over corrupt byte regions and recovers any valid downstream frames.

### The Code
```cpp
void act6_crash_recovery() {
    std::filesystem::path wal_dir = "./recovery_wal";
    std::filesystem::create_directories(wal_dir);

    // Phase 1: Write initial records and close
    {
        wal<> log{wal_options{.wal_dir = wal_dir, .segment_size = 1024 * 1024}};
        log.append(as_bytes("TX_COMMITTED_1"));
        log.append(as_bytes("TX_COMMITTED_2"));
        log.sync();
    }

    // Phase 2: Open and recover with stop_at_first_error
    {
        wal<> recovered_log{wal_options{.wal_dir = wal_dir, .segment_size = 1024 * 1024}};
        auto status = recovered_log.recover(recovery_mode::stop_at_first_error);

        if (status) {
            std::cout << "[Act 6] Recovery Complete!\n";
            std::cout << "  Records Recovered: " << status->records_recovered << "\n";
            std::cout << "  Bytes Recovered:   " << status->bytes_recovered << "\n";
            std::cout << "  Last Valid LSN:    " << status->last_valid_lsn << "\n";
        }
    }
}
```

---

## Act 7: The Messenger Birds (Replication Subscriptions)

### The Concept
In distributed clusters (such as Raft or Primary-Replica architectures), follower nodes need to tail the primary's log in real-time.

Nitya provides a **Replication Stream Subscription Engine** that notifies streaming consumers whenever new LSNs are published or flushed.

### The Code
```cpp
void act7_replication_stream() {
    std::filesystem::path wal_dir = "./replication_wal";
    std::filesystem::create_directories(wal_dir);

    wal<> log{wal_options{.wal_dir = wal_dir, .segment_size = 1024 * 1024}};

    // Create a subscription stream starting at the beginning of the log
    auto subscription = log.create_subscription(k_segment_header_size);

    log.append(as_bytes("REPL_EVENT_LOGIN"));
    log.append(as_bytes("REPL_EVENT_TRANSFER"));
    log.sync();

    // Replicator reads stream updates
    while (subscription.has_available()) {
        auto entry = subscription.poll_next();
        if (!entry) break;
        std::cout << "[Act 7] Replicating LSN " << entry->lsn 
                  << " -> Follower: " << as_string(entry->payload) << "\n";
    }
}
```

---

## Act 8: The Ancient Scribe's Law (Automated Retention & Archival)

### The Concept
Old segments that have already been backed up to cold storage or checkpointed should be reclaimed to free disk space.

Nitya integrates with **EasyRules** for declarative administrative retention policies:
- **`truncate_before(lsn)`**: Reclaims physical segment files whose `sealed_lsn <= lsn`.
- **`archive_segment(segment_id)`**: Marks segments as archived for backup pipelines.

### The Code
```cpp
void act8_retention_and_cleanup() {
    std::filesystem::path wal_dir = "./retention_wal";
    std::filesystem::create_directories(wal_dir);

    wal_options opts{
        .wal_dir = wal_dir,
        .segment_size = 256, // Small segments to trigger multiple files
        .auto_rotate = true
    };

    wal<> log{opts};

    for (int i = 0; i < 10; ++i) {
        log.append(as_bytes("AUDIT_LOG_ENTRY_" + std::to_string(i)));
    }
    log.sync();

    std::cout << "[Act 8] Tail LSN after 10 entries: " << log.tail_lsn() << "\n";

    // Truncate all historical segments before LSN 512
    auto trunc_res = log.truncate_before(512);
    if (trunc_res) {
        std::cout << "Purged historical segments prior to checkpoint LSN 512!\n";
    }
}
```

---

## 11. Quick API Reference & Cheat Sheet

| Category | Method | Description |
|---|---|---|
| **Lifecycle** | `wal log(opts)` | Initialize or open WAL engine |
| **Simple Writes** | `log.append(span)` | Append record and advance tail LSN |
| **Zero-Copy Writes** | `log.reserve(size)`, `log.publish(res)` | 2-phase non-blocking mapped buffer write |
| **Durability** | `log.sync()` | Force synchronous `fsync` to disk |
| **Coordinates** | `log.tail_lsn()`, `log.flushed_lsn()` | Query current allocation & disk watermarks |
| **Reads** | `log.read(lsn)` | Point lookup of a record by physical LSN |
| **Scans** | `log.scan(start_lsn)` | Sequential log iterator |
| **Recovery** | `log.recover(mode)` | Crash recovery (`strict`, `stop_at_first_error`, `salvage`) |
| **Replication** | `log.create_subscription(lsn)` | Real-time stream consumer for followers |
| **Retention** | `log.truncate_before(lsn)` | Reclaim sealed historical segment files |
