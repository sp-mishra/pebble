#pragma once
// Header-only static-composition MVCC substrate. No virtual dispatch or public macros.

#include "containers/associative/SkipList.hpp"

#include <algorithm>
#include <atomic>
#include <concepts>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <utility>
#include <vector>

namespace anukrama {
    using timestamp = std::uint64_t;

    enum class error { not_found, conflict, invalid_snapshot, transaction_finished, stale_timestamp, allocation_failure };
    template <class T> using result = std::expected<T, error>;

    template <class Clock>
    concept logical_clock = requires(Clock clock) {
        { clock.now() } noexcept -> std::same_as<timestamp>;
        { clock.next() } -> std::same_as<timestamp>;
    };

    class atomic_clock {
    public:
        [[nodiscard]] timestamp now() const noexcept { return committed_.load(std::memory_order_acquire); }
        [[nodiscard]] timestamp next() noexcept { return committed_.fetch_add(1, std::memory_order_acq_rel) + 1; }
        [[nodiscard]] bool advance_to(const timestamp value) noexcept {
            auto observed = committed_.load(std::memory_order_acquire);
            while (observed < value && !committed_.compare_exchange_weak(
                observed, value, std::memory_order_acq_rel, std::memory_order_acquire)) {}
            return observed < value;
        }
    private:
        std::atomic<timestamp> committed_{0};
    };

    template <class Clock>
    concept externally_advanceable_clock = logical_clock<Clock> && requires(Clock clock, timestamp value) {
        { clock.advance_to(value) } noexcept -> std::convertible_to<bool>;
    };

    struct snapshot_isolation { static constexpr bool validate_reads = false; };
    struct optimistic_point_serializable { static constexpr bool validate_reads = true; };

    template <class Policy>
    concept conflict_policy = requires { { Policy::validate_reads } -> std::convertible_to<bool>; };

    template <class Key, class Chain, class Compare>
    using skip_list_index = containers::SkipList<Key, Chain, Compare>;

    template <class Index, class Key, class Chain>
    concept ordered_version_index = requires(Index index, const Index constant, const Key& key,
                                             Key owned_key, Chain chain) {
        { index.find(key) };
        { constant.find(key) };
        { index.begin() };
        { constant.begin() };
        { index.end() };
        { constant.end() };
        index.insert_or_assign(std::move(owned_key), std::move(chain));
        { index.erase(key) } -> std::convertible_to<bool>;
    };

    template <class Key, class Value, class Compare = std::less<>,
              template <class, class, class> class IndexPolicy = skip_list_index,
              externally_advanceable_clock Clock = atomic_clock,
              conflict_policy ConflictPolicy = snapshot_isolation>
        requires std::copy_constructible<Key> && std::copy_constructible<Value>
    class store {
        struct version_node {
            timestamp stamp{};
            std::optional<Value> value{};
            std::unique_ptr<version_node> previous{};
            version_node(timestamp version, std::optional<Value> payload)
                : stamp{version}, value{std::move(payload)} {}
        };
        struct chain { std::unique_ptr<version_node> head{}; };
        using index_type = IndexPolicy<Key, chain, Compare>;
        static_assert(ordered_version_index<index_type, Key, chain>);
        struct observed_key { Key key; timestamp head_stamp{}; };

    public:
        struct write {
            Key key;
            std::optional<Value> value;
        };
        // A caller-provided optimistic validation token. A zero version means
        // the key was absent when it was observed.
        struct observation {
            Key key;
            timestamp version{};
        };
        class snapshot;
        class transaction;
        store() = default;
        explicit store(Compare compare) : compare_{compare}, index_{std::move(compare)} {}
        store(const store&) = delete;
        store& operator=(const store&) = delete;

        [[nodiscard]] snapshot snapshot_at_current() const {
            std::unique_lock lock{mutex_};
            const auto boundary = clock_.now();
            active_snapshots_.push_back(boundary);
            return snapshot{*this, boundary};
        }
        [[nodiscard]] transaction begin() { return transaction{*this, snapshot_at_current()}; }
        [[nodiscard]] result<Value> get(const Key& key) const {
            std::shared_lock lock{mutex_};
            return read_at_locked(key, clock_.now());
        }
        [[nodiscard]] result<Value> get_at(const Key& key, const timestamp boundary) const {
            std::shared_lock lock{mutex_};
            return read_at_locked(key, boundary);
        }
        [[nodiscard]] timestamp version_of(const Key& key) const {
            std::shared_lock lock{mutex_};
            return head_stamp_locked(key);
        }
        [[nodiscard]] timestamp version_at(const Key& key, const timestamp boundary) const {
            std::shared_lock lock{mutex_};
            const auto it = index_.find(key);
            if (it == index_.end()) return 0;
            auto* node = it->second.head.get();
            while (node && node->stamp > boundary) node = node->previous.get();
            return node ? node->stamp : 0;
        }
        [[nodiscard]] bool contains(const Key& key) const { return get(key).has_value(); }
        [[nodiscard]] std::size_t size() const {
            std::shared_lock lock{mutex_};
            std::size_t result{};
            for (auto it = index_.begin(); it != index_.end(); ++it)
                result += it->second.head && it->second.head->value ? 1U : 0U;
            return result;
        }

