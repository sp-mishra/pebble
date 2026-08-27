#pragma once
// ============================================================================
// kosha.hpp — High-Performance Cache Library
// ============================================================================
//
// C++23, header-only, single-file.
//
// Namespace layout
// ----------------
//   kosha::core      — zero-overhead local cache kernel
//   kosha::adapter   — optional wrappers (threading, sharding, metrics, TTL)
//   kosha::cluster   — distributed cache skeleton (no-op by default, zero cost)
//   kosha::          — backward-compatible convenience aliases
//
// Core (kosha::core)
//   Cache<K,V,Policy,Storage>   — single-threaded primitive; zero overhead
//
// Eviction policies
//   LRUPolicy<K>    — Least Recently Used        mutates_on_hit = true
//   LFUPolicy<K>    — Least Frequently Used      mutates_on_hit = true
//   FIFOPolicy<K>   — First In, First Out        mutates_on_hit = false
//   ARCPolicy<K>    — Adaptive Replacement Cache  mutates_on_hit = true
//
// Storage backends
//   FlatHashStorage<K,V,...>  — Robin-Hood open-addressing, cache-line-friendly
//   NodeStorage<K,V,...>      — std::pmr::unordered_map (pointer-stable nodes)
//
// Adapters (kosha::adapter)
//   ThreadSafeCache<Cache>        — shared_mutex wrapper; lock grade at compile time
//   ShardedCache<Cache, N>        — hash-striped false-sharing-free shards
//   InstrumentedCache<Cache>      — hit/miss/eviction counters; zero cost if unused
//   TTLCache<Cache>               — per-entry expiry; lazy eviction on access
//
// Distributed skeleton (kosha::cluster)
//   ClusterCache<Local,Router,Transport,Serializer,Replication,Consistency,Membership>
//   All policy slots default to no-ops — [[no_unique_address]] keeps object size zero.
//   None of the distributed code is compiled unless a non-default policy is used.
//
// Error propagation
//   All fallible operations return std::expected<T, kosha::core::Error> or
//   std::optional<T>. No exceptions on the hot path.
//
// Allocator
//   std::pmr::polymorphic_allocator by default. Pass a monotonic_buffer_resource
//   or pool_resource to bind allocations to a pre-sized arena.
//
// Quick start
//   kosha::LRUCache<std::string, int> c{256};
//   c.put("x", 1);
//   if (auto v = c.get("x")) { use(*v); }
//
//   kosha::InstrumentedLRUCache<int, int> ic{256};
//   ic.put(1, 10); ic.get(1);
//   ic.hit_rate(); // 1.0
//
//   kosha::TTLLRUCache<int, int> tc{256};
//   tc.put(1, 10, std::chrono::seconds{5});
//
//   kosha::cluster::ClusterCache<kosha::LRUCache<int,int>> cc{256}; // local, zero cost
// ============================================================================

#include <memory>
#include <memory_resource>
#include <new>
#include <utility>
#include <concepts>
#include <type_traits>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cassert>
#include <chrono>
#include <deque>
#include <expected>
#include <functional>
#include <list>
#include <map>
#include <mutex>
#include <optional>
#include <ranges>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// ============================================================================
// kosha::core — zero-overhead local cache kernel
// ============================================================================
namespace kosha::core {
    // ============================================================================
    // § 1  Error type
    // ============================================================================

    enum class Error : std::uint8_t {
        NotFound, // key absent during get / peek / erase
        Capacity, // put() when full and policy cannot evict
        InvalidArg, // e.g. zero-capacity construction
    };

    // ============================================================================
    // § 2  Concepts
    // ============================================================================

    template <typename H>
    concept TransparentHash = requires { typename H::is_transparent; };

    template <typename E>
    concept TransparentEqual = requires { typename E::is_transparent; };

    template <typename P, typename K>
    concept EvictionPolicy = requires(P p, const K& k) {
        { p.on_insert(k) } -> std::same_as<void>;
        { p.on_hit(k) } -> std::same_as<void>;
        { p.on_miss(k) } -> std::same_as<void>;
        { p.on_erase(k) } -> std::same_as<void>;
        { p.evict() } -> std::same_as<std::optional<K>>;
        { p.clear() } -> std::same_as<void>;
        { p.size() } -> std::convertible_to<std::size_t>;
    };

    template <typename S, typename K, typename V>
    concept StorageBackend = requires(S s, const K& k, V v) {
        { s.find(k) } -> std::same_as<V*>;
        { std::as_const(s).find(k) } -> std::same_as<const V*>;
        { s.insert(k, std::move(v)) } -> std::same_as<bool>;
        { s.insert_or_assign(k, std::move(v)) } -> std::same_as<void>;
        { s.erase(k) } -> std::same_as<bool>;
        { s.clear() } -> std::same_as<void>;
        { s.size() } -> std::convertible_to<std::size_t>;
    };

    // ============================================================================
    // § 3  Policy traits
    // ============================================================================

    template <typename P>
    inline constexpr bool mutates_on_hit = true;

    template <typename K>
    class FIFOPolicy; // forward decl for trait specialization

    template <typename K>
    inline constexpr bool mutates_on_hit<FIFOPolicy<K>> = false;

    // HasTransactionalInsert: policies that own their own eviction+insertion
    // transaction (ARC). Cache::put branches on this at compile time.
    template <typename P, typename K>
    concept HasTransactionalInsert = requires(P p, const K& k, std::size_t n) {
        { p.prepare_insert(k, n) } -> std::same_as<std::pair<std::optional<K>, bool>>;
        { p.commit_insert(k, bool{}) } -> std::same_as<void>;
    };

    // ============================================================================
    // § 4  Eviction policies
    // ============================================================================

    // ----------------------------------------------------------------------------
    // LRUPolicy<Key>
    // ----------------------------------------------------------------------------
    template <typename Key>
    class LRUPolicy {
    public:
        explicit LRUPolicy(const std::size_t capacity) noexcept : cap_{capacity} {}

        void on_insert(const Key& k) {
            order_.push_front(k);
            index_[k] = order_.begin();
        }

        void on_hit(const Key& k) {
            if (auto it = index_.find(k); it != index_.end())
                order_.splice(order_.begin(), order_, it->second);
        }

        void on_miss(const Key&) noexcept {}

        void on_erase(const Key& k) {
            if (auto it = index_.find(k); it != index_.end()) {
                order_.erase(it->second);
                index_.erase(it);
            }
        }

        std::optional<Key> evict() {
            if (order_.empty()) return std::nullopt;
            Key victim = order_.back();
            order_.pop_back();
            index_.erase(victim);
            return victim;
        }

        void clear() noexcept {
            order_.clear();
            index_.clear();
        }

        [[nodiscard]] std::size_t size() const noexcept { return order_.size(); }
        [[nodiscard]] std::size_t capacity() const noexcept { return cap_; }

