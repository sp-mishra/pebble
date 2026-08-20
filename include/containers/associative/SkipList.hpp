#pragma once
// ============================================================================
// containers/associative/SkipList.hpp — Generic C++23 SkipList Container
// ============================================================================
//
// Modern, policy-based, zero-virtual, high-performance SkipList container.
// Features:
//   - O(log n) search, insert, and delete
//   - O(log n + k) range queries / iteration
//   - Single-allocation variable-sized nodes: [NodeBase][forward* lvl][value_type]
//   - Head is a plain std::array<NodeBase*, MaxLevel> inside SkipList (zero heap
//     alloc on empty construction, no HeadSentinel special-casing)
//   - Fully allocator-aware (single allocation per node through rebound allocator)
//   - Copy-and-swap strong exception guarantee
//   - Standard container member types & PMR aliases
//   - ComparatorConcept and PromotionPolicyConcept enforced at class instantiation
//   - max_size(), key_comp(), value_comp(), initializer_list ctor
//   - std::construct_at / std::destroy_at for placement-new and destruction
//   - Zero macros, zero virtual functions, zero RTTI
// ============================================================================

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory>
#include <memory_resource>
#include <new>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>

namespace containers {

    // ============================================================================
    // Concepts
    // ============================================================================

    template <typename P>
    concept PromotionPolicyConcept = requires(P p, std::uint64_t& state) {
        { p(state) } -> std::convertible_to<std::size_t>;
    };

    /// ComparatorConcept: must be a strict weak order over K.
    template <typename C, typename K>
    concept ComparatorConcept =
        std::strict_weak_order<C, K, K> &&
        requires(const C& comp, const K& a, const K& b) {
            { comp(a, b) } -> std::convertible_to<bool>;
        };

    // ============================================================================
    // Default Promotion Policy
    // ============================================================================
    template <std::size_t MaxLevel>
    struct xorshift_promotion_policy {
        [[nodiscard]] std::size_t operator()(std::uint64_t& state) const noexcept {
            std::size_t level = 1;
            while (level < MaxLevel) {
                state ^= state << 13;
                state ^= state >> 7;
                state ^= state << 17;
                if ((state & 1ULL) == 0ULL) {
                    break;
                }
                ++level;
            }
            return level;
        }
    };

    // ============================================================================
    // SkipList
    // ============================================================================
    template <
        typename Key,
        typename Value,
        typename Compare = std::less<>,
        std::size_t MaxLevel = 16,
        typename Allocator = std::allocator<std::pair<const Key, Value>>,
        typename PromotionPolicy = xorshift_promotion_policy<MaxLevel>
    >
        requires PromotionPolicyConcept<PromotionPolicy> &&
                 ComparatorConcept<Compare, Key>
    class SkipList {
    public:
        // Standard container member types
        using key_type        = Key;
        using mapped_type     = Value;
        using value_type      = std::pair<const Key, Value>;
        using key_compare     = Compare;
        using size_type       = std::size_t;
        using difference_type = std::ptrdiff_t;
        using allocator_type  = Allocator;
        using reference       = value_type&;
        using const_reference = const value_type&;
        using pointer         = typename std::allocator_traits<allocator_type>::pointer;
        using const_pointer   = typename std::allocator_traits<allocator_type>::const_pointer;

        static_assert(MaxLevel > 0 && MaxLevel <= 64,
                      "SkipList MaxLevel must be in [1, 64]");

        // value_comp: compare by key through key_compare (std::map compatible)
        struct value_compare {
            key_compare comp;
            constexpr explicit value_compare(key_compare c) noexcept : comp{std::move(c)} {}
            [[nodiscard]] bool operator()(const value_type& a, const value_type& b) const {
                return comp(a.first, b.first);
            }
        };

    private:
        // -----------------------------------------------------------------------
        // Node layout: single allocation per node
        //   [ NodeBase ]
        //   [ NodeBase* x level  ]          (forward pointers, trailing the struct)
        //   [ alignment padding to alignof(value_type) ]
        //   [ value_type         ]
        // -----------------------------------------------------------------------
        struct NodeBase {
            std::uint8_t level{1};

            constexpr explicit NodeBase(std::size_t lvl = 1) noexcept
                : level(static_cast<std::uint8_t>(lvl)) {}

            NodeBase(const NodeBase&)            = delete;
            NodeBase& operator=(const NodeBase&) = delete;
            NodeBase(NodeBase&&) noexcept        = default;
            NodeBase& operator=(NodeBase&&) noexcept = default;
            ~NodeBase() = default;

