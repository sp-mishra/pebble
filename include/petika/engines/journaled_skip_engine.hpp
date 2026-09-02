#pragma once
// ============================================================================
// petika/engines/journaled_skip_engine.hpp — Journaled Skip List Storage Engine
// ============================================================================
//
// Primary single-version engine for Petika:
//   - Backed by containers::SkipList for clean container separation & reuse
//   - Stores Key -> NodePayload (value + LSN)
//   - O(log n) point lookups and O(log n + k) ordered range scans
//   - Deterministic recovery replaying Nitya durable log entries
//   - Zero virtual functions, zero RTTI, zero macros
//
// WAL is the rollback authority: failed apply_batch leaves partial state
// that recovery corrects by replaying to the last committed LSN boundary.
// Use MvccJournaledSkipEngine when true atomic CoW visibility is required.
// ============================================================================

#include "petika/engine.hpp"
#include "petika/serializer.hpp"
#include "containers/associative/SkipList.hpp"

#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>

namespace petika {
    template <
        typename Key,
        typename Value,
        typename Comparator = LexicalComparator,
        std::size_t MaxLevel = 16
    >
    class JournaledSkipEngine {
    public:
        using key_type = Key;
        using value_type = Value;
        using comparator_type = Comparator;

        struct NodePayload {
            Value value;
            nitya::lsn_t lsn{0};
        };

        using IndexType = containers::SkipList<Key, NodePayload, Comparator, MaxLevel>;

        struct EntryView {
            const Key& key;
            const Value& value;
            nitya::lsn_t lsn;
        };

        explicit JournaledSkipEngine(Comparator comp = Comparator{})
            : list_{comp}, size_{0} {}

        ~JournaledSkipEngine() = default;
        JournaledSkipEngine(const JournaledSkipEngine&) = delete;
        JournaledSkipEngine& operator=(const JournaledSkipEngine&) = delete;
        JournaledSkipEngine(JournaledSkipEngine&&) noexcept = default;
        JournaledSkipEngine& operator=(JournaledSkipEngine&&) noexcept = default;

        // ------------------------------------------------------------------------
        // Point Operations
        // ------------------------------------------------------------------------
        Result<void> put(const Key& key, const Value& value, nitya::lsn_t lsn) {
            std::unique_lock lk{mutex_};
            const bool inserted = !list_.contains(key);
            list_.insert_or_assign(key, NodePayload{.value = value, .lsn = lsn});
            if (inserted) size_.fetch_add(1, std::memory_order_relaxed);
            return {};
        }

        Result<Value> get(const Key& key) const {
            std::shared_lock lk{mutex_};
            auto it = list_.find(key);
            if (it == list_.end()) return std::unexpected(StorageError::NotFound);
            return it->second.value;
        }

        Result<void> erase(const Key& key, nitya::lsn_t /*lsn*/) {
            std::unique_lock lk{mutex_};
            if (!list_.erase(key)) return std::unexpected(StorageError::NotFound);
            size_.fetch_sub(1, std::memory_order_relaxed);
            return {};
        }

        [[nodiscard]] bool contains(const Key& key) const {
            std::shared_lock lk{mutex_};
            return list_.contains(key);
        }

        // Lock-free size query via atomic counter.
        [[nodiscard]] std::size_t size() const noexcept {
            return size_.load(std::memory_order_relaxed);
        }

        [[nodiscard]] bool empty() const noexcept {
            return size_.load(std::memory_order_relaxed) == 0;
        }

        Result<void> clear(nitya::lsn_t /*lsn*/) {
            std::unique_lock lk{mutex_};
            list_.clear();
            size_.store(0, std::memory_order_relaxed);
            return {};
        }

        // Apply mutations directly under unique_lock. WAL is the rollback authority.
        template <class Range>
        Result<void> apply_batch(const Range& mutations, nitya::lsn_t lsn) {
            std::unique_lock lk{mutex_};
            for (const auto& mutation : mutations) {
                if (mutation.op == EntryOp::Put) {
                    const bool inserted = !list_.contains(mutation.key);
                    list_.insert_or_assign(mutation.key, NodePayload{.value = mutation.value, .lsn = lsn});
                    if (inserted) size_.fetch_add(1, std::memory_order_relaxed);
                }
                else if (mutation.op == EntryOp::Delete) {
                    if (!list_.erase(mutation.key)) return std::unexpected(StorageError::NotFound);
                    size_.fetch_sub(1, std::memory_order_relaxed);
                }
                else {
                    return std::unexpected(StorageError::InvalidArg);
                }
            }
            return {};
        }

        // ------------------------------------------------------------------------
        // Range Scan: [start_key, end_key)
        // ------------------------------------------------------------------------
        template <typename Callback>
        void scan(const Key& start_key, const Key& end_key, Callback&& cb) const {
            std::shared_lock lk{mutex_};
            Comparator comp{};
            if constexpr (ThreeWayComparator<Comparator, Key>) {
                for (auto it = list_.lower_bound(start_key);
                     it != list_.end() && comp.three_way(it->first, end_key) < 0; ++it) {
                    cb(EntryView{.key = it->first, .value = it->second.value, .lsn = it->second.lsn});
                }
            }
            else {
                for (auto it = list_.lower_bound(start_key);
                     it != list_.end() && comp(it->first, end_key); ++it) {
                    cb(EntryView{.key = it->first, .value = it->second.value, .lsn = it->second.lsn});
                }
            }
        }

        // ------------------------------------------------------------------------
        // Full Forward Iterate
        // ------------------------------------------------------------------------
        template <typename Callback>
        void for_each(Callback&& cb) const {
            std::shared_lock lk{mutex_};
            for (auto it = list_.begin(); it != list_.end(); ++it) {
                cb(EntryView{.key = it->first, .value = it->second.value, .lsn = it->second.lsn});
            }
        }

        // ------------------------------------------------------------------------
        // Recovery Replay
        // ------------------------------------------------------------------------
        Result<void> apply_log_record(EntryOp op, const Key& key, const Value& val, nitya::lsn_t lsn) {
            switch (op) {
            case EntryOp::Put: return put(key, val, lsn);
            case EntryOp::Delete: return erase(key, lsn);
            case EntryOp::Clear: return clear(lsn);
            case EntryOp::Batch: return std::unexpected(StorageError::NotSupported);
            }
            return {};
        }

    private:
        IndexType list_;
        mutable std::shared_mutex mutex_;
        std::atomic<std::size_t> size_;
    };
} // namespace petika
