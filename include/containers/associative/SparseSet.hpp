#pragma once

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <expected>
#include <iterator>
#include <limits>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

// ============================================================================
// SparseSet<Key, Value> — Briggs–Torczon sparse set with optional satellite data
//
// Characteristics
// ---------------
//  • O(1) insert, remove, contains — branch-free hot paths
//  • O(n) clear where n = size() — resets only occupied sparse slots
//  • O(n) dense iteration with perfect cache locality
//  • Universe-bounded: keys must satisfy 0 ≤ key < capacity()
//  • Dual-buffer layout: sparse[key] → dense index; dense[i] → key
//  • Optional satellite Value per key (defaults to std::monostate for pure set)
//  • Typed Key support: any unsigned integer or enum class with underlying
//    unsigned type (concept: SparseKey)
//  • std::expected for all fallible operations — no exceptions thrown
//  • Stable iteration order: insertion order preserved until a remove shifts
//    the last element into the removed slot (swap-and-pop)
//  • Range interface: begin/end, all_keys(), all_values(), all_pairs()
//  • Bulk operations: insert_range, remove_range, contains_all, contains_any
//  • Set-theoretic ops (intersection, union_with, difference) restricted to
//    pure-set usage (Value = std::monostate) to avoid silently dropping values
//  • No macros, no virtual functions
//
// Allocator
// ---------
//  Both internal vectors accept custom allocators via DenseAlloc / SparseAlloc
//  template parameters. Defaults to std::allocator for each.
// ============================================================================

namespace sparseset {
    // -------------------------------------------------------------------------
    // Error type
    // -------------------------------------------------------------------------
    enum class SSError : std::uint8_t {
        KeyOutOfRange, // key >= universe capacity
        KeyNotFound, // key not present during remove/lookup
        KeyAlreadyExists, // key already present during insert
        EmptySet, // operation requires non-empty set
    };

    // -------------------------------------------------------------------------
    // Concept: SparseKey
    //   Any unsigned integer type, or an enum class with unsigned underlying.
    // -------------------------------------------------------------------------
    template <typename K>
    concept SparseKey =
        (std::is_integral_v<K> && std::is_unsigned_v<K>) ||
        (std::is_enum_v<K> && std::is_unsigned_v<std::underlying_type_t<K>>);

    namespace detail {
        template <SparseKey K>
        [[nodiscard]] constexpr std::size_t to_index(K k) noexcept {
            if constexpr (std::is_enum_v<K>)
                return static_cast<std::size_t>(static_cast<std::underlying_type_t<K>>(k));
            else
                return static_cast<std::size_t>(k);
        }

        template <SparseKey K>
        [[nodiscard]] constexpr K from_index(std::size_t i) noexcept {
            if constexpr (std::is_enum_v<K>)
                return static_cast<K>(static_cast<std::underlying_type_t<K>>(i));
            else
                return static_cast<K>(i);
        }
    } // namespace detail

    // =========================================================================
    // Sparse Storage Policies
    // =========================================================================

    template <std::unsigned_integral IndexT = std::uint32_t, typename SparseAlloc = std::allocator<IndexT>>
    class FlatSparsePolicy {
    public:
        static constexpr IndexT kInvalid = std::numeric_limits<IndexT>::max();

        FlatSparsePolicy() = default;
        explicit FlatSparsePolicy(std::size_t universe_capacity, const SparseAlloc& sa = SparseAlloc{})
            : sparse_(universe_capacity, kInvalid, sa) {}

        void reserve(std::size_t new_cap) {
            if (new_cap > sparse_.size())
                sparse_.resize(new_cap, kInvalid);
        }

        [[nodiscard]] std::size_t capacity() const noexcept { return sparse_.size(); }

        [[nodiscard]] IndexT get(std::size_t idx) const noexcept {
            return idx < sparse_.size() ? sparse_[idx] : kInvalid;
        }

        void set(std::size_t idx, IndexT pos) {
            if (idx >= sparse_.size())
                sparse_.resize(idx + 1, kInvalid);
            sparse_[idx] = pos;
        }