    private:
        std::size_t cap_;
        std::list<Key> order_;
        std::unordered_map<Key, typename std::list<Key>::iterator> index_;
    };

    // ----------------------------------------------------------------------------
    // LFUPolicy<Key>
    // O(1) amortised LFU with per-frequency LRU tie-break.
    // ----------------------------------------------------------------------------
    template <typename Key>
    class LFUPolicy {
    public:
        explicit LFUPolicy(const std::size_t capacity) noexcept : cap_{capacity} {}

        void on_insert(const Key& k) {
            freq_[k] = 1;
            buckets_[1].push_front(k);
            pos_[k] = buckets_[1].begin();
            if (min_freq_ > 1) min_freq_ = 1;
        }

        void on_hit(const Key& k) {
            auto fit = freq_.find(k);
            if (fit == freq_.end()) return;
            promote(k, fit->second);
        }

        void on_miss(const Key&) noexcept {}

        void on_erase(const Key& k) {
            auto fit = freq_.find(k);
            if (fit == freq_.end()) return;
            remove_from_bucket(k, fit->second);
            freq_.erase(fit);
            pos_.erase(k);
        }

        std::optional<Key> evict() {
            auto bit = buckets_.find(min_freq_);
            while (bit != buckets_.end() && bit->second.empty())
                bit = buckets_.erase(bit);
            if (bit == buckets_.end()) return std::nullopt;

            Key victim = bit->second.back();
            bit->second.pop_back();
            if (bit->second.empty()) buckets_.erase(bit);
            freq_.erase(victim);
            pos_.erase(victim);
            return victim;
        }

        void clear() noexcept {
            freq_.clear();
            buckets_.clear();
            pos_.clear();
            min_freq_ = 1;
        }

        [[nodiscard]] std::size_t size() const noexcept { return freq_.size(); }
        [[nodiscard]] std::size_t capacity() const noexcept { return cap_; }

    private:
        using ListT = std::list<Key>;
        using IterT = ListT::iterator;

        std::size_t cap_;
        std::size_t min_freq_{1};
        std::unordered_map<Key, std::size_t> freq_;
        std::map<std::size_t, ListT> buckets_;
        std::unordered_map<Key, IterT> pos_;

        void remove_from_bucket(const Key& k, std::size_t f) {
            auto bit = buckets_.find(f);
            if (bit == buckets_.end()) return;
            auto pit = pos_.find(k);
            if (pit == pos_.end()) return;
            bit->second.erase(pit->second);
            if (bit->second.empty()) buckets_.erase(bit);
        }

        void promote(const Key& k, std::size_t f) {
            remove_from_bucket(k, f);
            const std::size_t nf = f + 1;
            freq_[k] = nf;
            buckets_[nf].push_front(k);
            pos_[k] = buckets_[nf].begin();
            if (f == min_freq_ && buckets_.find(f) == buckets_.end())
                min_freq_ = nf;
        }
    };

    // ----------------------------------------------------------------------------
    // FIFOPolicy<Key>
    // mutates_on_hit = false — get() is as safe as peek() for lock purposes.
    // ----------------------------------------------------------------------------
    template <typename Key>
    class FIFOPolicy {
    public:
        explicit FIFOPolicy(const std::size_t capacity) noexcept : cap_{capacity} {}

        void on_insert(const Key& k) {
            queue_.push_back(k);
            present_.insert(k);
        }

        void on_hit(const Key&) noexcept {}

        void on_miss(const Key&) noexcept {}

        void on_erase(const Key& k) { present_.erase(k); }

        std::optional<Key> evict() {
            while (!queue_.empty()) {
                Key c = queue_.front();
                queue_.pop_front();
                if (present_.count(c)) {
                    present_.erase(c);
                    return c;
                }
            }
            return std::nullopt;
        }

        void clear() noexcept {
            queue_.clear();
            present_.clear();
        }

        [[nodiscard]] std::size_t size() const noexcept { return present_.size(); }
        [[nodiscard]] std::size_t capacity() const noexcept { return cap_; }

    private:
        std::size_t cap_;
        std::deque<Key> queue_;
        std::unordered_set<Key> present_;
    };

    // ----------------------------------------------------------------------------
    // ARCPolicy<Key>
    // Adaptive Replacement Cache with O(1) unified index over T1/T2/B1/B2.
    //
    // Uses the HasTransactionalInsert protocol: Cache::put calls
    // prepare_insert() + commit_insert() instead of evict() + on_insert().
    // This keeps eviction and insertion as one atomic transaction, which is
    // required for ARC correctness.
    // ----------------------------------------------------------------------------
    template <typename Key>
    class ARCPolicy {
    public:
        enum class ListId : std::uint8_t { T1, T2, B1, B2 };

        explicit ARCPolicy(const std::size_t capacity) noexcept : cap_{capacity} {}

        // Phase 1: decide victim (if any) and target list for the incoming key.
        // Returns {storage_key_to_erase, target_is_t2}.
        std::pair<std::optional<Key>, bool>
        prepare_insert(const Key& k, const std::size_t current_size) {
            auto it = idx_.find(k);

            if (it != idx_.end() && it->second.list_id == ListId::B1) {
                const std::size_t delta = (b1_.size() >= b2_.size())
                                              ? 1
                                              : b2_.size() / b1_.size();
                p_ = std::min(p_ + delta, cap_);
                return {replace(false), true};
            }

            if (it != idx_.end() && it->second.list_id == ListId::B2) {
                const std::size_t delta = (b2_.size() >= b1_.size())
                                              ? 1
                                              : b1_.size() / b2_.size();
                p_ = (p_ >= delta) ? p_ - delta : 0;
                return {replace(true), true};
            }

            // Pure miss.
            std::optional<Key> victim;
            if (current_size >= cap_) victim = replace(false);
            return {victim, false};
        }

        // Phase 2: commit key into T1 or T2, trim ghost lists.
        void commit_insert(const Key& k, const bool target_t2) {
            // If key was in a ghost list (B1/B2 hit), remove it first — O(1).
            if (auto it = idx_.find(k); it != idx_.end()) {
                auto& ghost = (it->second.list_id == ListId::B1) ? b1_ : b2_;
                ghost.erase(it->second.node_it);
                idx_.erase(it);
            }
            if (target_t2) {
                t2_.push_front(k);
                idx_[k] = {ListId::T2, t2_.begin()};
            }
            else {
                t1_.push_front(k);
                idx_[k] = {ListId::T1, t1_.begin()};
            }
            trim_ghosts();
        }