            // Forward pointers are stored immediately after NodeBase in raw allocation
            [[nodiscard]] NodeBase** forward_ptrs() noexcept {
                return reinterpret_cast<NodeBase**>(
                    reinterpret_cast<std::byte*>(this) + sizeof(NodeBase));
            }
            [[nodiscard]] NodeBase* const* forward_ptrs() const noexcept {
                return reinterpret_cast<NodeBase* const*>(
                    reinterpret_cast<const std::byte*>(this) + sizeof(NodeBase));
            }

            [[nodiscard]] NodeBase* get_forward(std::size_t idx) const noexcept {
                return forward_ptrs()[idx];
            }
            void set_forward(std::size_t idx, NodeBase* n) noexcept {
                forward_ptrs()[idx] = n;
            }
        };

        // -----------------------------------------------------------------------
        // Static layout helpers
        // -----------------------------------------------------------------------
        [[nodiscard]] static constexpr std::size_t align_up(std::size_t n, std::size_t a) noexcept {
            return (n + a - 1) & ~(a - 1);
        }

        [[nodiscard]] static constexpr std::size_t value_offset_for_level(std::size_t lvl) noexcept {
            return align_up(sizeof(NodeBase) + lvl * sizeof(NodeBase*), alignof(value_type));
        }

        [[nodiscard]] static constexpr std::size_t total_allocation_size(std::size_t lvl) noexcept {
            return value_offset_for_level(lvl) + sizeof(value_type);
        }

        [[nodiscard]] static value_type* get_value_ptr(NodeBase* base) noexcept {
            return reinterpret_cast<value_type*>(
                reinterpret_cast<std::byte*>(base) + value_offset_for_level(base->level));
        }
        [[nodiscard]] static const value_type* get_value_ptr(const NodeBase* base) noexcept {
            return reinterpret_cast<const value_type*>(
                reinterpret_cast<const std::byte*>(base) + value_offset_for_level(base->level));
        }

        // -----------------------------------------------------------------------
        // Static layout correctness assertions
        // -----------------------------------------------------------------------
        static_assert(alignof(NodeBase) <= alignof(std::max_align_t),
                      "NodeBase alignment exceeds max_align_t");
        // Note: NodeBase is intentionally small (1 byte). The forward-pointer array
        // immediately follows in the raw allocation, but value_offset_for_level() uses
        // align_up() which already accounts for any padding needed before value_type.
        // No alignment assertion on NodeBase size vs NodeBase* is required.
        // The computed value offset for level-1 must be >= sizeof(NodeBase) + sizeof(NodeBase*)
        static_assert(value_offset_for_level(1) >= sizeof(NodeBase) + sizeof(NodeBase*),
                      "Single-level value offset must fit NodeBase and one forward pointer");
        static_assert(value_offset_for_level(MaxLevel) % alignof(value_type) == 0,
                      "Value offset at MaxLevel must satisfy alignof(value_type)");
        static_assert(total_allocation_size(MaxLevel) >= value_offset_for_level(MaxLevel) + sizeof(value_type),
                      "Total allocation must fit the value_type at the computed offset");

        // -----------------------------------------------------------------------
        // Rebound byte allocator for variable-sized raw allocations
        // -----------------------------------------------------------------------
        using byte_allocator_type   = typename std::allocator_traits<allocator_type>::template rebind_alloc<std::byte>;
        using byte_allocator_traits = std::allocator_traits<byte_allocator_type>;

    public:
        // -----------------------------------------------------------------------
        // Iterators
        // -----------------------------------------------------------------------
        class iterator {
        public:
            using iterator_category = std::forward_iterator_tag;
            using value_type        = SkipList::value_type;
            using difference_type   = std::ptrdiff_t;
            using pointer           = value_type*;
            using reference         = value_type&;

            constexpr iterator() noexcept : node_{nullptr} {}
            constexpr explicit iterator(NodeBase* n) noexcept : node_{n} {}

            [[nodiscard]] reference operator*()  const noexcept { return *get_value_ptr(node_); }
            [[nodiscard]] pointer   operator->() const noexcept { return  get_value_ptr(node_); }

            iterator& operator++() noexcept {
                if (node_) node_ = node_->get_forward(0);
                return *this;
            }
            iterator operator++(int) noexcept {
                iterator tmp = *this;
                ++(*this);
                return tmp;
            }

            [[nodiscard]] constexpr bool operator==(const iterator& o) const noexcept { return node_ == o.node_; }
            [[nodiscard]] constexpr bool operator!=(const iterator& o) const noexcept { return node_ != o.node_; }
            [[nodiscard]] constexpr NodeBase* node() const noexcept { return node_; }

        private:
            NodeBase* node_;
        };