        void clear_index(std::size_t idx) noexcept {
            if (idx < sparse_.size())
                sparse_[idx] = kInvalid;
        }

        void clear_all() noexcept {
            sparse_.assign(sparse_.size(), kInvalid);
        }

        [[nodiscard]] std::span<const IndexT> sparse_array() const noexcept {
            return sparse_;
        }

    private:
        std::vector<IndexT, SparseAlloc> sparse_;
    };

    template <std::unsigned_integral IndexT = std::uint32_t, std::size_t PageSize = 1024, typename SparseAlloc = std::allocator<IndexT>>
    class PagedSparsePolicy {
    public:
        static constexpr IndexT kInvalid = std::numeric_limits<IndexT>::max();
        using Page = std::array<IndexT, PageSize>;
        using PagePtr = std::unique_ptr<Page>;
        using PagePtrAlloc = typename std::allocator_traits<SparseAlloc>::template rebind_alloc<PagePtr>;

        PagedSparsePolicy() = default;
        explicit PagedSparsePolicy(std::size_t universe_capacity, const SparseAlloc& sa = SparseAlloc{})
            : pages_(PagePtrAlloc(sa)) {
            if (universe_capacity > 0) reserve(universe_capacity);
        }

        void reserve(std::size_t new_cap) {
            const std::size_t num_pages = (new_cap + PageSize - 1) / PageSize;
            if (num_pages > pages_.size())
                pages_.resize(num_pages);
        }

        [[nodiscard]] std::size_t capacity() const noexcept { return pages_.size() * PageSize; }

        [[nodiscard]] IndexT get(std::size_t idx) const noexcept {
            const std::size_t page_idx = idx / PageSize;
            const std::size_t offset = idx % PageSize;
            if (page_idx >= pages_.size() || !pages_[page_idx])
                return kInvalid;
            return (*pages_[page_idx])[offset];
        }

        void set(std::size_t idx, IndexT pos) {
            const std::size_t page_idx = idx / PageSize;
            const std::size_t offset = idx % PageSize;
            if (page_idx >= pages_.size())
                pages_.resize(page_idx + 1);
            if (!pages_[page_idx]) {
                pages_[page_idx] = std::make_unique<Page>();
                pages_[page_idx]->fill(kInvalid);
            }
            (*pages_[page_idx])[offset] = pos;
        }

        void clear_index(std::size_t idx) noexcept {
            const std::size_t page_idx = idx / PageSize;
            const std::size_t offset = idx % PageSize;
            if (page_idx < pages_.size() && pages_[page_idx])
                (*pages_[page_idx])[offset] = kInvalid;
        }

        void clear_all() noexcept {
            for (auto& p : pages_) {
                if (p) p->fill(kInvalid);
            }
        }

        [[nodiscard]] std::span<const IndexT> sparse_array() const noexcept {
            return {};
        }

    private:
        std::vector<PagePtr, PagePtrAlloc> pages_;
    };

    // =========================================================================
    // SparseSet
    //
    // Template parameters
    //   Key        – must satisfy SparseKey (e.g. std::uint32_t, entity_id enum)
    //   Value      – satellite data stored per key; std::monostate = pure set
    //   IndexT     – internal index type; uint32_t saves 4 bytes/slot vs size_t
    //   DenseAlloc – allocator for the dense Entry vector
    //   SparseAlloc – allocator for the sparse IndexT vector
    //   Policy     – FlatSparsePolicy or PagedSparsePolicy
    // =========================================================================
    template <
        SparseKey Key,
        typename Value = std::monostate,
        std::unsigned_integral IndexT = std::uint32_t,
        typename DenseAlloc = std::allocator<std::pair<Key, Value>>,
        typename SparseAlloc = std::allocator<IndexT>,
        class Policy = FlatSparsePolicy<IndexT, SparseAlloc>>
    class SparseSet {
    public:
        // ------------------------------------------------------------------ //
        // Type aliases
        // ------------------------------------------------------------------ //
        using key_type = Key;
        using value_type = Value;
        using index_type = IndexT;
        using size_type = std::size_t;
        using sparse_policy_type = Policy;
        static constexpr IndexT kInvalid = std::numeric_limits<IndexT>::max();

