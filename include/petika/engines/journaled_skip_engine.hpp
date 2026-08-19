#pragma once
// ============================================================================
// petika/engines/journaled_skip_engine.hpp — Journaled Skip List Storage Engine
// ============================================================================
//
// Primary production engine for Petika:
//   - Backed by containers::SkipList for clean container separation & reuse
//   - Stores Key -> NodePayload (value + LSN)
//   - O(log n) point lookups and O(log n + k) ordered range scans
//   - Deterministic recovery replaying Nitya durable log entries
//   - Zero virtual functions, zero RTTI, zero macros
// ============================================================================

#include "petika/engine.hpp"
#include "petika/serializer.hpp"
#include "containers/associative/SkipList.hpp"

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
            : list_{comp} {}

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
            list_.insert_or_assign(key, NodePayload{.value = value, .lsn = lsn});
            return {};
        }

        Result<Value> get(const Key& key) const {
            std::shared_lock lk{mutex_};
            auto it = list_.find(key);
            if (it == list_.end()) {
                return std::unexpected(StorageError::NotFound);
            }
            return it->second.value;
        }

        Result<void> erase(const Key& key, nitya::lsn_t /*lsn*/) {
            std::unique_lock lk{mutex_};
            if (!list_.erase(key)) {
                return std::unexpected(StorageError::NotFound);
            }
            return {};
        }

        [[nodiscard]] bool contains(const Key& key) const {
            std::shared_lock lk{mutex_};
            return list_.contains(key);
        }

        [[nodiscard]] std::size_t size() const noexcept {
            std::shared_lock lk{mutex_};
            return list_.size();
        }

        [[nodiscard]] bool empty() const noexcept {
            std::shared_lock lk{mutex_};
            return list_.empty();
        }

        Result<void> clear(nitya::lsn_t /*lsn*/) {
            std::unique_lock lk{mutex_};
            list_.clear();
            return {};
        }

        // ------------------------------------------------------------------------
        // Range Scan: [start_key, end_key)
        // ------------------------------------------------------------------------
        template <typename Callback>
        void scan(const Key& start_key, const Key& end_key, Callback&& cb) const {
            std::shared_lock lk{mutex_};
            Comparator comp{};
            for (auto it = list_.lower_bound(start_key); it != list_.end() && comp(it->first, end_key); ++it) {
                cb(EntryView{.key = it->first, .value = it->second.value, .lsn = it->second.lsn});
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
                case EntryOp::Put:
                    return put(key, val, lsn);
                case EntryOp::Delete:
                    return erase(key, lsn);
                case EntryOp::Clear:
                    return clear(lsn);
            }
            return {};
        }

    private:
        IndexType list_;
        mutable std::shared_mutex mutex_;
    };

} // namespace petika