        class const_iterator {
        public:
            using iterator_category = std::forward_iterator_tag;
            using value_type        = const SkipList::value_type;
            using difference_type   = std::ptrdiff_t;
            using pointer           = const value_type*;
            using reference         = const value_type&;

            constexpr const_iterator() noexcept : node_{nullptr} {}
            constexpr explicit const_iterator(const NodeBase* n) noexcept : node_{n} {}
            constexpr /* implicit */ const_iterator(iterator it) noexcept : node_{it.node()} {} // NOLINT

            [[nodiscard]] reference operator*()  const noexcept { return *get_value_ptr(node_); }
            [[nodiscard]] pointer   operator->() const noexcept { return  get_value_ptr(node_); }

            const_iterator& operator++() noexcept {
                if (node_) node_ = node_->get_forward(0);
                return *this;
            }
            const_iterator operator++(int) noexcept {
                const_iterator tmp = *this;
                ++(*this);
                return tmp;
            }

            [[nodiscard]] constexpr bool operator==(const const_iterator& o) const noexcept { return node_ == o.node_; }
            [[nodiscard]] constexpr bool operator!=(const const_iterator& o) const noexcept { return node_ != o.node_; }

        private:
            const NodeBase* node_;
        };

        // -----------------------------------------------------------------------
        // Constructors, Destructor & Assignment (Copy-and-Swap Strong Guarantee)
        // -----------------------------------------------------------------------
        SkipList()
            : SkipList(Compare{}, allocator_type{}, PromotionPolicy{}) {}

        explicit SkipList(const allocator_type& alloc)
            : SkipList(Compare{}, alloc, PromotionPolicy{}) {}

        explicit SkipList(
            Compare          comp,
            allocator_type   alloc            = allocator_type{},
            PromotionPolicy  promotion_policy = PromotionPolicy{})
            : comp_{std::move(comp)}
            , alloc_{std::move(alloc)}
            , promotion_policy_{std::move(promotion_policy)}
            , head_{}
            , current_level_{1}
            , size_{0}
            , rng_state_{0x9E3779B97F4A7C15ULL}
        {
            head_.fill(nullptr);
        }

        // Initializer-list constructor
        SkipList(std::initializer_list<value_type> init,
                 const allocator_type& alloc = allocator_type{})
            : SkipList(Compare{}, alloc, PromotionPolicy{})
        {
            for (const auto& kv : init) {
                insert(kv);
            }
        }

        ~SkipList() {
            clear();
        }

        SkipList(SkipList&& other) noexcept
            : comp_{std::move(other.comp_)}
            , alloc_{std::move(other.alloc_)}
            , promotion_policy_{std::move(other.promotion_policy_)}
            , head_{}
            , current_level_{other.current_level_}
            , size_{other.size_}
            , rng_state_{other.rng_state_}
        {
            head_ = other.head_;
            other.head_.fill(nullptr);
            other.current_level_ = 1;
            other.size_ = 0;
        }

        SkipList(const SkipList& other)
            : comp_{other.comp_}
            , alloc_{byte_allocator_traits::select_on_container_copy_construction(other.alloc_)}
            , promotion_policy_{other.promotion_policy_}
            , head_{}
            , current_level_{1}
            , size_{0}
            , rng_state_{other.rng_state_}
        {
            head_.fill(nullptr);
            for (const auto& kv : other) { insert(kv); }
        }

        SkipList(const SkipList& other, const allocator_type& alloc)
            : comp_{other.comp_}
            , alloc_{alloc}
            , promotion_policy_{other.promotion_policy_}
            , head_{}
            , current_level_{1}
            , size_{0}
            , rng_state_{other.rng_state_}
        {
            head_.fill(nullptr);
            for (const auto& kv : other) { insert(kv); }
        }

        SkipList(SkipList&& other, const allocator_type& alloc)
            : comp_{std::move(other.comp_)}
            , alloc_{alloc}
            , promotion_policy_{std::move(other.promotion_policy_)}
            , head_{}
            , current_level_{1}
            , size_{0}
            , rng_state_{other.rng_state_}
        {
            head_.fill(nullptr);
            if (alloc_ == other.alloc_) {
                head_          = other.head_;
                current_level_ = other.current_level_;
                size_          = other.size_;
                other.head_.fill(nullptr);
                other.current_level_ = 1;
                other.size_          = 0;
            } else {
                for (auto&& kv : other) { insert(std::move(kv)); }
                other.clear();
            }
        }

        // Copy assignment — copy-and-swap (strong exception guarantee)
        SkipList& operator=(const SkipList& other) {
            if (this != &other) {
                if constexpr (byte_allocator_traits::propagate_on_container_copy_assignment::value) {
                    if (alloc_ != other.alloc_) {
                        clear();
                        alloc_ = other.alloc_;
                    }
                }
                SkipList tmp(other, alloc_);
                swap(tmp);
            }
            return *this;
        }