        static constexpr bool has_value = !std::is_same_v<Value, std::monostate>;

        // ------------------------------------------------------------------ //
        // Dense entry: keeps key and value together for cache-friendly iteration
        // ------------------------------------------------------------------ //
    private:
        struct Entry {
            Key key;
            [[no_unique_address]] Value val{};
        };

        using DenseAllocRebound =
        std::allocator_traits<DenseAlloc>::template rebind_alloc<Entry>;

    public:
        // ------------------------------------------------------------------ //
        // Construction
        // ------------------------------------------------------------------ //

        SparseSet() = default;

        explicit SparseSet(size_type universe_capacity,
                           const DenseAlloc& da = DenseAlloc{},
                           const SparseAlloc& sa = SparseAlloc{})
            : sparse_(universe_capacity, sa), dense_(DenseAllocRebound{da}) {}

        SparseSet(const SparseSet&) = default;

        SparseSet(SparseSet&&) noexcept = default;

        SparseSet& operator=(const SparseSet&) = default;

        SparseSet& operator=(SparseSet&&) noexcept = default;

        ~SparseSet() = default;

        // ------------------------------------------------------------------ //
        // Capacity management
        // ------------------------------------------------------------------ //

        // Grow the universe to at least new_cap. Does not shrink.
        void reserve(size_type new_cap) {
            sparse_.reserve(new_cap);
        }

        // Total keys the set can hold without resizing.
        [[nodiscard]] size_type capacity() const noexcept { return sparse_.capacity(); }

        // Number of keys currently in the set.
        [[nodiscard]] size_type size() const noexcept { return dense_.size(); }

        [[nodiscard]] bool empty() const noexcept { return dense_.empty(); }

        // ------------------------------------------------------------------ //
        // Core mutating operations
        // ------------------------------------------------------------------ //

        // Insert key with const-ref value. Returns the dense index on success.
        // SSError::KeyOutOfRange if key >= capacity().
        // SSError::KeyAlreadyExists if key is already present.
        std::expected<IndexT, SSError>
        insert(Key k, const Value& v) {
            const size_type idx = detail::to_index(k);
            if (idx >= sparse_.capacity())
                return std::unexpected(SSError::KeyOutOfRange);
            if (sparse_.get(idx) != kInvalid)
                return std::unexpected(SSError::KeyAlreadyExists);
            if (dense_.size() >= static_cast<size_type>(std::numeric_limits<IndexT>::max()))
                return std::unexpected(SSError::KeyOutOfRange);

            const auto pos = static_cast<IndexT>(dense_.size());
            sparse_.set(idx, pos);
            dense_.push_back(Entry{.key = k, .val = v});
            return pos;
        }

        // Insert key with rvalue value.
        std::expected<IndexT, SSError>
        insert(Key k, Value&& v = Value{}) {
            const size_type idx = detail::to_index(k);
            if (idx >= sparse_.capacity())
                return std::unexpected(SSError::KeyOutOfRange);
            if (sparse_.get(idx) != kInvalid)
                return std::unexpected(SSError::KeyAlreadyExists);
            if (dense_.size() >= static_cast<size_type>(std::numeric_limits<IndexT>::max()))
                return std::unexpected(SSError::KeyOutOfRange);

            const auto pos = static_cast<IndexT>(dense_.size());
            sparse_.set(idx, pos);
            dense_.push_back(Entry{.key = k, .val = std::move(v)});
            return pos;
        }

        // Insert or update with const-ref value: if key absent → insert; if present → overwrite.
        // Returns the dense index. Auto-reserves if needed.
        IndexT insert_or_update(Key k, const Value& v) {
            const size_type idx = detail::to_index(k);
            const IndexT current_pos = sparse_.get(idx);
            if (current_pos != kInvalid) {
                dense_[current_pos].val = v;
                return current_pos;
            }
            const auto pos = static_cast<IndexT>(dense_.size());
            sparse_.set(idx, pos);
            dense_.push_back(Entry{.key = k, .val = v});
            return pos;
        }