        // For a durability layer that already owns ordering (for example a
        // Nitya WAL record), install a fully prepared batch at its commit LSN.
        [[nodiscard]] result<void> apply_at(const std::vector<write>& writes, const timestamp committed) {
            std::unique_lock lock{mutex_};
            if (committed <= clock_.now()) return std::unexpected(error::stale_timestamp);
            if (auto applied = publish_locked(writes, committed); !applied) return applied;
            (void)clock_.advance_to(committed);
            return {};
        }

        // Atomically validates all observed point versions and publishes the
        // complete write batch under the same writer lock. This is the bridge
        // used by external transaction coordinators such as Medha.
        [[nodiscard]] result<timestamp> commit_if_unchanged(
            const std::vector<observation>& observed,
            const std::vector<write>& writes) {
            std::unique_lock lock{mutex_};
            for (const auto& read : observed) {
                if (head_stamp_locked(read.key) != read.version)
                    return std::unexpected(error::conflict);
            }
            const auto committed = clock_.next();
            if (auto applied = publish_locked(writes, committed); !applied)
                return std::unexpected(applied.error());
            return committed;
        }

        // Explicit reclamation: retain the latest version visible to every live snapshot.
        void prune() {
            std::unique_lock lock{mutex_};
            const auto boundary = oldest_active_snapshot_locked().value_or(clock_.now());
            for (auto it = index_.begin(); it != index_.end();) {
                auto next = it; ++next;
                auto& head = it->second.head;
                if (head && !head->value && head->stamp <= boundary) {
                    index_.erase(it->first);
                } else if (head) {
                    auto* retained = head.get();
                    while (retained->stamp > boundary && retained->previous) retained = retained->previous.get();
                    retained->previous.reset();
                }
                it = next;
            }
        }

        class snapshot {
        public:
            snapshot() = default;
            snapshot(const snapshot&) = delete;
            snapshot& operator=(const snapshot&) = delete;
            snapshot(snapshot&& other) noexcept : owner_{std::exchange(other.owner_, nullptr)}, boundary_{other.boundary_} {}
            snapshot& operator=(snapshot&& other) noexcept {
                if (this != &other) { reset(); owner_ = std::exchange(other.owner_, nullptr); boundary_ = other.boundary_; }
                return *this;
            }
            ~snapshot() { reset(); }
            [[nodiscard]] timestamp timestamp_value() const noexcept { return boundary_; }
            [[nodiscard]] bool valid() const noexcept { return owner_ != nullptr; }
            [[nodiscard]] result<Value> get(const Key& key) const {
                if (!owner_) return std::unexpected(error::invalid_snapshot);
                std::shared_lock lock{owner_->mutex_};
                return owner_->read_at_locked(key, boundary_);
            }
            template <class Callback>
            void scan(const Key& first, const Key& last, Callback&& callback) const {
                if (!owner_) return;
                std::shared_lock lock{owner_->mutex_};
                for (auto it = owner_->index_.lower_bound(first); it != owner_->index_.end() &&
                     owner_->compare_(it->first, last); ++it) {
                    auto* node = it->second.head.get();
                    while (node && node->stamp > boundary_) node = node->previous.get();
                    if (node && node->value) callback(it->first, *node->value, node->stamp);
                }
            }
            template <class Callback>
            void for_each(Callback&& callback) const {
                if (!owner_) return;
                std::shared_lock lock{owner_->mutex_};
                for (auto it = owner_->index_.begin(); it != owner_->index_.end(); ++it) {
                    auto* node = it->second.head.get();
                    while (node && node->stamp > boundary_) node = node->previous.get();
                    if (node && node->value) callback(it->first, *node->value, node->stamp);
                }
            }
        private:
            friend class store;
            snapshot(const store& owner, timestamp boundary) noexcept : owner_{&owner}, boundary_{boundary} {}
            void reset() noexcept {
                if (!owner_) return;
                std::unique_lock lock{owner_->mutex_};
                auto it = std::find(owner_->active_snapshots_.begin(), owner_->active_snapshots_.end(), boundary_);
                if (it != owner_->active_snapshots_.end()) owner_->active_snapshots_.erase(it);
                owner_ = nullptr;
            }
            const store* owner_{};
            timestamp boundary_{};
        };