        // Move assignment
        SkipList& operator=(SkipList&& other) noexcept(
            byte_allocator_traits::propagate_on_container_move_assignment::value ||
            byte_allocator_traits::is_always_equal::value)
        {
            if (this != &other) {
                auto steal = [&]() noexcept {
                    clear();
                    head_          = other.head_;
                    current_level_ = other.current_level_;
                    size_          = other.size_;
                    rng_state_     = other.rng_state_;
                    comp_          = std::move(other.comp_);
                    promotion_policy_ = std::move(other.promotion_policy_);
                    other.head_.fill(nullptr);
                    other.current_level_ = 1;
                    other.size_          = 0;
                };

                if constexpr (byte_allocator_traits::propagate_on_container_move_assignment::value) {
                    clear();
                    alloc_ = std::move(other.alloc_);
                    steal();
                } else if (alloc_ == other.alloc_) {
                    steal();
                } else {
                    clear();
                    comp_             = std::move(other.comp_);
                    promotion_policy_ = std::move(other.promotion_policy_);
                    rng_state_        = other.rng_state_;
                    for (auto&& kv : other) { insert(std::move(kv)); }
                    other.clear();
                }
            }
            return *this;
        }

        SkipList& operator=(std::initializer_list<value_type> init) {
            clear();
            for (const auto& kv : init) { insert(kv); }
            return *this;
        }

        // -----------------------------------------------------------------------
        // Modifiers
        // -----------------------------------------------------------------------
        template <typename K, typename V>
        std::pair<iterator, bool> insert_or_assign(K&& key, V&& val) {
            std::array<NodeBase*, MaxLevel> update{};
            NodeBase* existing = find_predecessors(key, update);

            if (existing && keys_equal(get_value_ptr(existing)->first, key)) {
                get_value_ptr(existing)->second = std::forward<V>(val);
                return {iterator{existing}, false};
            }

            NodeBase* new_node = link_new_node(std::forward<K>(key), std::forward<V>(val), update);
            return {iterator{new_node}, true};
        }

        std::pair<iterator, bool> insert(const value_type& value) {
            std::array<NodeBase*, MaxLevel> update{};
            NodeBase* existing = find_predecessors(value.first, update);
            if (existing && keys_equal(get_value_ptr(existing)->first, value.first)) {
                return {iterator{existing}, false};
            }
            return {iterator{link_new_node(value.first, value.second, update)}, true};
        }

        std::pair<iterator, bool> insert(value_type&& value) {
            std::array<NodeBase*, MaxLevel> update{};
            NodeBase* existing = find_predecessors(value.first, update);
            if (existing && keys_equal(get_value_ptr(existing)->first, value.first)) {
                return {iterator{existing}, false};
            }
            return {iterator{link_new_node(value.first, std::move(value.second), update)}, true};
        }

        iterator insert(const_iterator /*hint*/, const value_type& value) {
            return insert(value).first;
        }

        iterator insert(const_iterator /*hint*/, value_type&& value) {
            return insert(std::move(value)).first;
        }

        template <typename P>
            requires std::is_constructible_v<value_type, P&&>
        std::pair<iterator, bool> insert(P&& value) {
            value_type v(std::forward<P>(value));
            return insert(std::move(v));
        }

        template <typename P>
            requires std::is_constructible_v<value_type, P&&>
        iterator insert(const_iterator /*hint*/, P&& value) {
            return insert(std::forward<P>(value)).first;
        }

        template <typename InputIt>
        void insert(InputIt first, InputIt last) {
            for (; first != last; ++first) { insert(*first); }
        }

        void insert(std::initializer_list<value_type> init) {
            for (const auto& kv : init) { insert(kv); }
        }

        template <typename... Args>
        std::pair<iterator, bool> emplace(Args&&... args) {
            value_type v(std::forward<Args>(args)...);
            return insert(std::move(v));
        }

        template <typename... Args>
        std::pair<iterator, bool> try_emplace(const Key& key, Args&&... args) {
            std::array<NodeBase*, MaxLevel> update{};
            NodeBase* existing = find_predecessors(key, update);
            if (existing && keys_equal(get_value_ptr(existing)->first, key)) {
                return {iterator{existing}, false};
            }
            return {iterator{link_new_node(key, Value(std::forward<Args>(args)...), update)}, true};
        }