        // Insert or update with rvalue value.
        IndexT insert_or_update(Key k, Value&& v = Value{}) {
            const size_type idx = detail::to_index(k);
            const IndexT current_pos = sparse_.get(idx);
            if (current_pos != kInvalid) {
                dense_[current_pos].val = std::move(v);
                return current_pos;
            }
            const auto pos = static_cast<IndexT>(dense_.size());
            sparse_.set(idx, pos);
            dense_.push_back(Entry{.key = k, .val = std::move(v)});
            return pos;
        }

        // Remove key. Swap-and-pop to maintain dense packing.
        // SSError::KeyNotFound if key absent.
        std::expected<void, SSError>
        remove(Key k) {
            const size_type idx = detail::to_index(k);
            const IndexT pos = sparse_.get(idx);
            if (pos == kInvalid)
                return std::unexpected(SSError::KeyNotFound);

            assert(!dense_.empty());
            const size_type last = dense_.size() - 1;

            if (pos != static_cast<IndexT>(last)) {
                // Swap removed slot with the last dense entry.
                dense_[pos] = std::move(dense_[last]);
                sparse_.set(detail::to_index(dense_[pos].key), pos);
            }
            dense_.pop_back();
            sparse_.clear_index(idx);
            return {};
        }

        // ------------------------------------------------------------------ //
        // Lookup
        // ------------------------------------------------------------------ //

        [[nodiscard]] bool contains(Key k) const noexcept {
            const size_type idx = detail::to_index(k);
            return sparse_.get(idx) != kInvalid;
        }

        // Returns a reference to the value for key k.
        // SSError::KeyNotFound / KeyOutOfRange on failure.
        [[nodiscard]] std::expected<std::reference_wrapper<Value>, SSError>
        get(Key k) requires has_value {
            const size_type idx = detail::to_index(k);
            const IndexT pos = sparse_.get(idx);
            if (pos == kInvalid)
                return std::unexpected(SSError::KeyNotFound);
            return std::ref(dense_[pos].val);
        }

        [[nodiscard]] std::expected<std::reference_wrapper<const Value>, SSError>
        get(Key k) const requires has_value {
            const size_type idx = detail::to_index(k);
            const IndexT pos = sparse_.get(idx);
            if (pos == kInvalid)
                return std::unexpected(SSError::KeyNotFound);
            return std::cref(dense_[pos].val);
        }

        // Returns the dense-array index for key k (for direct indexing).
        [[nodiscard]] std::expected<IndexT, SSError>
        dense_index_of(Key k) const noexcept {
            const size_type idx = detail::to_index(k);
            const IndexT pos = sparse_.get(idx);
            if (pos == kInvalid)
                return std::unexpected(SSError::KeyNotFound);
            return pos;
        }

        // ------------------------------------------------------------------ //
        // Clear operations
        // ------------------------------------------------------------------ //

        // Clear all elements. O(n) where n = size(): resets occupied sparse slots.
        void clear() noexcept {
            for (const auto& e : dense_)
                sparse_.clear_index(detail::to_index(e.key));
            dense_.clear();
        }

        // O(universe_capacity): resets entire sparse array.
        void clear_all() noexcept {
            sparse_.clear_all();
            dense_.clear();
        }

        // Alias for clear_all(); kept for API compatibility.
        void reset() noexcept { clear_all(); }

        // ------------------------------------------------------------------ //
        // Range views — O(n) dense, perfect cache locality
        // ------------------------------------------------------------------ //

        // Lazy view of all keys in insertion order (modulo swap-and-pop).
        [[nodiscard]] auto all_keys() const {
            return dense_
                | std::views::transform([](const Entry& e) -> const Key& { return e.key; });
        }

        // Lazy view of all values.
        [[nodiscard]] auto all_values() requires has_value {
            return dense_
                | std::views::transform([](Entry& e) -> Value& { return e.val; });
        }