        void on_hit(const Key& k) {
            auto it = idx_.find(k);
            if (it == idx_.end()) return;
            if (it->second.list_id == ListId::T1) {
                t1_.erase(it->second.node_it);
                t2_.push_front(k);
                it->second = {ListId::T2, t2_.begin()};
            }
            else if (it->second.list_id == ListId::T2) {
                t2_.splice(t2_.begin(), t2_, it->second.node_it);
                it->second.node_it = t2_.begin();
            }
        }

        void on_insert(const Key&) noexcept {} // no-op; commit_insert handles placement
        void on_miss(const Key&) noexcept {}

        std::optional<Key> evict() noexcept { return std::nullopt; } // transactional path owns eviction

        void on_erase(const Key& k) {
            auto it = idx_.find(k);
            if (it == idx_.end()) return;
            switch (it->second.list_id) {
            case ListId::T1: t1_.erase(it->second.node_it);
                break;
            case ListId::T2: t2_.erase(it->second.node_it);
                break;
            case ListId::B1: b1_.erase(it->second.node_it);
                break;
            case ListId::B2: b2_.erase(it->second.node_it);
                break;
            default: assert(false && "ARCPolicy: invalid list_id in index");
            }
            idx_.erase(it);
        }

        void clear() noexcept {
            t1_.clear();
            t2_.clear();
            b1_.clear();
            b2_.clear();
            idx_.clear();
            p_ = 0;
        }

        [[nodiscard]] std::size_t size() const noexcept { return t1_.size() + t2_.size(); }
        [[nodiscard]] std::size_t capacity() const noexcept { return cap_; }

#ifndef NDEBUG
        void validate(std::size_t storage_size) const {
            assert(t1_.size() + t2_.size() <= cap_);
            assert(b1_.size() + b2_.size() <= cap_);
            assert(t1_.size() + t2_.size() + b1_.size() + b2_.size() <= 2 * cap_);
            assert(t1_.size() + t2_.size() == storage_size);
        }
#endif

    private:
        struct NodeLocation {
            ListId list_id;
            std::list<Key>::iterator node_it;
        };

        std::size_t cap_;
        std::size_t p_{0};
        std::list<Key> t1_, t2_, b1_, b2_;
        std::unordered_map<Key, NodeLocation> idx_;

        // Demote one victim from T1 or T2 into its ghost list. O(1).
        std::optional<Key> replace(bool hit_in_b2) {
            const auto p_floor = p_;
            const bool prefer_t1 =
                !t1_.empty() &&
                (t1_.size() > p_floor ||
                    (hit_in_b2 && t1_.size() == p_floor));
            if (prefer_t1 || t2_.empty()) {
                if (!t1_.empty()) {
                    Key v = t1_.back();
                    t1_.pop_back();
                    b1_.push_front(v);
                    idx_[v] = {ListId::B1, b1_.begin()};
                    return v;
                }
            }
            if (!t2_.empty()) {
                Key v = t2_.back();
                t2_.pop_back();
                b2_.push_front(v);
                idx_[v] = {ListId::B2, b2_.begin()};
                return v;
            }
            return std::nullopt;
        }

        // Enforce |B1|+|B2| ≤ cap_.
        void trim_ghosts() {
            while (b1_.size() + b2_.size() > cap_) {
                const auto p_floor = p_;
                if ((!b1_.empty() && b1_.size() > (cap_ > p_floor ? cap_ - p_floor : 0)) || !b1_.empty()) {
                    idx_.erase(b1_.back());
                    b1_.pop_back();
                }
                else if (!b2_.empty()) {
                    idx_.erase(b2_.back());
                    b2_.pop_back();
                }
                else break;
            }
        }
    };

    // ============================================================================
    // § 5  Storage backends
    // ============================================================================

    namespace detail {
        template <typename Alloc, typename T>
        using rebind_alloc_t =
        std::allocator_traits<Alloc>::template rebind_alloc<T>;
    } // namespace detail

    // ----------------------------------------------------------------------------
    // FlatHashStorage<K,V,Hash,Eq,Alloc>
    // Robin-Hood open-addressing with linear probing + backward-shift deletion.
    // No tombstones. Slots use manual lifetime — K and V need not be
    // default-constructible.
    // ----------------------------------------------------------------------------
    template <
        typename Key,
        typename Value,
        typename Hash = std::hash<Key>,
        typename KeyEq = std::equal_to<Key>,
        typename Alloc = std::pmr::polymorphic_allocator<>>
    class FlatHashStorage {
    public:
        explicit FlatHashStorage(
            const std::size_t initial_cap = 16,
            Hash h = {},
            KeyEq e = {},
            Alloc a = {})
            : hash_{h}, eq_{e}, alloc_{a} {
            alloc_slots(next_pow2(std::max(initial_cap, std::size_t{16})));
        }

        FlatHashStorage(const FlatHashStorage&) = delete;

        FlatHashStorage& operator=(const FlatHashStorage&) = delete;

        FlatHashStorage(FlatHashStorage&& o) noexcept
            : hash_{std::move(o.hash_)}, eq_{std::move(o.eq_)}
              , alloc_{std::move(o.alloc_)}
              , slots_{o.slots_}, cap_{o.cap_}, size_{o.size_} {
            o.slots_ = nullptr;
            o.cap_ = 0;
            o.size_ = 0;
        }

        FlatHashStorage& operator=(FlatHashStorage&& o) noexcept {
            if (this != &o) {
                destroy_slots();
                hash_ = std::move(o.hash_);
                eq_ = std::move(o.eq_);
                alloc_ = std::move(o.alloc_);
                slots_ = o.slots_;
                cap_ = o.cap_;
                size_ = o.size_;
                o.slots_ = nullptr;
                o.cap_ = 0;
                o.size_ = 0;
            }
            return *this;
        }

        ~FlatHashStorage() { destroy_slots(); }

        Value* find(const auto& k) {
            std::size_t pos = probe_find(k);
            return pos == npos ? nullptr : &slots_[pos].val();
        }

        [[nodiscard]] const Value* find(const auto& k) const {
            std::size_t pos = probe_find(k);
            return pos == npos ? nullptr : &slots_[pos].val();
        }

        bool insert(const Key& k, Value v) {
            maybe_grow();
            return robin_insert(k, std::move(v), false);
        }

        void insert_or_assign(const Key& k, Value v) {
            maybe_grow();
            robin_insert(k, std::move(v), true);
        }

        bool erase(const Key& k) {
            std::size_t pos = probe_find(k);
            if (pos == npos) return false;
            slots_[pos].destroy();
            --size_;
            std::size_t cur = pos, next = (cur + 1) & mask();
            while (slots_[next].occupied && slots_[next].dist > 0) {
                slots_[cur].move_from(slots_[next]);
                --slots_[cur].dist;
                slots_[next].destroy();
                cur = next;
                next = (cur + 1) & mask();
            }
            return true;
        }

