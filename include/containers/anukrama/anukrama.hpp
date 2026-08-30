#pragma once
// Header-only static-composition MVCC substrate. No virtual dispatch or public macros.

#include "containers/associative/SkipList.hpp"
#include "mem/smriti.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <shared_mutex>
#include <utility>
#include <vector>

namespace anukrama {
    // The store clock defines the timestamp domain; the free alias tracks the
    // default clock so the enum/result signatures below stay stable.
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
        using timestamp_type = timestamp;
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
        typename Index::value_type;
        { index.find(key) };
        { constant.find(key) };
        { index.lower_bound(key) };
        { index.begin() };
        { constant.begin() };
        { index.end() };
        { constant.end() };
        { (*index.begin()).first };
        { (*index.begin()).second };
        index.insert_or_assign(std::move(owned_key), std::move(chain));
        { index.erase(key) } -> std::convertible_to<bool>;
    };

    // ------------------------------------------------------------------------
    // NodeAllocatorPolicy — the version-node lifetime seam.
    //
    // A pool hands out and reclaims raw Node storage; the store wraps each node
    // in a unique_ptr carrying a deleter that routes back to the owning pool, so
    // chain teardown stays RAII-automatic while the allocation source is pluggable.
    // ------------------------------------------------------------------------
    template <class Pool, class Node, class... Args>
    concept version_node_pool = requires(Pool pool, Node* node, Args&&... args) {
        { pool.allocate(std::forward<Args>(args)...) } -> std::same_as<Node*>;
        { pool.deallocate(node) } noexcept;
        { pool.reset() } noexcept;
    };

    // Default policy: behaviour-identical to std::make_unique<Node> — plain new/delete.
    template <class Node>
    struct heap_node_pool {
        template <class... Args>
        [[nodiscard]] Node* allocate(Args&&... args) { return new Node(std::forward<Args>(args)...); }
        void deallocate(Node* node) noexcept { delete node; }
        void reset() noexcept {}
    };

    // Arena policy: fixed-size version_node slabs drawn from a Smriti FixedPool with a
    // lock-free free-list, so prune()/deallocate recycle nodes rather than returning
    // them to the OS. reset() releases every slab at once.
    template <class Node, class Domain = smriti::domains::SystemRAMDomain>
    class basic_smriti_node_pool {
        static constexpr std::size_t block_size = sizeof(Node) < sizeof(void*) ? sizeof(void*) : sizeof(Node);
        smriti::pools::FixedPool<block_size, Domain> pool_;
    public:
        explicit basic_smriti_node_pool(std::size_t nodes_per_slab = 256) : pool_{nodes_per_slab} {}
        template <class... Args>
        [[nodiscard]] Node* allocate(Args&&... args) {
            void* raw = pool_.allocate(block_size, alignof(Node));
            if (!raw) throw std::bad_alloc{};
            return new (raw) Node(std::forward<Args>(args)...);
        }
        void deallocate(Node* node) noexcept {
            if (!node) return;
            node->~Node();
            pool_.deallocate(node, block_size);
        }
        void reset() noexcept { pool_.reset(); }
    };

    // Single-parameter alias for the store's `template <class> class` slot (no reliance
    // on relaxed template-template matching). For a custom domain, declare your own
    // one-argument alias over basic_smriti_node_pool.
    template <class Node>
    using smriti_node_pool = basic_smriti_node_pool<Node, smriti::domains::SystemRAMDomain>;

    // ------------------------------------------------------------------------
    // SynchronizationPolicy — the locking seam.
    //
    // read_lock() guards a snapshot read; write_lock(key) and commit_lock(write_set)
    // guard mutation. Guards are RAII scoped objects the store holds for the critical
    // section. Policies decide the granularity.
    // ------------------------------------------------------------------------
    // Today's exact behaviour: one shared_mutex over the whole store.
    class global_shared_lock {
        mutable std::shared_mutex mutex_;
    public:
        [[nodiscard]] std::shared_lock<std::shared_mutex> read_lock() const { return std::shared_lock{mutex_}; }
        template <class Key>
        [[nodiscard]] std::unique_lock<std::shared_mutex> write_lock(const Key&) const { return std::unique_lock{mutex_}; }
        template <class WriteSet>
        [[nodiscard]] std::unique_lock<std::shared_mutex> commit_lock(const WriteSet&) const { return std::unique_lock{mutex_}; }
    };

    // Single-threaded tier: zero synchronisation overhead. Guards are empty objects.
    class null_lock {
        struct empty_guard {};
    public:
        [[nodiscard]] empty_guard read_lock() const noexcept { return {}; }
        template <class Key>
        [[nodiscard]] empty_guard write_lock(const Key&) const noexcept { return {}; }
        template <class WriteSet>
        [[nodiscard]] empty_guard commit_lock(const WriteSet&) const noexcept { return {}; }
    };

    // N-way key-hash striping: disjoint keys commit concurrently. read_lock holds
    // every stripe shared (a reader may touch any key); write/commit acquire only the
    // stripes their keys hash to, in ascending stripe order to avoid deadlock.
    // Transparent hasher: dispatches to std::hash<Key> per call so striped_lock
    // need not know the key type up front.
    struct default_stripe_hash {
        template <class Key>
        [[nodiscard]] std::size_t operator()(const Key& key) const noexcept { return std::hash<Key>{}(key); }
    };

    template <std::size_t N = 64, class Hash = default_stripe_hash>
    class striped_lock {
        static_assert(N > 0 && (N & (N - 1)) == 0, "striped_lock stripe count must be a power of two");
        mutable std::array<std::shared_mutex, N> stripes_;

        template <class Key, class KeyHash>
        [[nodiscard]] static std::size_t stripe_of(const Key& key, const KeyHash& hash) noexcept {
            return hash(key) & (N - 1);
        }

        // RAII guard over an ordered, deduplicated set of exclusive stripe locks.
        class multi_guard {
            std::array<std::shared_mutex, N>* stripes_{};
            std::array<bool, N> held_{};
        public:
            multi_guard() = default;
            explicit multi_guard(std::array<std::shared_mutex, N>& stripes) : stripes_{&stripes} {}
            void acquire(std::size_t index) { if (!held_[index]) { stripes_->operator[](index).lock(); held_[index] = true; } }
            multi_guard(const multi_guard&) = delete;
            multi_guard& operator=(const multi_guard&) = delete;
            multi_guard(multi_guard&& other) noexcept : stripes_{std::exchange(other.stripes_, nullptr)}, held_{other.held_} { other.held_.fill(false); }
            multi_guard& operator=(multi_guard&&) = delete;
            ~multi_guard() { if (stripes_) for (std::size_t i = N; i-- > 0;) if (held_[i]) stripes_->operator[](i).unlock(); }
        };

        // RAII guard holding every stripe shared, for whole-store reads.
        class shared_all_guard {
            std::array<std::shared_mutex, N>* stripes_{};
        public:
            shared_all_guard() = default;
            explicit shared_all_guard(std::array<std::shared_mutex, N>& stripes) : stripes_{&stripes} {
                for (auto& stripe : *stripes_) stripe.lock_shared();
            }
            shared_all_guard(const shared_all_guard&) = delete;
            shared_all_guard& operator=(const shared_all_guard&) = delete;
            shared_all_guard(shared_all_guard&& other) noexcept : stripes_{std::exchange(other.stripes_, nullptr)} {}
            shared_all_guard& operator=(shared_all_guard&&) = delete;
            ~shared_all_guard() { if (stripes_) for (std::size_t i = N; i-- > 0;) stripes_->operator[](i).unlock_shared(); }
        };

    public:
        [[nodiscard]] shared_all_guard read_lock() const { return shared_all_guard{stripes_}; }

        template <class Key>
        [[nodiscard]] multi_guard write_lock(const Key& key) const {
            multi_guard guard{stripes_};
            guard.acquire(stripe_of(key, Hash{}));
            return guard;
        }

        // WriteSet is any range of elements exposing a `.key`. Acquire the touched
        // stripes in ascending index order (total order → deadlock-free).
        template <class WriteSet>
        [[nodiscard]] multi_guard commit_lock(const WriteSet& writes) const {
            std::array<bool, N> wanted{};
            for (const auto& w : writes) wanted[stripe_of(w.key, Hash{})] = true;
            multi_guard guard{stripes_};
            for (std::size_t i = 0; i < N; ++i) if (wanted[i]) guard.acquire(i);
            return guard;
        }
    };

    template <class Sync>
    concept store_sync = requires(const Sync sync, int key, std::vector<int> writes) {
        sync.read_lock();
        sync.write_lock(key);
        sync.commit_lock(writes);
    };

    // ------------------------------------------------------------------------
    // SnapshotRegistryPolicy — the active-snapshot bookkeeping seam.
    //
    // Tracks the timestamps of live snapshots so GC can retain the oldest still
    // visible. Needs O(log k) insert/erase and O(1) minimum. Duplicate timestamps
    // are legal (two snapshots opened at the same commit boundary).
    // ------------------------------------------------------------------------
    template <class Registry>
    concept snapshot_registry = requires(Registry registry, const Registry constant, timestamp value) {
        registry.insert(value);
        registry.erase(value);
        { constant.min() } -> std::convertible_to<std::optional<timestamp>>;
    };

    // Default: multiset gives O(log k) insert/erase, O(1) begin(), and duplicate keys.
    class multiset_snapshot_registry {
        std::multiset<timestamp> active_;
    public:
        void insert(const timestamp value) { active_.insert(value); }
        void erase(const timestamp value) { if (auto it = active_.find(value); it != active_.end()) active_.erase(it); }
        [[nodiscard]] std::optional<timestamp> min() const {
            return active_.empty() ? std::nullopt : std::optional{*active_.begin()};
        }
    };

    template <class Key, class Value, class Compare = std::less<>,
              template <class, class, class> class IndexPolicy = skip_list_index,
              externally_advanceable_clock Clock = atomic_clock,
              conflict_policy ConflictPolicy = snapshot_isolation,
              template <class> class NodeAllocatorPolicy = heap_node_pool,
              store_sync SynchronizationPolicy = global_shared_lock,
              snapshot_registry SnapshotRegistryPolicy = multiset_snapshot_registry>
        requires std::copy_constructible<Key> && std::copy_constructible<Value>
    class store {
        struct version_node;
        using node_pool = NodeAllocatorPolicy<version_node>;

        // Deleter routes reclamation back to the store-owned pool, so chains stay
        // owning unique_ptr yet allocate/free through the policy.
        struct node_deleter {
            node_pool* pool{};
            void operator()(version_node* node) const noexcept { if (pool) pool->deallocate(node); }
        };
        using node_ptr = std::unique_ptr<version_node, node_deleter>;

        struct version_node {
            timestamp stamp{};
            std::optional<Value> value{};
            node_ptr previous{};
            version_node(timestamp version, std::optional<Value> payload)
                : stamp{version}, value{std::move(payload)} {}
        };
        static_assert(version_node_pool<node_pool, version_node, timestamp, std::optional<Value>>);
        struct chain { node_ptr head{}; };
        using index_type = IndexPolicy<Key, chain, Compare>;
        static_assert(ordered_version_index<index_type, Key, chain>,
                      "IndexPolicy must model ordered_version_index: find/lower_bound/begin/end, "
                      "insert_or_assign, erase, and a value_type with .first/.second");
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
            // A pure-read boundary capture: only the registry mutex, never the store lock.
            std::scoped_lock registry_guard{registry_mutex_};
            const auto boundary = clock_.now();
            registry_.insert(boundary);
            return snapshot{*this, boundary};
        }
        [[nodiscard]] transaction begin() { return transaction{*this, snapshot_at_current()}; }
        [[nodiscard]] result<Value> get(const Key& key) const {
            auto guard = sync_.read_lock();
            return read_at_locked(key, clock_.now());
        }
        [[nodiscard]] result<Value> get_at(const Key& key, const timestamp boundary) const {
            auto guard = sync_.read_lock();
            return read_at_locked(key, boundary);
        }
        [[nodiscard]] timestamp version_of(const Key& key) const {
            auto guard = sync_.read_lock();
            return head_stamp_locked(key);
        }
        [[nodiscard]] timestamp version_at(const Key& key, const timestamp boundary) const {
            auto guard = sync_.read_lock();
            const auto it = index_.find(key);
            if (it == index_.end()) return 0;
            auto* node = it->second.head.get();
            while (node && node->stamp > boundary) node = node->previous.get();
            return node ? node->stamp : 0;
        }
        [[nodiscard]] bool contains(const Key& key) const { return get(key).has_value(); }
        [[nodiscard]] std::size_t size() const { return live_keys_.load(std::memory_order_acquire); }

        // For a durability layer that already owns ordering (for example a
        // Nitya WAL record), install a fully prepared batch at its commit LSN.
        [[nodiscard]] result<void> apply_at(const std::vector<write>& writes, const timestamp committed) {
            auto guard = sync_.commit_lock(writes);
            if (committed <= clock_.now()) return std::unexpected(error::stale_timestamp);
            if (auto applied = publish_writes(writes, committed); !applied) return applied;
            (void)clock_.advance_to(committed);
            return {};
        }

        // Atomically validates all observed point versions and publishes the
        // complete write batch under the same writer lock. This is the bridge
        // used by external transaction coordinators such as Medha.
        [[nodiscard]] result<timestamp> commit_if_unchanged(
            const std::vector<observation>& observed,
            const std::vector<write>& writes) {
            auto guard = sync_.commit_lock(writes);
            for (const auto& read : observed) {
                if (head_stamp_locked(read.key) != read.version)
                    return std::unexpected(error::conflict);
            }
            const auto committed = clock_.next();
            if (auto applied = publish_writes(writes, committed); !applied)
                return std::unexpected(applied.error());
            return committed;
        }

        // Explicit reclamation: retain the latest version visible to every live snapshot.
        void prune() {
            std::vector<write> nothing;
            auto guard = sync_.commit_lock(nothing);
            const auto boundary = oldest_active_snapshot().value_or(clock_.now());
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
                auto guard = owner_->sync_.read_lock();
                return owner_->read_at_locked(key, boundary_);
            }
            template <class Callback>
            void scan(const Key& first, const Key& last, Callback&& callback) const {
                if (!owner_) return;
                auto guard = owner_->sync_.read_lock();
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
                auto guard = owner_->sync_.read_lock();
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
                std::scoped_lock registry_guard{owner_->registry_mutex_};
                owner_->registry_.erase(boundary_);
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
                auto guard = owner_->sync_.read_lock();
                observe_locked(key);
                return owner_->read_at_locked(key, snapshot_.boundary_);
            }
            transaction& put(Key key, Value value) { mutations_.push_back({std::move(key), std::move(value)}); return *this; }
            transaction& erase(Key key) { mutations_.push_back({std::move(key), std::nullopt}); return *this; }
            [[nodiscard]] result<timestamp> commit() {
                if (!owner_) return std::unexpected(error::invalid_snapshot);
                if (finished_) return std::unexpected(error::transaction_finished);
                auto guard = owner_->sync_.commit_lock(mutations_);
                for (const auto& write : mutations_)
                    if (owner_->head_stamp_locked(write.key) > snapshot_.boundary_) return std::unexpected(error::conflict);
                if constexpr (ConflictPolicy::validate_reads)
                    for (const auto& read : reads_)
                        if (owner_->head_stamp_locked(read.key) != read.head_stamp) return std::unexpected(error::conflict);

                const auto committed = owner_->clock_.next();
                if (auto applied = owner_->publish_writes(mutations_, committed); !applied)
                    return std::unexpected(applied.error());
                finished_ = true;
                return committed;
            }
            void abort() noexcept { mutations_.clear(); reads_.clear(); finished_ = true; }
            // Lift the accumulated optimistic read-set as OCC tokens for an external
            // coordinator, without re-deriving versions. Valid until the next get().
            [[nodiscard]] std::vector<observation> read_set() const {
                std::vector<observation> tokens;
                tokens.reserve(reads_.size());
                for (const auto& read : reads_) tokens.push_back({read.key, read.head_stamp});
                return tokens;
            }
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

        // The single publish path shared by transaction::commit, apply_at, and
        // commit_if_unchanged. Allocates every node through the pool, links the
        // chains newest→oldest, and maintains the O(1) live-key counter. Must be
        // called under a commit guard.
        template <class WriteSet>
        [[nodiscard]] result<void> publish_writes(const WriteSet& writes, const timestamp committed) {
            std::vector<node_ptr> prepared;
            std::vector<Key> inserted;
            try {
                prepared.reserve(writes.size());
                inserted.reserve(writes.size());
                for (const auto& write : writes) prepared.push_back(make_node(committed, write.value));
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
            std::size_t index = 0;
            for (const auto& write : writes) {
                auto it = index_.find(write.key);
                const bool was_live = it->second.head && it->second.head->value.has_value();
                const bool now_live = prepared[index]->value.has_value();
                prepared[index]->previous = std::move(it->second.head);
                it->second.head = std::move(prepared[index]);
                if (now_live && !was_live) live_keys_.fetch_add(1, std::memory_order_acq_rel);
                else if (!now_live && was_live) live_keys_.fetch_sub(1, std::memory_order_acq_rel);
                ++index;
            }
            return {};
        }

        template <class... Args>
        [[nodiscard]] node_ptr make_node(Args&&... args) {
            return node_ptr{node_pool_.allocate(std::forward<Args>(args)...), node_deleter{&node_pool_}};
        }

        [[nodiscard]] timestamp head_stamp_locked(const Key& key) const {
            const auto it = index_.find(key);
            return it == index_.end() || !it->second.head ? 0 : it->second.head->stamp;
        }
        [[nodiscard]] std::optional<timestamp> oldest_active_snapshot() const {
            std::scoped_lock registry_guard{registry_mutex_};
            return registry_.min();
        }
        mutable SynchronizationPolicy sync_{};
        mutable Clock clock_{};
        [[no_unique_address]] Compare compare_{};
        mutable node_pool node_pool_{};
        index_type index_{};
        std::atomic<std::size_t> live_keys_{0};
        mutable std::mutex registry_mutex_;
        mutable SnapshotRegistryPolicy registry_{};
    };
} // namespace anukrama