        [[nodiscard]] auto all_values() const requires has_value {
            return dense_
                | std::views::transform([](const Entry& e) -> const Value& { return e.val; });
        }

        // Lazy view of (key, value) pairs.
        [[nodiscard]] auto all_pairs() requires has_value {
            return dense_
                | std::views::transform([](Entry& e) -> std::pair<const Key&, Value&> {
                    return {e.key, e.val};
                });
        }

        [[nodiscard]] auto all_pairs() const requires has_value {
            return dense_
                | std::views::transform([](const Entry& e)
                    -> std::pair<const Key&, const Value&> {
                        return {e.key, e.val};
                    });
        }

        // Raw span over the dense array entries (zero-copy).
        [[nodiscard]] std::span<const Entry> dense_entries() const noexcept {
            return dense_;
        }

        // ------------------------------------------------------------------ //
        // Iterators — iterate keys directly (compatible with range-for)
        // ------------------------------------------------------------------ //
        struct const_iterator {
            using iterator_category = std::random_access_iterator_tag;
            using value_type = Key;
            using difference_type = std::ptrdiff_t;
            using pointer = const Key*;
            using reference = const Key&;

            const Entry* ptr{};

            const_iterator& operator++() noexcept {
                ++ptr;
                return *this;
            }

            const_iterator operator++(int) noexcept {
                auto tmp = *this;
                ++ptr;
                return tmp;
            }

            const_iterator& operator--() noexcept {
                --ptr;
                return *this;
            }

            const_iterator operator--(int) noexcept {
                auto tmp = *this;
                --ptr;
                return tmp;
            }

            const_iterator& operator+=(difference_type n) noexcept {
                ptr += n;
                return *this;
            }

            const_iterator& operator-=(difference_type n) noexcept {
                ptr -= n;
                return *this;
            }

            const_iterator operator+(difference_type n) const noexcept { return {ptr + n}; }
            const_iterator operator-(difference_type n) const noexcept { return {ptr - n}; }
            difference_type operator-(const_iterator o) const noexcept { return ptr - o.ptr; }
            reference operator*() const noexcept { return ptr->key; }
            pointer operator->() const noexcept { return &ptr->key; }
            reference operator[](difference_type n) const noexcept { return ptr[n].key; }

            bool operator==(const const_iterator&) const noexcept = default;

            auto operator<=>(const const_iterator&) const noexcept = default;
        };

        [[nodiscard]] const_iterator begin() const noexcept {
            return {dense_.data()};
        }

        [[nodiscard]] const_iterator end() const noexcept {
            return {dense_.data() + dense_.size()};
        }

        [[nodiscard]] const_iterator cbegin() const noexcept { return begin(); }
        [[nodiscard]] const_iterator cend() const noexcept { return end(); }

        // ------------------------------------------------------------------ //
        // Bulk operations
        // ------------------------------------------------------------------ //

        template <std::ranges::input_range R>
            requires std::convertible_to<std::ranges::range_value_t<R>, Key>
        void insert_range(R&& rng) {
            for (Key k : rng) {
                [[maybe_unused]] auto _ = insert_or_update(k);
            }
        }

        template <std::ranges::input_range R>
            requires std::convertible_to<std::ranges::range_value_t<R>, Key>
        void remove_range(R&& rng) {
            for (Key k : rng) {
                [[maybe_unused]] auto _ = remove(k);
            }
        }

        template <std::ranges::input_range R>
            requires std::convertible_to<std::ranges::range_value_t<R>, Key>
        [[nodiscard]] bool contains_all(R&& rng) const {
            return std::ranges::all_of(rng, [this](Key k) { return contains(k); });
        }

        template <std::ranges::input_range R>
            requires std::convertible_to<std::ranges::range_value_t<R>, Key>
        [[nodiscard]] bool contains_any(R&& rng) const {
            return std::ranges::any_of(rng, [this](Key k) { return contains(k); });
        }

        // ------------------------------------------------------------------ //
        // Set-theoretic operations (pure-set only; produce new SparseSet)
        // ------------------------------------------------------------------ //

