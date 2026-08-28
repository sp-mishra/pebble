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
        InternalError
    };

    template <typename T>
    using Result = std::expected<T, StorageError>;

    enum class EntryOp : std::uint8_t {
        Put = 1,
        Delete = 2,
        Clear = 3,
        Batch = 4
    };

    // Forward declaration of Manifest
    struct Manifest {
        std::string engine_type{"JournaledSkipEngine"};
        std::uint32_t version{1};
        nitya::lsn_t last_lsn{0};
        nitya::lsn_t checkpoint_lsn{0};
        std::uint64_t snapshot_id{0};
        std::uint64_t record_count{0};
    };

    // Engine concept defining required operations for pluggable storage engines.
    template <typename E, typename Key, typename Value>
    concept StorageEngine = requires(
        E& engine,
        const Key& key,
        const Value& val,
        nitya::lsn_t lsn,
        std::span<const std::byte> payload,
        EntryOp op
    ) {
            // Core operations
            { engine.put(key, val, lsn) } -> std::same_as<Result<void>>;
            { engine.get(key) } -> std::same_as<Result<Value>>;
            { engine.erase(key, lsn) } -> std::same_as<Result<void>>;
            { engine.contains(key) } -> std::same_as<bool>;
            { engine.size() } noexcept -> std::same_as<std::size_t>;
            { engine.empty() } noexcept -> std::same_as<bool>;
            { engine.clear(lsn) } -> std::same_as<Result<void>>;

            // Log record replay for recovery
            { engine.apply_log_record(op, key, val, lsn) } -> std::same_as<Result<void>>;
        };
} // namespace petika