        void clear() noexcept {
            for (std::size_t i = 0; i < cap_; ++i)
                if (slots_[i].occupied) slots_[i].destroy();
            size_ = 0;
        }

        [[nodiscard]] std::size_t size() const noexcept { return size_; }

        template <typename Fn>
        void for_each(Fn&& fn) {
            for (std::size_t i = 0; i < cap_; ++i)
                if (slots_[i].occupied) fn(slots_[i].key(), slots_[i].val());
        }

        template <typename Fn>
        void for_each(Fn&& fn) const {
            for (std::size_t i = 0; i < cap_; ++i)
                if (slots_[i].occupied) fn(slots_[i].key(), slots_[i].val());
        }

    private:
        struct Slot {
            bool occupied{false};
            std::uint8_t dist{0};
            alignas(Key) unsigned char key_buf[sizeof(Key)]{};
            alignas(Value) unsigned char val_buf[sizeof(Value)]{};

            Key& key() noexcept { return *std::launder(reinterpret_cast<Key*>(key_buf)); }
            const Key& key() const noexcept { return *std::launder(reinterpret_cast<const Key*>(key_buf)); }
            Value& val() noexcept { return *std::launder(reinterpret_cast<Value*>(val_buf)); }
            const Value& val() const noexcept { return *std::launder(reinterpret_cast<const Value*>(val_buf)); }

            template <typename K2, typename V2>
            void construct(K2&& k, V2&& v) {
                new(key_buf) Key(std::forward<K2>(k));
                new(val_buf) Value(std::forward<V2>(v));
                occupied = true;
            }

            template <typename K2, typename V2, typename A>
            void construct(K2&& k, V2&& v, const A& alloc) {
                if constexpr (std::uses_allocator_v<Key, A>)
                    new(key_buf) Key(std::allocator_arg, alloc, std::forward<K2>(k));
                else
                    new(key_buf) Key(std::forward<K2>(k));
                if constexpr (std::uses_allocator_v<Value, A>)
                    new(val_buf) Value(std::allocator_arg, alloc, std::forward<V2>(v));
                else
                    new(val_buf) Value(std::forward<V2>(v));
                occupied = true;
            }

            void destroy() noexcept {
                if (occupied) {
                    key().~Key();
                    val().~Value();
                    occupied = false;
                }
            }

            void move_from(Slot& from) noexcept {
                construct(std::move(from.key()), std::move(from.val()));
                dist = from.dist;
                from.dist = 0;
            }
        };

        static constexpr float kMaxLoad = 0.75f;
        static constexpr std::size_t npos = std::numeric_limits<std::size_t>::max();

        using SlotAlloc = detail::rebind_alloc_t<Alloc, Slot>;

        Hash hash_;
        KeyEq eq_;
        Alloc alloc_;
        Slot* slots_{nullptr};
        std::size_t cap_{0};
        std::size_t size_{0};

        [[nodiscard]] std::size_t mask() const noexcept { return cap_ - 1; }

        [[nodiscard]] float load() const noexcept {
            return cap_ == 0 ? 1.f : static_cast<float>(size_) / static_cast<float>(cap_);
        }

        static std::size_t next_pow2(const std::size_t n) noexcept { return std::bit_ceil(n); }
        std::size_t ideal(const auto& k) const noexcept { return hash_(k) & mask(); }

        void alloc_slots(std::size_t n) {
            SlotAlloc sa{alloc_};
            slots_ = std::allocator_traits<SlotAlloc>::allocate(sa, n);
            cap_ = n;
            for (std::size_t i = 0; i < cap_; ++i) std::construct_at(&slots_[i]);
        }

        void destroy_slots() noexcept {
            if (!slots_) return;
            for (std::size_t i = 0; i < cap_; ++i) {
                slots_[i].destroy();
                std::destroy_at(&slots_[i]);
            }
            SlotAlloc sa{alloc_};
            std::allocator_traits<SlotAlloc>::deallocate(sa, slots_, cap_);
            slots_ = nullptr;
            cap_ = 0;
        }

        std::size_t probe_find(const auto& k) const noexcept {
            std::size_t pos = ideal(k), dist = 0;
            while (true) {
                const Slot& s = slots_[pos];
                if (!s.occupied || s.dist < dist) return npos;
                if (eq_(s.key(), k)) return pos;
                pos = (pos + 1) & mask();
                ++dist;
            }
        }

        bool robin_insert(const Key& k, Value v, const bool overwrite) {
            std::size_t pos = ideal(k);
            std::uint8_t dist = 0;
            Key ins_k = k;
            Value ins_v = std::move(v);
            while (true) {
                Slot& s = slots_[pos];
                if (!s.occupied) {
                    s.construct(std::move(ins_k), std::move(ins_v), alloc_);
                    s.dist = dist;
                    ++size_;
                    return true;
                }
                if (eq_(s.key(), ins_k)) {
                    if (overwrite) s.val() = std::move(ins_v);
                    return false;
                }
                if (s.dist < dist) {
                    std::swap(dist, s.dist);
                    std::swap(ins_k, s.key());
                    std::swap(ins_v, s.val());
                }
                pos = (pos + 1) & mask();
                ++dist;
            }
        }

        void maybe_grow() {
            if (load() < kMaxLoad) return;
            const std::size_t new_cap = cap_ * 2;
            Slot* old = slots_;
            std::size_t old_cap = cap_;
            alloc_slots(new_cap);
            size_ = 0;
            for (std::size_t i = 0; i < old_cap; ++i) {
                if (old[i].occupied)
                    robin_insert(old[i].key(), std::move(old[i].val()), false);
            }
            for (std::size_t i = 0; i < old_cap; ++i) {
                old[i].destroy();
                std::destroy_at(&old[i]);
            }
            SlotAlloc sa{alloc_};
            std::allocator_traits<SlotAlloc>::deallocate(sa, old, old_cap);
        }
    };

    // ----------------------------------------------------------------------------
    // NodeStorage<K,V,Alloc>
    // std::pmr::unordered_map wrapper — pointer-stable nodes.
    // ----------------------------------------------------------------------------
    template <
        typename Key,
        typename Value,
        typename Hash = std::hash<Key>,
        typename KeyEq = std::equal_to<Key>,
        typename Alloc = std::pmr::polymorphic_allocator<std::byte>>
    class NodeStorage {
        using MapAlloc = detail::rebind_alloc_t<Alloc, std::pair<const Key, Value>>;

    public:
        explicit NodeStorage (
            std::size_t /* initial_cap */ =
        16
        ,
        Hash h = {}, KeyEq e = {}, Alloc a = {}
        )
        :
        map_ { 0, h, e, MapAlloc{a} }
 {}

        Value* find(const auto& k) {
            auto it = map_.find(k);
            return it == map_.end() ? nullptr : &it->second;
        }