        [[nodiscard]] SparseSet intersection(const SparseSet& other) const
            requires (!has_value) {
            const SparseSet& smaller = size() <= other.size() ? *this : other;
            const SparseSet& larger = size() <= other.size() ? other : *this;
            SparseSet result(larger.capacity());
            for (Key k : smaller)
                if (larger.contains(k))
                    result.insert_or_update(k);
            return result;
        }

        [[nodiscard]] SparseSet union_with(const SparseSet& other) const
            requires (!has_value) {
            SparseSet result(std::max(capacity(), other.capacity()));
            for (Key k : *this) result.insert_or_update(k);
            for (Key k : other) result.insert_or_update(k);
            return result;
        }

        [[nodiscard]] SparseSet difference(const SparseSet& other) const
            requires (!has_value) {
            SparseSet result(capacity());
            for (Key k : *this)
                if (!other.contains(k))
                    result.insert_or_update(k);
            return result;
        }

        // Direct access to the sparse array (for debugging / serialisation).
        [[nodiscard]] std::span<const IndexT> sparse_array() const noexcept {
            return sparse_.sparse_array();
        }

    private:
        Policy sparse_; // sparse_[key_index] → dense position or kInvalid
        std::vector<Entry, DenseAllocRebound> dense_; // dense_[pos] → {key, value}
    };

    template <
        SparseKey Key,
        typename Value = std::monostate,
        std::unsigned_integral IndexT = std::uint32_t,
        typename DenseAlloc = std::allocator<std::pair<Key, Value>>,
        typename SparseAlloc = std::allocator<IndexT>>
    using FlatSparseSet = SparseSet<Key, Value, IndexT, DenseAlloc, SparseAlloc, FlatSparsePolicy<IndexT, SparseAlloc>>;

    template <
        SparseKey Key,
        typename Value = std::monostate,
        std::unsigned_integral IndexT = std::uint32_t,
        typename DenseAlloc = std::allocator<std::pair<Key, Value>>,
        typename SparseAlloc = std::allocator<IndexT>>
    using PagedSparseSet = SparseSet<Key, Value, IndexT, DenseAlloc, SparseAlloc, PagedSparsePolicy<IndexT, 1024, SparseAlloc>>;

    // =========================================================================
    // Free-function helpers
    // =========================================================================

    // Build a SparseSet from a range of keys.
    template <SparseKey Key,
        typename Value = std::monostate,
        std::ranges::input_range R>
        requires std::convertible_to<std::ranges::range_value_t<R>, Key>
    [[nodiscard]] auto make_sparse_set(std::size_t universe_capacity, R&& rng) {
        SparseSet<Key, Value> s(universe_capacity);
        for (Key k : rng)
            s.insert_or_update(k);
        return s;
    }

    template <SparseKey Key, typename Value, std::unsigned_integral IndexT,
                             typename DA, typename SA, class Pol>
    [[nodiscard]] bool operator==(
        const SparseSet<Key, Value, IndexT, DA, SA, Pol>& a,
        const SparseSet<Key, Value, IndexT, DA, SA, Pol>& b) {
        if (a.size() != b.size()) return false;
        return std::ranges::all_of(a, [&b](Key k) { return b.contains(k); });
    }
} // namespace sparseset

namespace containers {
    using sparseset::SparseSet;
    using sparseset::FlatSparseSet;
    using sparseset::PagedSparseSet;
    using sparseset::FlatSparsePolicy;
    using sparseset::PagedSparsePolicy;
    using sparseset::SparseKey;
    using sparseset::SSError;
    using sparseset::make_sparse_set;
} // namespace containers

namespace pebble::containers {
    using sparseset::SparseSet;
    using sparseset::FlatSparseSet;
    using sparseset::PagedSparseSet;
    using sparseset::FlatSparsePolicy;
    using sparseset::PagedSparsePolicy;
    using sparseset::SparseKey;
    using sparseset::SSError;
    using sparseset::make_sparse_set;
} // namespace pebble::containers