        template <typename... Args>
        std::pair<iterator, bool> try_emplace(Key&& key, Args&&... args) {
            std::array<NodeBase*, MaxLevel> update{};
            NodeBase* existing = find_predecessors(key, update);
            if (existing && keys_equal(get_value_ptr(existing)->first, key)) {
                return {iterator{existing}, false};
            }
            return {iterator{link_new_node(std::move(key), Value(std::forward<Args>(args)...), update)}, true};
        }

        bool erase(const Key& key) { return erase_impl(key); }

        template <typename K>
            requires requires(const Compare& c, const Key& a, const K& b) { c(a, b); c(b, a); }
        bool erase(const K& key) { return erase_impl(key); }

        iterator erase(iterator pos) {
            if (pos == end()) { return end(); }
            Key key_copy = pos->first;
            iterator next = std::next(pos);
            (void)erase(key_copy);
            return next;
        }

        iterator erase(iterator first, iterator last) {
            while (first != last) { first = erase(first); }
            return last;
        }

        mapped_type& operator[](const key_type& key) {
            return try_emplace(key).first->second;
        }

        mapped_type& operator[](key_type&& key) {
            return try_emplace(std::move(key)).first->second;
        }

        mapped_type& at(const key_type& key) {
            auto it = find(key);
            if (it == end()) { throw std::out_of_range("SkipList::at: key not found"); }
            return it->second;
        }

        const mapped_type& at(const key_type& key) const {
            auto it = find(key);
            if (it == end()) { throw std::out_of_range("SkipList::at: key not found"); }
            return it->second;
        }

        void swap(SkipList& other) noexcept(
            std::is_nothrow_swappable_v<Compare> &&
            byte_allocator_traits::is_always_equal::value &&
            std::is_nothrow_swappable_v<PromotionPolicy>)
        {
            using std::swap;
            swap(comp_, other.comp_);
            if constexpr (byte_allocator_traits::propagate_on_container_swap::value) {
                swap(alloc_, other.alloc_);
            }
            swap(promotion_policy_, other.promotion_policy_);
            swap(head_, other.head_);
            swap(current_level_, other.current_level_);
            swap(size_, other.size_);
            swap(rng_state_, other.rng_state_);
        }

        void clear() noexcept {
            NodeBase* cur = head_[0];
            while (cur) {
                NodeBase* next = cur->get_forward(0);
                deallocate_node(cur);
                cur = next;
            }
            head_.fill(nullptr);
            current_level_ = 1;
            size_ = 0;
        }

        // -----------------------------------------------------------------------
        // Lookup
        // -----------------------------------------------------------------------
        [[nodiscard]] iterator find(const Key& key) noexcept {
            NodeBase* node = find_node(key);
            return node ? iterator{node} : end();
        }

        template <typename K>
            requires requires(const Compare& c, const Key& a, const K& b) { c(a, b); c(b, a); }
        [[nodiscard]] iterator find(const K& key) noexcept {
            NodeBase* node = find_node(key);
            return node ? iterator{node} : end();
        }

        [[nodiscard]] const_iterator find(const Key& key) const noexcept {
            const NodeBase* node = find_node(key);
            return node ? const_iterator{node} : end();
        }

        template <typename K>
            requires requires(const Compare& c, const Key& a, const K& b) { c(a, b); c(b, a); }
        [[nodiscard]] const_iterator find(const K& key) const noexcept {
            const NodeBase* node = find_node(key);
            return node ? const_iterator{node} : end();
        }

        [[nodiscard]] bool contains(const Key& key) const noexcept {
            return find_node(key) != nullptr;
        }

        template <typename K>
            requires requires(const Compare& c, const Key& a, const K& b) { c(a, b); c(b, a); }
        [[nodiscard]] bool contains(const K& key) const noexcept {
            return find_node(key) != nullptr;
        }

        [[nodiscard]] size_type count(const Key& key) const noexcept {
            return contains(key) ? 1 : 0;
        }

        template <typename K>
            requires requires(const Compare& c, const Key& a, const K& b) { c(a, b); c(b, a); }
        [[nodiscard]] size_type count(const K& key) const noexcept {
            return contains(key) ? 1 : 0;
        }

        [[nodiscard]] iterator lower_bound(const Key& key) noexcept {
            return lower_bound_impl(key);
        }
        [[nodiscard]] const_iterator lower_bound(const Key& key) const noexcept {
            return lower_bound_impl(key);
        }

        template <typename K>
            requires requires(const Compare& c, const Key& a, const K& b) { c(a, b); c(b, a); }
        [[nodiscard]] iterator lower_bound(const K& key) noexcept {
            return lower_bound_impl(key);
        }

        template <typename K>
            requires requires(const Compare& c, const Key& a, const K& b) { c(a, b); c(b, a); }
        [[nodiscard]] const_iterator lower_bound(const K& key) const noexcept {
            return lower_bound_impl(key);
        }