        [[nodiscard]] const Value* find(const auto& k) const {
            auto it = map_.find(k);
            return it == map_.end() ? nullptr : &it->second;
        }

        bool insert(const Key& k, Value v) { return map_.emplace(k, std::move(v)).second; }
        void insert_or_assign(const Key& k, Value v) { map_.insert_or_assign(k, std::move(v)); }
        bool erase(const Key& k) { return map_.erase(k) > 0; }
        void clear() noexcept { map_.clear(); }
        [[nodiscard]] std::size_t size() const noexcept { return map_.size(); }

        template <typename Fn>
        void for_each(Fn&& fn) { for (auto& [k,v] : map_) fn(k, v); }

        template <typename Fn>
        void for_each(Fn&& fn) const { for (const auto& [k,v] : map_) fn(k, v); }

    private:
        std::unordered_map<Key, Value, Hash, KeyEq, MapAlloc> map_;
    };

    // ============================================================================
    // § 6  Core Cache
    // ============================================================================

    template <
        typename Key,
        typename Value,
        typename Policy = LRUPolicy<Key>,
        typename Storage = FlatHashStorage<Key, Value>,
        typename Alloc = std::pmr::polymorphic_allocator<std::byte>>
        requires EvictionPolicy<Policy, Key>
    class Cache {
    public:
        using key_type = Key;
        using value_type = Value;
        using policy_type = Policy;
        using storage_type = Storage;
        using alloc_type = Alloc;

        explicit Cache(std::size_t capacity)
            : cap_{capacity}, policy_{capacity}, storage_{} {
            assert(capacity > 0 && "kosha::core::Cache: capacity must be > 0");
        }

        Cache(std::size_t capacity, std::pmr::memory_resource* /* r */)
            : cap_{capacity}, policy_{capacity}, storage_{} {
            assert(capacity > 0 && "kosha::core::Cache: capacity must be > 0");
        }

        Cache(const std::size_t capacity, Policy policy, Storage storage = Storage{},
              Alloc /* alloc */  = Alloc{})
            : cap_{capacity}, policy_{std::move(policy)}, storage_{std::move(storage)} {
            assert(capacity > 0 && "kosha::core::Cache: capacity must be > 0");
        }

        [[nodiscard]] static std::expected<Cache, Error> create(std::size_t capacity) {
            if (capacity == 0) return std::unexpected(Error::InvalidArg);
            return Cache{capacity};
        }

        [[nodiscard]] std::expected<Value, Error> get(const auto& key) {
            Value* vp = storage_.find(key);
            if (!vp) {
                on_miss_notify(key);
                return std::unexpected(Error::NotFound);
            }
            policy_.on_hit(key_cast(key));
            return *vp;
        }

        // Zero-copy access — returns pointer directly into storage (null on miss).
        // Pointer is invalidated by any subsequent put(), erase(), or clear().
        [[nodiscard]] Value* get_ref(const auto& key) {
            Value* vp = storage_.find(key);
            if (!vp) {
                on_miss_notify(key);
                return nullptr;
            }
            policy_.on_hit(key_cast(key));
            return vp;
        }

        [[nodiscard]] std::optional<Value> peek(const auto& key) const {
            const Value* vp = storage_.find(key);
            return vp ? std::optional{*vp} : std::nullopt;
        }

        [[nodiscard]] const Value* peek_ref(const auto& key) const {
            return storage_.find(key);
        }

        [[nodiscard]] std::expected<void, Error> put(Key key, Value value) {
            if (storage_.find(key) != nullptr) {
                policy_.on_hit(key);
                storage_.insert_or_assign(std::move(key), std::move(value));
                return {};
            }
            if constexpr (HasTransactionalInsert<Policy, Key>) {
                auto [victim, target_t2] = policy_.prepare_insert(key, storage_.size());
                if (victim) storage_.erase(*victim);
                // Capture key before move: commit_insert needs it after storage takes ownership.
                Key key_copy = key;
                storage_.insert_or_assign(std::move(key), std::move(value));
                policy_.commit_insert(key_copy, target_t2);
            }
            else {
                if (storage_.size() >= cap_) {
                    auto victim = policy_.evict();
                    if (!victim) return std::unexpected(Error::Capacity);
                    storage_.erase(*victim);
                }
                policy_.on_insert(key);
                storage_.insert_or_assign(std::move(key), std::move(value));
            }
            return {};
        }

        bool erase(const Key& key) noexcept {
            const bool removed = storage_.erase(key);
            if (removed) policy_.on_erase(key);
            return removed;
        }

        template <std::ranges::input_range R>
            requires std::convertible_to < std::ranges::range_value_t < R >



        ,
        Key
        >
        std::size_t erase_range(R&& keys) {
            std::size_t n = 0;
            for (const Key& k : keys) n += erase(k) ? 1 : 0;
            return n;
        }

        [[nodiscard]] std::size_t size() const noexcept { return storage_.size(); }
        [[nodiscard]] std::size_t capacity() const noexcept { return cap_; }
        [[nodiscard]] bool full() const noexcept { return size() >= cap_; }
        [[nodiscard]] bool empty() const noexcept { return size() == 0; }

        void clear() noexcept {
            storage_.clear();
            policy_.clear();
        }

        [[nodiscard]] const Policy& policy() const noexcept { return policy_; }
        [[nodiscard]] const Storage& storage() const noexcept { return storage_; }

        template <typename Fn>
        void for_each(Fn&& fn) const { storage_.for_each(std::forward<Fn>(fn)); }

        [[nodiscard]] std::size_t memory_bytes() const noexcept {
            return storage_.size() * (sizeof(Key) + sizeof(Value));
        }

    private:
        std::size_t cap_;
        Policy policy_;
        Storage storage_;

        // on_miss_notify avoids constructing a temporary Key for heterogeneous lookups:
        // policies accept const Key& so we only allocate when Q != Key.
        void on_miss_notify(const Key& k) { policy_.on_miss(k); }

        template <typename Q>
            requires (!std::same_as<std::decay_t<Q>, Key>)
        void on_miss_notify(const Q& q) {
            if constexpr (requires { policy_.on_miss(q); })
                policy_.on_miss(q); // transparent on_miss if policy supports it
            else
                policy_.on_miss(Key(q));
        }

        static const Key& key_cast(const Key& k) noexcept { return k; }

        template <typename Q>
        static Key key_cast(const Q& q) { return Key(q); }
    };

    // Concept conformance checks — verified at instantiation time.
    static_assert(StorageBackend<FlatHashStorage<int, int>, int, int>);
    static_assert(StorageBackend<NodeStorage<int, int>, int, int>);
    static_assert(EvictionPolicy<LRUPolicy<int>, int>);
    static_assert(EvictionPolicy<LFUPolicy<int>, int>);
    static_assert(EvictionPolicy<FIFOPolicy<int>, int>);
    static_assert(EvictionPolicy<ARCPolicy<int>, int>);
} // namespace kosha::core