        class transaction {
        public:
            transaction(const transaction&) = delete;
            transaction& operator=(const transaction&) = delete;
            transaction(transaction&&) noexcept = default;
            transaction& operator=(transaction&&) noexcept = default;
            [[nodiscard]] result<Value> get(const Key& key) {
                for (auto it = mutations_.rbegin(); it != mutations_.rend(); ++it) {
                    if (equivalent(it->key, key)) return it->value ? result<Value>{*it->value} : std::unexpected(error::not_found);
                }
                std::shared_lock lock{owner_->mutex_};
                observe_locked(key);
                return owner_->read_at_locked(key, snapshot_.boundary_);
            }
            transaction& put(Key key, Value value) { mutations_.push_back({std::move(key), std::move(value)}); return *this; }
            transaction& erase(Key key) { mutations_.push_back({std::move(key), std::nullopt}); return *this; }
            [[nodiscard]] result<timestamp> commit() {
                if (!owner_) return std::unexpected(error::invalid_snapshot);
                if (finished_) return std::unexpected(error::transaction_finished);
                std::unique_lock lock{owner_->mutex_};
                for (const auto& write : mutations_)
                    if (owner_->head_stamp_locked(write.key) > snapshot_.boundary_) return std::unexpected(error::conflict);
                if constexpr (ConflictPolicy::validate_reads)
                    for (const auto& read : reads_)
                        if (owner_->head_stamp_locked(read.key) != read.head_stamp) return std::unexpected(error::conflict);

                std::vector<std::unique_ptr<version_node>> prepared;
                std::vector<Key> inserted;
                try {
                    prepared.reserve(mutations_.size());
                    inserted.reserve(mutations_.size());
                    for (const auto& write : mutations_) prepared.push_back(std::make_unique<version_node>(0, write.value));
                    for (const auto& write : mutations_) {
                        if (owner_->index_.find(write.key) == owner_->index_.end()) {
                            inserted.push_back(write.key);
                            owner_->index_.insert_or_assign(write.key, chain{});
                        }
                    }
                } catch (...) {
                    for (const auto& key : inserted) owner_->index_.erase(key);
                    return std::unexpected(error::allocation_failure);
                }
                const auto committed = owner_->clock_.next();
                for (std::size_t i = 0; i < mutations_.size(); ++i) {
                    auto it = owner_->index_.find(mutations_[i].key);
                    prepared[i]->stamp = committed;
                    prepared[i]->previous = std::move(it->second.head);
                    it->second.head = std::move(prepared[i]);
                }
                finished_ = true;
                return committed;
            }
            void abort() noexcept { mutations_.clear(); reads_.clear(); finished_ = true; }
        private:
            friend class store;
            transaction(store& owner, snapshot snap) : owner_{&owner}, snapshot_{std::move(snap)} {}
            [[nodiscard]] bool equivalent(const Key& left, const Key& right) const {
                return !owner_->compare_(left, right) && !owner_->compare_(right, left);
            }
            void observe_locked(const Key& key) {
                if (std::ranges::any_of(reads_, [this, &key](const observed_key& read) { return equivalent(read.key, key); })) return;
                reads_.push_back({key, owner_->head_stamp_locked(key)});
            }
            store* owner_{};
            snapshot snapshot_;
            std::vector<write> mutations_;
            std::vector<observed_key> reads_;
            bool finished_{};
        };

    private:
        [[nodiscard]] result<Value> read_at_locked(const Key& key, timestamp boundary) const {
            const auto it = index_.find(key);
            if (it == index_.end()) return std::unexpected(error::not_found);
            auto* node = it->second.head.get();
            while (node && node->stamp > boundary) node = node->previous.get();
            if (!node || !node->value) return std::unexpected(error::not_found);
            return *node->value;
        }
        [[nodiscard]] result<void> publish_locked(const std::vector<write>& writes, const timestamp committed) {
            std::vector<std::unique_ptr<version_node>> prepared;
            std::vector<Key> inserted;
            try {
                prepared.reserve(writes.size());
                inserted.reserve(writes.size());
                for (const auto& write : writes) prepared.push_back(std::make_unique<version_node>(committed, write.value));
                for (const auto& write : writes) {
                    if (index_.find(write.key) == index_.end()) {
                        inserted.push_back(write.key);
                        index_.insert_or_assign(write.key, chain{});
                    }
                }
            } catch (...) {
                for (const auto& key : inserted) index_.erase(key);
                return std::unexpected(error::allocation_failure);
            }
            for (std::size_t i = 0; i < writes.size(); ++i) {
                auto it = index_.find(writes[i].key);
                prepared[i]->previous = std::move(it->second.head);
                it->second.head = std::move(prepared[i]);
            }
            return {};
        }
        [[nodiscard]] timestamp head_stamp_locked(const Key& key) const {
            const auto it = index_.find(key);
            return it == index_.end() || !it->second.head ? 0 : it->second.head->stamp;
        }
        [[nodiscard]] std::optional<timestamp> oldest_active_snapshot_locked() const {
            if (active_snapshots_.empty()) return std::nullopt;
            return *std::min_element(active_snapshots_.begin(), active_snapshots_.end());
        }
        mutable std::shared_mutex mutex_;
        mutable Clock clock_{};
        [[no_unique_address]] Compare compare_{};
        index_type index_{};
        mutable std::vector<timestamp> active_snapshots_;
    };
} // namespace anukrama