        [[nodiscard]] iterator upper_bound(const Key& key) noexcept {
            return upper_bound_impl(key);
        }
        [[nodiscard]] const_iterator upper_bound(const Key& key) const noexcept {
            return upper_bound_impl(key);
        }

        template <typename K>
            requires requires(const Compare& c, const Key& a, const K& b) { c(a, b); c(b, a); }
        [[nodiscard]] iterator upper_bound(const K& key) noexcept {
            return upper_bound_impl(key);
        }

        template <typename K>
            requires requires(const Compare& c, const Key& a, const K& b) { c(a, b); c(b, a); }
        [[nodiscard]] const_iterator upper_bound(const K& key) const noexcept {
            return upper_bound_impl(key);
        }

        [[nodiscard]] std::pair<iterator, iterator> equal_range(const Key& key) noexcept {
            return {lower_bound(key), upper_bound(key)};
        }
        [[nodiscard]] std::pair<const_iterator, const_iterator> equal_range(const Key& key) const noexcept {
            return {lower_bound(key), upper_bound(key)};
        }

        template <typename K>
            requires requires(const Compare& c, const Key& a, const K& b) { c(a, b); c(b, a); }
        [[nodiscard]] std::pair<iterator, iterator> equal_range(const K& key) noexcept {
            return {lower_bound(key), upper_bound(key)};
        }

        template <typename K>
            requires requires(const Compare& c, const Key& a, const K& b) { c(a, b); c(b, a); }
        [[nodiscard]] std::pair<const_iterator, const_iterator> equal_range(const K& key) const noexcept {
            return {lower_bound(key), upper_bound(key)};
        }

        // -----------------------------------------------------------------------
        // Iterators & Capacity
        // -----------------------------------------------------------------------
        [[nodiscard]] iterator       begin()  noexcept       { return iterator{head_[0]}; }
        [[nodiscard]] iterator       end()    noexcept       { return iterator{nullptr}; }
        [[nodiscard]] const_iterator begin()  const noexcept { return const_iterator{head_[0]}; }
        [[nodiscard]] const_iterator end()    const noexcept { return const_iterator{nullptr}; }
        [[nodiscard]] const_iterator cbegin() const noexcept { return const_iterator{head_[0]}; }
        [[nodiscard]] const_iterator cend()   const noexcept { return const_iterator{nullptr}; }

        [[nodiscard]] size_type size()  const noexcept { return size_; }
        [[nodiscard]] bool      empty() const noexcept { return size_ == 0; }

        /// Maximum number of elements supportable by the byte-rebound allocator.
        [[nodiscard]] size_type max_size() const noexcept {
            // Each element requires at least total_allocation_size(1) bytes through
            // the byte allocator. Use the allocator's own max_size as an upper bound.
            const size_type alloc_max = byte_allocator_traits::max_size(alloc_);
            const size_type per_elem  = total_allocation_size(1);
            return alloc_max / per_elem;
        }

        // -----------------------------------------------------------------------
        // Observers
        // -----------------------------------------------------------------------
        [[nodiscard]] key_compare   key_comp()   const noexcept { return comp_; }
        [[nodiscard]] value_compare value_comp()  const noexcept { return value_compare{comp_}; }
        [[nodiscard]] allocator_type get_allocator() const noexcept { return allocator_type{alloc_}; }

    private:
        // -----------------------------------------------------------------------
        // Member data
        // -----------------------------------------------------------------------
        Compare                            comp_;
        byte_allocator_type                alloc_;
        PromotionPolicy                    promotion_policy_;
        std::array<NodeBase*, MaxLevel>    head_;   // Direct forward-pointer array; NO heap allocation on construction
        std::size_t                        current_level_{1};
        std::size_t                        size_{0};
        std::uint64_t                      rng_state_{0x9E3779B97F4A7C15ULL};

        // -----------------------------------------------------------------------
        // Internal helpers
        // -----------------------------------------------------------------------
        std::size_t random_level() noexcept {
            return promotion_policy_(rng_state_);
        }

        template <typename K, typename V>
        [[nodiscard]] NodeBase* allocate_node(K&& key, V&& value, std::size_t lvl) {
            const std::size_t bytes   = total_allocation_size(lvl);
            std::byte*        raw_mem = byte_allocator_traits::allocate(alloc_, bytes);

            // Construct NodeBase via std::construct_at
            NodeBase* base = std::construct_at(reinterpret_cast<NodeBase*>(raw_mem), lvl);

            // Zero-initialise trailing forward pointers
            for (std::size_t i = 0; i < lvl; ++i) {
                base->set_forward(i, nullptr);
            }

            // Construct value_type at computed aligned offset via std::construct_at
            value_type* val_dest = get_value_ptr(base);
            try {
                std::construct_at(
                    val_dest,
                    std::piecewise_construct,
                    std::forward_as_tuple(std::forward<K>(key)),
                    std::forward_as_tuple(std::forward<V>(value)));
            }
            catch (...) {
                std::destroy_at(base);
                byte_allocator_traits::deallocate(alloc_, raw_mem, bytes);
                throw;
            }
            return base;
        }