// ============================================================================
// kosha::adapter — optional wrappers; unused adapters compile to nothing
// ============================================================================
namespace kosha::adapter {
    using core::Error;
    using core::mutates_on_hit;

    // ----------------------------------------------------------------------------
    // ThreadSafeCache<CacheType>
    // shared_mutex wrapper. get() lock grade is compile-time selected:
    //   mutates_on_hit = true  → unique_lock
    //   mutates_on_hit = false → shared_lock
    // ----------------------------------------------------------------------------
    template <typename CacheType>
    class ThreadSafeCache {
    public:
        using key_type = CacheType::key_type;
        using value_type = CacheType::value_type;
        using policy_type = CacheType::policy_type;

        template <typename... Args>
            requires std::constructible_from<CacheType, Args...>
        explicit ThreadSafeCache(Args&&... args) : cache_{std::forward<Args>(args)...} {}

        explicit ThreadSafeCache(CacheType c) : cache_{std::move(c)} {}

        [[nodiscard]] std::expected<value_type, Error> get(const auto& key) {
            if constexpr (mutates_on_hit<policy_type>) {
                std::unique_lock lk{mutex_};
                return cache_.get(key);
            }
            else {
                std::shared_lock lk{mutex_};
                return cache_.get(key);
            }
        }

        [[nodiscard]] std::optional<value_type> peek(const auto& key) const {
            std::shared_lock lk{mutex_};
            return cache_.peek(key);
        }

        [[nodiscard]] std::expected<void, Error> put(key_type key, value_type value) {
            std::unique_lock lk{mutex_};
            return cache_.put(std::move(key), std::move(value));
        }

        bool erase(const key_type& key) {
            std::unique_lock lk{mutex_};
            return cache_.erase(key);
        }

        void clear() {
            std::unique_lock lk{mutex_};
            cache_.clear();
        }

        [[nodiscard]] std::size_t size() const {
            std::shared_lock lk{mutex_};
            return cache_.size();
        }

        [[nodiscard]] std::size_t capacity() const noexcept { return cache_.capacity(); }

    private:
        CacheType cache_;
        mutable std::shared_mutex mutex_;
    };

    // ----------------------------------------------------------------------------
    // ShardedCache<CacheType, Shards>
    // Hash-stripes the keyspace across Shards ThreadSafeCache instances.
    // Each shard is cache-line-padded to eliminate false sharing.
    // Shards must be a power of two.
    // ----------------------------------------------------------------------------
    template <typename CacheType, std::size_t Shards = 16>
        requires (Shards >= 1) && (std::has_single_bit(Shards))
    class ShardedCache {
    public:
        using key_type = CacheType::key_type;
        using value_type = CacheType::value_type;

        explicit ShardedCache(const std::size_t total_capacity,
                              std::pmr::memory_resource* r = std::pmr::get_default_resource()) {
            const std::size_t per = std::max(total_capacity / Shards, std::size_t{1});
            for (auto& s : shards_)
                ::new(static_cast<void*>(s.storage)) ThreadSafeCache<CacheType>{per, r};
        }

        ~ShardedCache() {
            for (auto& s : shards_)
                std::destroy_at(reinterpret_cast<ThreadSafeCache<CacheType>*>(s.storage));
        }

        ShardedCache(const ShardedCache&) = delete;

        ShardedCache& operator=(const ShardedCache&) = delete;

        ShardedCache(ShardedCache&&) = delete;

        ShardedCache& operator=(ShardedCache&&) = delete;

        [[nodiscard]] std::expected<value_type, Error> get(const auto& key) { return shard(key).get(key); }
        [[nodiscard]] std::optional<value_type> peek(const auto& key) const { return shard_c(key).peek(key); }

        [[nodiscard]] std::expected<void, Error> put(key_type key, value_type value) {
            return shard(key).put(std::move(key), std::move(value));
        }

        bool erase(const key_type& key) { return shard(key).erase(key); }

        void clear() {
            for (auto& s : shards_)
                std::launder(reinterpret_cast<ThreadSafeCache<CacheType>*>(s.storage))->clear();
        }

        [[nodiscard]] std::size_t size() const {
            std::size_t t = 0;
            for (const auto& s : shards_)
                t += std::launder(reinterpret_cast<const ThreadSafeCache<CacheType>*>(s.storage))->size();
            return t;
        }

    private:
        static constexpr std::size_t cacheline_size = 64;

        struct alignas(cacheline_size) AlignedShard {
            alignas(ThreadSafeCache<CacheType>) unsigned char storage[sizeof(ThreadSafeCache<CacheType>)];
        };

        std::array<AlignedShard, Shards> shards_;

        ThreadSafeCache<CacheType>& shard(const auto& k) noexcept {
            const std::size_t i = std::hash<key_type>{}(k) & (Shards - 1);
            return *std::launder(reinterpret_cast<ThreadSafeCache<CacheType>*>(shards_[i].storage));
        }

        const ThreadSafeCache<CacheType>& shard_c(const auto& k) const noexcept {
            const std::size_t i = std::hash<key_type>{}(k) & (Shards - 1);
            return *std::launder(reinterpret_cast<const ThreadSafeCache<CacheType>*>(shards_[i].storage));
        }
    };

    // ----------------------------------------------------------------------------
    // InstrumentedCache<CacheType>
    // Wraps any cache and tracks hit / miss / eviction counts atomically.
    // hit_rate() is provided as a convenience.
    // Counters are std::atomic so the wrapper is safe to use inside ThreadSafeCache
    // without additional locking.
    // ----------------------------------------------------------------------------
    template <typename CacheType>
    class InstrumentedCache {
    public:
        using key_type = CacheType::key_type;
        using value_type = CacheType::value_type;
        using policy_type = CacheType::policy_type;

        template <typename... Args>
        explicit InstrumentedCache(Args&&... args) : cache_{std::forward<Args>(args)...} {}

        explicit InstrumentedCache(CacheType c) : cache_{std::move(c)} {}

        [[nodiscard]] std::expected<value_type, Error> get(const auto& key) {
            auto r = cache_.get(key);
            if (r.has_value()) ++hits_;
            else ++misses_;
            return r;
        }

        [[nodiscard]] std::optional<value_type> peek(const auto& key) const {
            return cache_.peek(key);
        }

        [[nodiscard]] std::expected<void, Error> put(key_type key, value_type value) {
            const bool was_full = cache_.full();
            auto r = cache_.put(std::move(key), std::move(value));
            if (was_full && r.has_value()) ++evictions_;
            return r;
        }

        bool erase(const key_type& key) { return cache_.erase(key); }

