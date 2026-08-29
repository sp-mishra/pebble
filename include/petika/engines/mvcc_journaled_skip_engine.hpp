#pragma once
// Petika engine binding Anukrama MVCC history to Nitya-assigned LSNs.

#include "containers/anukrama/anukrama.hpp"
#include "petika/engine.hpp"
#include "petika/serializer.hpp"

#include <mutex>
#include <unordered_set>
#include <utility>
#include <vector>

namespace petika {
    template <typename Key, typename Value, typename Comparator = LexicalComparator,
              std::size_t MaxLevel = 16>
    class MvccJournaledSkipEngine {
    public:
        using key_type = Key;
        using value_type = Value;
        using comparator_type = Comparator;
        using store_type = anukrama::store<Key, Value, Comparator>;

        struct EntryView {
            const Key& key;
            const Value& value;
            nitya::lsn_t lsn;
        };

        Result<void> put(const Key& key, const Value& value, const nitya::lsn_t lsn) {
            return apply_writes({typename store_type::write{key, value}}, lsn);
        }
        Result<Value> get(const Key& key) const { return translate(values_.get(key)); }
        Result<Value> get_at(const Key& key, const nitya::lsn_t lsn) const { return translate(values_.get_at(key, lsn)); }
        [[nodiscard]] nitya::lsn_t version_of(const Key& key) const { return values_.version_of(key); }
        [[nodiscard]] nitya::lsn_t version_at(const Key& key, const nitya::lsn_t lsn) const {
            return values_.version_at(key, lsn);
        }
        Result<void> erase(const Key& key, const nitya::lsn_t lsn) {
            if (!values_.contains(key)) return std::unexpected(StorageError::NotFound);
            return apply_writes({typename store_type::write{key, std::nullopt}}, lsn);
        }
        [[nodiscard]] bool contains(const Key& key) const { return values_.contains(key); }
        [[nodiscard]] std::size_t size() const noexcept { return values_.size(); }
        [[nodiscard]] bool empty() const noexcept { return size() == 0; }

        // O(N) clear — Anukrama has no single-epoch truncation API.
        // TODO: add anukrama::store::clear_epoch() to reduce this to O(1) publish.
        Result<void> clear(const nitya::lsn_t lsn) {
            std::vector<typename store_type::write> writes;
            auto snapshot = values_.snapshot_at_current();
            snapshot.for_each([&](const Key& key, const Value&, const anukrama::timestamp) {
                writes.push_back({key, std::nullopt});
            });
            return apply_writes(writes, lsn);
        }

        template <class Range>
        Result<void> apply_batch(const Range& mutations, const nitya::lsn_t lsn) {
            std::vector<typename store_type::write> writes;
            writes.reserve(std::ranges::size(mutations));

            // Build an O(1)-lookup set of keys inserted within this batch to
            // validate deletes without an O(N²) inner scan.
            std::unordered_set<Key> batch_inserts;
            for (const auto& mutation : mutations) {
                if (mutation.op == EntryOp::Put) batch_inserts.insert(mutation.key);
            }

            for (const auto& mutation : mutations) {
                if (mutation.op == EntryOp::Put) {
                    writes.push_back({mutation.key, mutation.value});
                } else if (mutation.op == EntryOp::Delete) {
                    const bool from_this_batch = batch_inserts.contains(mutation.key);
                    if (!from_this_batch && !values_.contains(mutation.key))
                        return std::unexpected(StorageError::NotFound);
                    writes.push_back({mutation.key, std::nullopt});
                } else {
                    return std::unexpected(StorageError::InvalidArg);
                }
            }
            return apply_writes(writes, lsn);
        }

        template <class Callback>
        void scan(const Key& first, const Key& last, Callback&& callback) const {
            auto snapshot = values_.snapshot_at_current();
            snapshot.scan(first, last, [&](const Key& key, const Value& value, const anukrama::timestamp stamp) {
                callback(EntryView{key, value, stamp});
            });
        }
        template <class Callback>
        void for_each(Callback&& callback) const {
            auto snapshot = values_.snapshot_at_current();
            snapshot.for_each([&](const Key& key, const Value& value, const anukrama::timestamp stamp) {
                callback(EntryView{key, value, stamp});
            });
        }

        Result<void> apply_log_record(const EntryOp op, const Key& key, const Value& value, const nitya::lsn_t lsn) {
            switch (op) {
            case EntryOp::Put:    return put(key, value, lsn);
            case EntryOp::Delete: return erase_for_replay(key, lsn);
            case EntryOp::Clear:  return clear(lsn);
            case EntryOp::Batch:  return std::unexpected(StorageError::NotSupported);
            }
            return std::unexpected(StorageError::InvalidArg);
        }

        template <class Range>
        [[nodiscard]] bool validate_observations(const Range& observations) const {
            for (const auto& observation : observations) {
                if (version_of(observation.key) != observation.version) return false;
            }
            return true;
        }

    private:
        [[nodiscard]] bool equivalent(const Key& left, const Key& right) const {
            const Comparator compare{};
            return !compare(left, right) && !compare(right, left);
        }
        template <class T>
        static Result<T> translate(anukrama::result<T> value) {
            if (value) return *std::move(value);
            return std::unexpected(value.error() == anukrama::error::not_found
                ? StorageError::NotFound : StorageError::InternalError);
        }
        Result<void> erase_for_replay(const Key& key, const nitya::lsn_t lsn) {
            return apply_writes({typename store_type::write{key, std::nullopt}}, lsn);
        }
        Result<void> apply_writes(const std::vector<typename store_type::write>& writes, const nitya::lsn_t lsn) {
            std::lock_guard lock{replay_mutex_};
            if (lsn <= last_lsn_) return {};
            auto applied = values_.apply_at(writes, lsn);
            if (!applied) return std::unexpected(StorageError::InternalError);
            last_lsn_ = lsn;
            return {};
        }
        store_type values_;
        mutable std::mutex replay_mutex_;
        nitya::lsn_t last_lsn_{};
    };
} // namespace petika