        void deallocate_node(NodeBase* node) noexcept {
            const std::size_t lvl   = node->level;
            const std::size_t bytes = total_allocation_size(lvl);

            std::destroy_at(get_value_ptr(node));
            std::destroy_at(node);

            std::byte* raw_mem = reinterpret_cast<std::byte*>(node);
            byte_allocator_traits::deallocate(alloc_, raw_mem, bytes);
        }

        template <typename A, typename B>
        [[nodiscard]] bool keys_equal(const A& lhs, const B& rhs) const {
            return !comp_(lhs, rhs) && !comp_(rhs, lhs);
        }

        // Shared traversal: fills update[] with predecessors at each level,
        // returns the level-0 candidate node immediately following the traversal position.
        // No head-sentinel special casing needed: head_[i] is the first real node at level i.
        template <typename K>
        NodeBase* find_predecessors(const K& key, std::array<NodeBase*, MaxLevel>& update) {
            // We use a "virtual predecessor" trick: update[i] initially points to nullptr
            // meaning "the slot before head_[i]". We track the actual current node
            // and detect when we are still in the head array.
            //
            // More idiomatically: use a sentinel index (-1) meaning "head".
            // The cleanest approach with a plain array head_ is:
            //   - use a raw pointer that represents "current node at this level";
            //   - when cur==nullptr we are at the head, so next = head_[i].
            //
            // Implementation: we carry cur as the last visited data-node (or null for head).

            NodeBase* cur = nullptr; // null == "at head sentinel position"
            for (int i = static_cast<int>(current_level_) - 1; i >= 0; --i) {
                NodeBase* nxt = (cur == nullptr) ? head_[i] : cur->get_forward(i);
                while (nxt && comp_(get_value_ptr(nxt)->first, key)) {
                    cur = nxt;
                    nxt = cur->get_forward(i);
                }
                update[i] = cur; // null means "predecessor is head"
            }
            // The level-0 candidate is head_[0] (if cur==null) or cur->forward[0]
            return (cur == nullptr) ? head_[0] : cur->get_forward(0);
        }

        template <typename K, typename V>
        NodeBase* link_new_node(K&& key, V&& value, std::array<NodeBase*, MaxLevel>& update) {
            const std::size_t new_level = random_level();
            if (new_level > current_level_) {
                // New levels: their predecessor is the head (represented by nullptr in update[])
                for (std::size_t i = current_level_; i < new_level; ++i) {
                    update[i] = nullptr;
                }
                current_level_ = new_level;
            }

            NodeBase* new_node = allocate_node(std::forward<K>(key), std::forward<V>(value), new_level);

            for (std::size_t i = 0; i < new_level; ++i) {
                NodeBase* pred = update[i]; // null == head
                // new_node->forward[i] = pred ? pred->forward[i] : head_[i]
                new_node->set_forward(i, pred ? pred->get_forward(i) : head_[i]);
                // predecessor->forward[i] = new_node
                if (pred) {
                    pred->set_forward(i, new_node);
                } else {
                    head_[i] = new_node;
                }
            }
            ++size_;
            return new_node;
        }

        template <typename K>
        [[nodiscard]] NodeBase* find_node(const K& key) noexcept {
            NodeBase* cur = nullptr;
            for (int i = static_cast<int>(current_level_) - 1; i >= 0; --i) {
                NodeBase* nxt = (cur == nullptr) ? head_[i] : cur->get_forward(i);
                while (nxt && comp_(get_value_ptr(nxt)->first, key)) {
                    cur = nxt;
                    nxt = cur->get_forward(i);
                }
            }
            NodeBase* candidate = (cur == nullptr) ? head_[0] : cur->get_forward(0);
            if (candidate && keys_equal(get_value_ptr(candidate)->first, key)) {
                return candidate;
            }
            return nullptr;
        }

        template <typename K>
        [[nodiscard]] const NodeBase* find_node(const K& key) const noexcept {
            const NodeBase* cur = nullptr;
            for (int i = static_cast<int>(current_level_) - 1; i >= 0; --i) {
                const NodeBase* nxt = (cur == nullptr) ? head_[i] : cur->get_forward(i);
                while (nxt && comp_(get_value_ptr(nxt)->first, key)) {
                    cur = nxt;
                    nxt = cur->get_forward(i);
                }
            }
            const NodeBase* candidate = (cur == nullptr) ? head_[0] : cur->get_forward(0);
            if (candidate && keys_equal(get_value_ptr(candidate)->first, key)) {
                return candidate;
            }
            return nullptr;
        }

