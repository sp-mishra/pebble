#pragma once
// ============================================================================
// petika/engine.hpp — Petika Engine Concepts, Contracts & Base Types
// ============================================================================
//
// Defines the core `Engine` concept for Petika.
// Engines implement storage and indexing while delegating durability (Nitya),
// memory (Smriti), concurrency (Containers), telemetry (NADI), and
// policies (EasyRules) to the Petika common platform layer.
// ============================================================================

#include "nitya/nitya.hpp"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace petika {
    enum class StorageError : std::uint8_t {
        Success = 0,
        NotFound,
        KeyExists,
        InvalidArg,
        CorruptedRecord,
        WalError,
        StorageFull,
        TransactionAborted,
        NotSupported,
        InternalError,
        Retry = 10  // OCC conflict — caller should retry from a fresh snapshot
    };

    template <typename T>
    using Result = std::expected<T, StorageError>;

    enum class EntryOp : std::uint8_t {
        Put = 1,
        Delete = 2,
        Clear = 3,
        Batch = 4
    };

    struct Manifest {
        std::string engine_type{"JournaledSkipEngine"};
        std::uint32_t version{1};
        nitya::lsn_t last_lsn{0};
        nitya::lsn_t checkpoint_lsn{0};
        nitya::lsn_t compaction_lsn{0};     // LSN at which last compaction/GC ran
        std::uint64_t snapshot_id{0};
        std::uint64_t record_count{0};
        std::uint64_t wal_bytes_written{0};  // cumulative WAL bytes appended
    };

    // Core engine concept — minimum interface all Petika engines must satisfy.
    template <typename E, typename Key, typename Value>
    concept StorageEngine = requires(
        E& engine,
        const Key& key,
        const Value& val,
        nitya::lsn_t lsn,
        std::span<const std::byte> payload,
        EntryOp op
    ) {
            { engine.put(key, val, lsn) } -> std::same_as<Result<void>>;
            { engine.get(key) } -> std::same_as<Result<Value>>;
            { engine.erase(key, lsn) } -> std::same_as<Result<void>>;
            { engine.contains(key) } -> std::same_as<bool>;
            { engine.size() } noexcept -> std::same_as<std::size_t>;
            { engine.empty() } noexcept -> std::same_as<bool>;
            { engine.clear(lsn) } -> std::same_as<Result<void>>;
            { engine.apply_log_record(op, key, val, lsn) } -> std::same_as<Result<void>>;
        };

    // Refinement concept for engines that support atomic bulk writes.
    // Engines satisfying BatchEngine enable transactional commit_batch in Petika.
    template <typename E, typename Key, typename Value>
    concept BatchEngine = StorageEngine<E, Key, Value> && requires(
        E& engine,
        std::vector<std::pair<EntryOp, Key>> batch,
        nitya::lsn_t lsn
    ) {
        { engine.apply_batch(batch, lsn) } -> std::same_as<Result<void>>;
    };

    // MutexPolicy concept — enables ConcurrencyPolicy injection into Petika.
    template <typename P>
    concept MutexPolicy = requires(P& p) {
        { p.lock() } -> std::same_as<void>;
        { p.unlock() } -> std::same_as<void>;
        { p.lock_shared() } -> std::same_as<void>;
        { p.unlock_shared() } -> std::same_as<void>;
    };

    // Zero-overhead no-op mutex for single-threaded use-cases.
    struct NullMutex {
        constexpr void lock() noexcept {}
        constexpr void unlock() noexcept {}
        constexpr void lock_shared() noexcept {}
        constexpr void unlock_shared() noexcept {}
    };

} // namespace petika