        void clear() noexcept {
            cache_.clear();
            reset_stats();
        }

        [[nodiscard]] std::size_t size() const noexcept { return cache_.size(); }
        [[nodiscard]] std::size_t capacity() const noexcept { return cache_.capacity(); }
        [[nodiscard]] bool full() const noexcept { return cache_.full(); }
        [[nodiscard]] bool empty() const noexcept { return cache_.empty(); }

        [[nodiscard]] const CacheType::policy_type& policy() const noexcept { return cache_.policy(); }
        [[nodiscard]] const CacheType::storage_type& storage() const noexcept { return cache_.storage(); }

        template <typename Fn>
        void for_each(Fn&& fn) const { cache_.for_each(std::forward<Fn>(fn)); }

        // --- Stats ---------------------------------------------------------------
        [[nodiscard]] std::uint64_t hit_count() const noexcept { return hits_.load(std::memory_order_relaxed); }
        [[nodiscard]] std::uint64_t miss_count() const noexcept { return misses_.load(std::memory_order_relaxed); }

        [[nodiscard]] std::uint64_t eviction_count() const noexcept {
            return evictions_.load(std::memory_order_relaxed);
        }

        [[nodiscard]] double hit_rate() const noexcept {
            const auto h = hit_count(), m = miss_count();
            return (h + m == 0) ? 0.0 : static_cast<double>(h) / static_cast<double>(h + m);
        }

        void reset_stats() const noexcept {
            hits_.store(0, std::memory_order_relaxed);
            misses_.store(0, std::memory_order_relaxed);
            evictions_.store(0, std::memory_order_relaxed);
        }

    private:
        CacheType cache_;
        mutable std::atomic<std::uint64_t> hits_{0};
        mutable std::atomic<std::uint64_t> misses_{0};
        mutable std::atomic<std::uint64_t> evictions_{0};
    };

    // ----------------------------------------------------------------------------
    // TTLCache<CacheType, Clock>
    // Wraps any cache with per-entry expiry. Expired entries are treated as misses
    // and lazily removed on access. put() accepts an optional TTL duration; entries
    // without an explicit TTL never expire.
    // ----------------------------------------------------------------------------
    template <
        typename CacheType,
        typename Clock = std::chrono::steady_clock>
    class TTLCache {
    public:
        using key_type = CacheType::key_type;
        using value_type = CacheType::value_type;
        using policy_type = CacheType::policy_type;
        using time_point = Clock::time_point;
        using duration = Clock::duration;

        template <typename... Args>
        explicit TTLCache(Args&&... args) : cache_{std::forward<Args>(args)...} {}

        explicit TTLCache(CacheType c) : cache_{std::move(c)} {}

        [[nodiscard]] std::expected<value_type, Error> get(const auto& key) {
            if (is_expired(key)) {
                evict_expired(key);
                return std::unexpected(Error::NotFound);
            }
            return cache_.get(key);
        }

        [[nodiscard]] std::optional<value_type> peek(const auto& key) const {
            if (is_expired(key)) return std::nullopt;
            return cache_.peek(key);
        }

        // put with explicit TTL duration.
        [[nodiscard]] std::expected<void, Error> put(
            key_type key, value_type value,
            std::optional<duration> ttl = std::nullopt) {
            if (ttl) {
                expiry_[key] = Clock::now() + *ttl;
            }
            else {
                expiry_.erase(key); // no expiry
            }
            return cache_.put(std::move(key), std::move(value));
        }

        bool erase(const key_type& key) {
            expiry_.erase(key);
            return cache_.erase(key);
        }

        void clear() noexcept {
            cache_.clear();
            expiry_.clear();
        }

        [[nodiscard]] std::size_t size() const noexcept { return cache_.size(); }
        [[nodiscard]] std::size_t capacity() const noexcept { return cache_.capacity(); }
        [[nodiscard]] bool full() const noexcept { return cache_.full(); }
        [[nodiscard]] bool empty() const noexcept { return cache_.empty(); }

        [[nodiscard]] const CacheType::policy_type& policy() const noexcept { return cache_.policy(); }
        [[nodiscard]] const CacheType::storage_type& storage() const noexcept { return cache_.storage(); }

        template <typename Fn>
        void for_each(Fn&& fn) const { cache_.for_each(std::forward<Fn>(fn)); }

    private:
        CacheType cache_;
        std::unordered_map<key_type, time_point> expiry_;

        bool is_expired(const auto& key) const {
            auto it = expiry_.find(key);
            if (it == expiry_.end()) return false;
            return Clock::now() >= it->second;
        }

        void evict_expired(const key_type& key) {
            expiry_.erase(key);
            cache_.erase(key);
        }
    };
} // namespace kosha::adapter

// ============================================================================
// kosha::cluster — distributed cache skeleton
//
// Zero cost when all policies are no-ops (the default).
// [[no_unique_address]] ensures empty policies occupy no space.
// Non-default policies are compiled in only when explicitly selected.
// ============================================================================
namespace kosha::cluster {
    using core::Error;

    // ============================================================================
    // § 1  Cluster policy concepts
    // ============================================================================

    // RouterPolicy: decides which node owns a key.
    template <typename R, typename K>
    concept RouterPolicy = requires(R r, const K& k) {
        { r.route(k) } -> std::convertible_to<std::size_t>; // returns node index
    };

    // TransportPolicy: sends and receives remote cache operations.
    template <typename T>
    concept TransportPolicy = requires(T t) {
        { t.is_local() } -> std::convertible_to<bool>;
    };

    // SerializerPolicy: encodes/decodes K and V for the wire.
    template <typename S>
    concept SerializerPolicy = requires { typename S::is_serializer; };

    // ReplicationPolicy: controls how many replicas are written.
    template <typename R>
    concept ReplicationPolicy = requires { typename R::is_replication; };

    // ConsistencyPolicy: controls read/write consistency semantics.
    template <typename C>
    concept ConsistencyPolicy = requires { typename C::is_consistency; };

    // MembershipPolicy: tracks which nodes are alive.
    template <typename M>
    concept MembershipPolicy = requires { typename M::is_membership; };

    // ============================================================================
    // § 2  No-op policy defaults (zero size via [[no_unique_address]])
    // ============================================================================

    struct NoRouter {
        template <typename K>
        std::size_t route(const K&) const noexcept { return 0; }
    };

    struct NoTransport {
        using is_transport = void;
        [[nodiscard]] bool is_local() const noexcept { return true; }
    };

    struct NoSerializer {
        using is_serializer = void;
    };

    struct NoReplication {
        using is_replication = void;
    };

    struct NoConsistency {
        using is_consistency = void;
    };

    struct NoMembership {
        using is_membership = void;
    };