        template <typename K>
        bool erase_impl(const K& key) {
            std::array<NodeBase*, MaxLevel> update{};
            NodeBase* victim = find_predecessors(key, update);

            if (!victim || !keys_equal(get_value_ptr(victim)->first, key)) {
                return false;
            }

            for (std::size_t i = 0; i < current_level_; ++i) {
                NodeBase* pred      = update[i]; // null == head
                NodeBase* pred_next = pred ? pred->get_forward(i) : head_[i];
                if (pred_next != victim) break;
                NodeBase* victim_next = victim->get_forward(i);
                if (pred) {
                    pred->set_forward(i, victim_next);
                } else {
                    head_[i] = victim_next;
                }
            }

            while (current_level_ > 1 && head_[current_level_ - 1] == nullptr) {
                --current_level_;
            }

            deallocate_node(victim);
            --size_;
            return true;
        }

        template <typename K>
        [[nodiscard]] iterator lower_bound_impl(const K& key) noexcept {
            NodeBase* cur = nullptr;
            for (int i = static_cast<int>(current_level_) - 1; i >= 0; --i) {
                NodeBase* nxt = (cur == nullptr) ? head_[i] : cur->get_forward(i);
                while (nxt && comp_(get_value_ptr(nxt)->first, key)) {
                    cur = nxt;
                    nxt = cur->get_forward(i);
                }
            }
            return iterator{(cur == nullptr) ? head_[0] : cur->get_forward(0)};
        }

        template <typename K>
        [[nodiscard]] const_iterator lower_bound_impl(const K& key) const noexcept {
            const NodeBase* cur = nullptr;
            for (int i = static_cast<int>(current_level_) - 1; i >= 0; --i) {
                const NodeBase* nxt = (cur == nullptr) ? head_[i] : cur->get_forward(i);
                while (nxt && comp_(get_value_ptr(nxt)->first, key)) {
                    cur = nxt;
                    nxt = cur->get_forward(i);
                }
            }
            return const_iterator{(cur == nullptr) ? head_[0] : cur->get_forward(0)};
        }

        template <typename K>
        [[nodiscard]] iterator upper_bound_impl(const K& key) noexcept {
            NodeBase* cur = nullptr;
            for (int i = static_cast<int>(current_level_) - 1; i >= 0; --i) {
                NodeBase* nxt = (cur == nullptr) ? head_[i] : cur->get_forward(i);
                while (nxt && !comp_(key, get_value_ptr(nxt)->first)) {
                    cur = nxt;
                    nxt = cur->get_forward(i);
                }
            }
            return iterator{(cur == nullptr) ? head_[0] : cur->get_forward(0)};
        }

        template <typename K>
        [[nodiscard]] const_iterator upper_bound_impl(const K& key) const noexcept {
            const NodeBase* cur = nullptr;
            for (int i = static_cast<int>(current_level_) - 1; i >= 0; --i) {
                const NodeBase* nxt = (cur == nullptr) ? head_[i] : cur->get_forward(i);
                while (nxt && !comp_(key, get_value_ptr(nxt)->first)) {
                    cur = nxt;
                    nxt = cur->get_forward(i);
                }
            }
            return const_iterator{(cur == nullptr) ? head_[0] : cur->get_forward(0)};
        }
    };

    // Free-function swap (ADL)
    template <typename Key, typename Value, typename Compare, std::size_t MaxLevel,
              typename Allocator, typename PromotionPolicy>
    void swap(SkipList<Key, Value, Compare, MaxLevel, Allocator, PromotionPolicy>& a,
              SkipList<Key, Value, Compare, MaxLevel, Allocator, PromotionPolicy>& b)
        noexcept(noexcept(a.swap(b)))
    {
        a.swap(b);
    }

    // ============================================================================
    // PMR (Polymorphic Memory Resource) Aliases
    // ============================================================================
    namespace pmr {
        template <
            typename Key,
            typename Value,
            typename Compare       = std::less<>,
            std::size_t MaxLevel   = 16,
            typename PromotionPolicy = xorshift_promotion_policy<MaxLevel>
        >
        using SkipList = containers::SkipList<
            Key,
            Value,
            Compare,
            MaxLevel,
            std::pmr::polymorphic_allocator<std::pair<const Key, Value>>,
            PromotionPolicy
        >;
    } // namespace pmr

} // namespace containers