    // ============================================================================
    // § 3  ClusterCache
    //
    // When all policies are no-ops:
    //   - Every operation delegates directly to the local cache.
    //   - No virtual calls, no network I/O, no serialization.
    //   - sizeof(ClusterCache<Local>) == sizeof(Local) (empty-base optimization
    //     via [[no_unique_address]]).
    //
    // To add distributed support, supply non-default policies:
    //   ClusterCache<ShardedLRUCache<int,int>,
    //                ConsistentHashRouter<int>,
    //                ZeroMQTransport,
    //                MsgpackSerializer,
    //                QuorumReplication<3>,
    //                StrongConsistency,
    //                GossipMembership>
    // ============================================================================
    template <
        typename LocalCache,
        typename Router = NoRouter,
        typename Transport = NoTransport,
        typename Serializer = NoSerializer,
        typename Replication = NoReplication,
        typename Consistency = NoConsistency,
        typename Membership = NoMembership>
    class ClusterCache {
    public:
        using key_type = LocalCache::key_type;
        using value_type = LocalCache::value_type;

        template <typename... Args>
        explicit ClusterCache(Args&&... args) : local_{std::forward<Args>(args)...} {}

        explicit ClusterCache(LocalCache lc,
                              Router r = {}, Transport t = {}, Serializer s = {},
                              Replication rep = {}, Consistency con = {}, Membership m = {})
            : local_{std::move(lc)}
              , router_{std::move(r)}, transport_{std::move(t)}, serializer_{std::move(s)}
              , replication_{std::move(rep)}, consistency_{std::move(con)}, membership_{std::move(m)} {}

        [[nodiscard]] std::expected<value_type, Error> get(const auto& key) {
            if (transport_.is_local()) return local_.get(key);
            // Future: route key, send remote get, deserialize response.
            return std::unexpected(Error::NotFound);
        }

        [[nodiscard]] std::optional<value_type> peek(const auto& key) const {
            if (transport_.is_local()) return local_.peek(key);
            return std::nullopt;
        }

        [[nodiscard]] std::expected<void, Error> put(key_type key, value_type value) {
            if (transport_.is_local()) return local_.put(std::move(key), std::move(value));
            // Future: route key, serialize, send remote put, handle replication.
            return std::unexpected(Error::Capacity);
        }

        bool erase(const key_type& key) {
            if (transport_.is_local()) return local_.erase(key);
            return false;
        }

        void clear() { local_.clear(); }

        [[nodiscard]] std::size_t size() const noexcept { return local_.size(); }
        [[nodiscard]] std::size_t capacity() const noexcept { return local_.capacity(); }

        // Expose policy accessors for inspection / testing.
        [[nodiscard]] const Router& router() const noexcept { return router_; }
        [[nodiscard]] const Transport& transport() const noexcept { return transport_; }
        [[nodiscard]] const Replication& replication() const noexcept { return replication_; }
        [[nodiscard]] const Consistency& consistency() const noexcept { return consistency_; }
        [[nodiscard]] const Membership& membership() const noexcept { return membership_; }

    private:
        LocalCache local_;
        [[no_unique_address]] Router router_;
        [[no_unique_address]] Transport transport_;
        [[no_unique_address]] Serializer serializer_;
        [[no_unique_address]] Replication replication_;
        [[no_unique_address]] Consistency consistency_;
        [[no_unique_address]] Membership membership_;
    };
} // namespace kosha::cluster

// ============================================================================
// kosha:: — backward-compatible aliases + new adapter aliases
// ============================================================================
namespace kosha {
    // Pull the error type and core primitives into the top-level namespace so
    // existing code that references kosha::Error, kosha::LRUPolicy, etc. keeps
    // compiling unchanged.
    using core::Error;
    using core::TransparentHash;
    using core::TransparentEqual;
    using core::EvictionPolicy;
    using core::StorageBackend;
    using core::mutates_on_hit;
    using core::LRUPolicy;
    using core::LFUPolicy;
    using core::FIFOPolicy;
    using core::ARCPolicy;
    using core::FlatHashStorage;
    using core::NodeStorage;
    using core::Cache;

    using adapter::ThreadSafeCache;
    using adapter::ShardedCache;
    using adapter::InstrumentedCache;
    using adapter::TTLCache;

    // --- Single-threaded core aliases -------------------------------------------
    template <typename K, typename V>
    using LRUCache = Cache<K, V, LRUPolicy<K>, FlatHashStorage<K, V>>;

    template <typename K, typename V>
    using LFUCache = Cache<K, V, LFUPolicy<K>, FlatHashStorage<K, V>>;

    template <typename K, typename V>
    using FIFOCache = Cache<K, V, FIFOPolicy<K>, FlatHashStorage<K, V>>;

    template <typename K, typename V>
    using ARCCache = Cache<K, V, ARCPolicy<K>, FlatHashStorage<K, V>>;

    // --- Thread-safe wrappers ---------------------------------------------------
    template <typename K, typename V>
    using TLRUCache = ThreadSafeCache<LRUCache<K, V>>;

    template <typename K, typename V>
    using TLFUCache = ThreadSafeCache<LFUCache<K, V>>;

    template <typename K, typename V>
    using TFIFOCache = ThreadSafeCache<FIFOCache<K, V>>;

    template <typename K, typename V>
    using TARCCache = ThreadSafeCache<ARCCache<K, V>>;

    // --- Sharded wrappers -------------------------------------------------------
    template <typename K, typename V, std::size_t S = 16>
    using ShardedLRUCache = ShardedCache<LRUCache<K, V>, S>;

    template <typename K, typename V, std::size_t S = 16>
    using ShardedLFUCache = ShardedCache<LFUCache<K, V>, S>;

    template <typename K, typename V, std::size_t S = 16>
    using ShardedARCCache = ShardedCache<ARCCache<K, V>, S>;

    // --- Instrumented aliases ---------------------------------------------------
    template <typename K, typename V>
    using InstrumentedLRUCache = InstrumentedCache<LRUCache<K, V>>;

    template <typename K, typename V>
    using InstrumentedLFUCache = InstrumentedCache<LFUCache<K, V>>;

    template <typename K, typename V>
    using InstrumentedFIFOCache = InstrumentedCache<FIFOCache<K, V>>;

    template <typename K, typename V>
    using InstrumentedARCCache = InstrumentedCache<ARCCache<K, V>>;

    // --- TTL aliases ------------------------------------------------------------
    template <typename K, typename V>
    using TTLLRUCache = TTLCache<LRUCache<K, V>>;

    template <typename K, typename V>
    using TTLLFUCache = TTLCache<LFUCache<K, V>>;

    template <typename K, typename V>
    using TTLFIFOCache = TTLCache<FIFOCache<K, V>>;

    template <typename K, typename V>
    using TTLARCCache = TTLCache<ARCCache<K, V>>;
} // namespace kosha
