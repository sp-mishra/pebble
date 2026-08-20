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
//   - Head sentinel with stack/direct array of forward pointers (zero heap alloc on empty construction)
//   - Fully allocator-aware (allocator-managed single allocation per node)
//   - Copy-and-swap strong exception guarantee
//   - Standard container member types & PMR aliases
//   - Concepts for PromotionPolicy and Comparator
//   - Zero macros, zero virtual functions, zero RTTI
// ============================================================================

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
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

    template <typename C, typename K>
    concept ComparatorConcept = requires(const C& comp, const K& a, const K& b) {
        { comp(a, b) } -> std::convertible_to<bool>;
    };

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

    template <
        typename Key,
        typename Value,
        typename Compare = std::less<>,
        std::size_t MaxLevel = 16,
        typename Allocator = std::allocator<std::pair<const Key, Value>>,
        typename PromotionPolicy = xorshift_promotion_policy<MaxLevel>
    >
        requires PromotionPolicyConcept<PromotionPolicy>
    class SkipList {
    public:
        // Standard container member types
        using key_type = Key;
        using mapped_type = Value;
        using value_type = std::pair<const Key, Value>;
        using key_compare = Compare;
        using size_type = std::size_t;
        using difference_type = std::ptrdiff_t;
        using allocator_type = Allocator;
        using reference = value_type&;
        using const_reference = const value_type&;
        using pointer = typename std::allocator_traits<allocator_type>::pointer;
        using const_pointer = typename std::allocator_traits<allocator_type>::const_pointer;

        static_assert(MaxLevel > 0, "SkipList MaxLevel must be > 0");

    private:
        struct NodeBase {
            std::uint8_t level{1};

            constexpr explicit NodeBase(const std::size_t lvl = 1) noexcept
                : level(static_cast<std::uint8_t>(lvl)) {}

            NodeBase(const NodeBase&) = delete;
            NodeBase& operator=(const NodeBase&) = delete;
            NodeBase(NodeBase&&) noexcept = default;
            NodeBase& operator=(NodeBase&&) noexcept = default;
            ~NodeBase() = default;

            // Direct access to trailing forward pointer array in single-allocation layout
            [[nodiscard]] NodeBase** forward_ptrs() noexcept {
                return reinterpret_cast<NodeBase**>(reinterpret_cast<std::byte*>(this) + sizeof(NodeBase));
            }

            [[nodiscard]] NodeBase* const* forward_ptrs() const noexcept {
                return reinterpret_cast<NodeBase* const*>(reinterpret_cast<const std::byte*>(this) + sizeof(NodeBase));
            }

            [[nodiscard]] NodeBase* get_forward(std::size_t idx) const noexcept {
                return forward_ptrs()[idx];
            }

            void set_forward(std::size_t idx, NodeBase* n) noexcept {
                forward_ptrs()[idx] = n;
            }
        };

        // Layout offsets and calculation for single-allocation node:
        // [ NodeBase ] -> [ NodeBase* x level ] -> [ padding to alignof(value_type) ] -> [ value_type ]
        [[nodiscard]] static constexpr std::size_t align_up(const std::size_t n, const std::size_t a) noexcept {
            return (n + a - 1) & ~(a - 1);
        }

        [[nodiscard]] static constexpr std::size_t value_offset_for_level(std::size_t lvl) noexcept {
            const std::size_t raw_offset = sizeof(NodeBase) + (lvl * sizeof(NodeBase*));
            return align_up(raw_offset, alignof(value_type));
        }

        [[nodiscard]] static constexpr std::size_t total_allocation_size(std::size_t lvl) noexcept {
            return value_offset_for_level(lvl) + sizeof(value_type);
        }

        [[nodiscard]] static value_type* get_value_ptr(NodeBase* base) noexcept {
            const std::size_t offset = value_offset_for_level(base->level);
            return reinterpret_cast<value_type*>(reinterpret_cast<std::byte*>(base) + offset);
        }

        [[nodiscard]] static const value_type* get_value_ptr(const NodeBase* base) noexcept {
            const std::size_t offset = value_offset_for_level(base->level);
            return reinterpret_cast<const value_type*>(reinterpret_cast<const std::byte*>(base) + offset);
        }

        // Rebound byte allocator for variable-sized raw allocations
        using byte_allocator_type = typename std::allocator_traits<allocator_type>::template rebind_alloc<std::byte>;
        using byte_allocator_traits = std::allocator_traits<byte_allocator_type>;

        // Helper structure for head sentinel (uses stack array for zero heap alloc on empty construction)
        struct HeadSentinel {
            NodeBase base{MaxLevel};
            std::array<NodeBase*, MaxLevel> forward_links{};

            constexpr HeadSentinel() noexcept {
                forward_links.fill(nullptr);
            }

            [[nodiscard]] NodeBase* get_forward(std::size_t idx) const noexcept {
                return forward_links[idx];
            }

            void set_forward(std::size_t idx, NodeBase* n) noexcept {
                forward_links[idx] = n;
            }

            [[nodiscard]] NodeBase* as_node_base() noexcept {
                return &base;
            }

            [[nodiscard]] const NodeBase* as_node_base() const noexcept {
                return &base;
            }
        };

        [[nodiscard]] NodeBase* get_node_forward(NodeBase* node, std::size_t idx) const noexcept {
            if (node == head_sentinel_.as_node_base()) {
                return head_sentinel_.get_forward(idx);
            }
            return node->get_forward(idx);
        }

        [[nodiscard]] const NodeBase* get_node_forward(const NodeBase* node, std::size_t idx) const noexcept {
            if (node == head_sentinel_.as_node_base()) {
                return head_sentinel_.get_forward(idx);
            }
            return node->get_forward(idx);
        }

        void set_node_forward(NodeBase* node, std::size_t idx, NodeBase* target) noexcept {
            if (node == head_sentinel_.as_node_base()) {
                head_sentinel_.set_forward(idx, target);
            } else {
                node->set_forward(idx, target);
            }
        }

    public:
        // Forward iterator for SkipList
        class iterator {
        public:
            using iterator_category = std::forward_iterator_tag;
            using value_type = SkipList::value_type;
            using difference_type = std::ptrdiff_t;
            using pointer = value_type*;
            using reference = value_type&;

            constexpr iterator() noexcept : node_{nullptr} {}
            constexpr explicit iterator(NodeBase* n) noexcept : node_{n} {}

            [[nodiscard]] reference operator*() const noexcept { return *get_value_ptr(node_); }
            [[nodiscard]] pointer operator->() const noexcept { return get_value_ptr(node_); }

            iterator& operator++() noexcept {
                if (node_) node_ = node_->get_forward(0);
                return *this;
            }

            iterator operator++(int) noexcept {
                iterator tmp = *this;
                ++(*this);
                return tmp;
            }

            constexpr bool operator==(const iterator& other) const noexcept { return node_ == other.node_; }
            constexpr bool operator!=(const iterator& other) const noexcept { return node_ != other.node_; }

            [[nodiscard]] constexpr NodeBase* node() const noexcept { return node_; }

        private:
            NodeBase* node_;
        };

        class const_iterator {
        public:
            using iterator_category = std::forward_iterator_tag;
            using value_type = const SkipList::value_type;
            using difference_type = std::ptrdiff_t;
            using pointer = const value_type*;
            using reference = const value_type&;

            constexpr const_iterator() noexcept : node_{nullptr} {}
            constexpr explicit const_iterator(const NodeBase* n) noexcept : node_{n} {}
            constexpr explicit const_iterator(iterator it) noexcept : node_{it.node()} {}

            [[nodiscard]] reference operator*() const noexcept { return *get_value_ptr(node_); }
            [[nodiscard]] pointer operator->() const noexcept { return get_value_ptr(node_); }

            const_iterator& operator++() noexcept {
                if (node_) node_ = node_->get_forward(0);
                return *this;
            }

            const_iterator operator++(int) noexcept {
                const_iterator tmp = *this;
                ++(*this);
                return tmp;
            }

            constexpr bool operator==(const const_iterator& other) const noexcept { return node_ == other.node_; }
            constexpr bool operator!=(const const_iterator& other) const noexcept { return node_ != other.node_; }

        private:
            const NodeBase* node_;
        };

        // ------------------------------------------------------------------------
        // Constructors, Destructor & Assignment (Copy-and-Swap Strong Guarantee)
        // ------------------------------------------------------------------------
        SkipList()
            : SkipList(Compare{}, allocator_type{}, PromotionPolicy{}) {}

        explicit SkipList(const allocator_type& alloc)
            : SkipList(Compare{}, alloc, PromotionPolicy{}) {}

        explicit SkipList(
            Compare comp,
            allocator_type alloc = allocator_type{},
            PromotionPolicy promotion_policy = PromotionPolicy{})
            : comp_{std::move(comp)},
              alloc_{std::move(alloc)},
              promotion_policy_{std::move(promotion_policy)},
              head_sentinel_{},
              current_level_{1},
              size_{0},
              rng_state_{0x9E3779B97F4A7C15ULL} {}

        ~SkipList() {
            clear();
        }

        SkipList(SkipList&& other) noexcept
            : comp_{std::move(other.comp_)},
              alloc_{std::move(other.alloc_)},
              promotion_policy_{std::move(other.promotion_policy_)},
              head_sentinel_{},
              current_level_{other.current_level_},
              size_{other.size_},
              rng_state_{other.rng_state_} {
            for (std::size_t i = 0; i < MaxLevel; ++i) {
                head_sentinel_.set_forward(i, other.head_sentinel_.get_forward(i));
                other.head_sentinel_.set_forward(i, nullptr);
            }
            other.current_level_ = 1;
            other.size_ = 0;
        }

        SkipList(const SkipList& other)
            : comp_{other.comp_},
              alloc_{byte_allocator_traits::select_on_container_copy_construction(other.alloc_)},
              promotion_policy_{other.promotion_policy_},
              head_sentinel_{},
              current_level_{1},
              size_{0},
              rng_state_{other.rng_state_} {
            for (const auto& kv : other) {
                insert(kv);
            }
        }

        SkipList(const SkipList& other, const allocator_type& alloc)
            : comp_{other.comp_},
              alloc_{alloc},
              promotion_policy_{other.promotion_policy_},
              head_sentinel_{},
              current_level_{1},
              size_{0},
              rng_state_{other.rng_state_} {
            for (const auto& kv : other) {
                insert(kv);
            }
        }

        SkipList(SkipList&& other, const allocator_type& alloc)
            : comp_{std::move(other.comp_)},
              alloc_{alloc},
              promotion_policy_{std::move(other.promotion_policy_)},
              head_sentinel_{},
              current_level_{1},
              size_{0},
              rng_state_{other.rng_state_} {
            if (alloc_ == other.alloc_) {
                for (std::size_t i = 0; i < MaxLevel; ++i) {
                    head_sentinel_.set_forward(i, other.head_sentinel_.get_forward(i));
                    other.head_sentinel_.set_forward(i, nullptr);
                }
                current_level_ = other.current_level_;
                size_ = other.size_;
                other.current_level_ = 1;
                other.size_ = 0;
            } else {
                for (auto&& kv : other) {
                    insert(std::move(kv));
                }
                other.clear();
            }
        }

        // Copy assignment using copy-and-swap for strong exception guarantee
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
            byte_allocator_traits::is_always_equal::value) {
            if (this != &other) {
                if constexpr (byte_allocator_traits::propagate_on_container_move_assignment::value) {
                    clear();
                    alloc_ = std::move(other.alloc_);
                    comp_ = std::move(other.comp_);
                    promotion_policy_ = std::move(other.promotion_policy_);
                    for (std::size_t i = 0; i < MaxLevel; ++i) {
                        head_sentinel_.set_forward(i, other.head_sentinel_.get_forward(i));
                        other.head_sentinel_.set_forward(i, nullptr);
                    }
                    current_level_ = other.current_level_;
                    size_ = other.size_;
                    rng_state_ = other.rng_state_;
                    other.current_level_ = 1;
                    other.size_ = 0;
                } else if (alloc_ == other.alloc_) {
                    clear();
                    comp_ = std::move(other.comp_);
                    promotion_policy_ = std::move(other.promotion_policy_);
                    for (std::size_t i = 0; i < MaxLevel; ++i) {
                        head_sentinel_.set_forward(i, other.head_sentinel_.get_forward(i));
                        other.head_sentinel_.set_forward(i, nullptr);
                    }
                    current_level_ = other.current_level_;
                    size_ = other.size_;
                    rng_state_ = other.rng_state_;
                    other.current_level_ = 1;
                    other.size_ = 0;
                } else {
                    clear();
                    comp_ = std::move(other.comp_);
                    promotion_policy_ = std::move(other.promotion_policy_);
                    rng_state_ = other.rng_state_;
                    for (auto&& kv : other) {
                        insert(std::move(kv));
                    }
                    other.clear();
                }
            }
            return *this;
        }

        // ------------------------------------------------------------------------
        // Modifiers
        // ------------------------------------------------------------------------
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

        bool erase(const Key& key) {
            return erase_impl(key);
        }

        template <typename K>
            requires requires(const Compare& c, const Key& a, const K& b) { c(a, b); c(b, a); }
        bool erase(const K& key) {
            return erase_impl(key);
        }

        iterator erase(iterator pos) {
            if (pos == end()) {
                return end();
            }
            Key key_copy = pos->first;
            iterator next = pos;
            ++next;
            (void)erase(key_copy);
            return next;
        }

        iterator erase(iterator first, iterator last) {
            while (first != last) {
                first = erase(first);
            }
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
            if (it == end()) {
                throw std::out_of_range("SkipList::at: key not found");
            }
            return it->second;
        }

        const mapped_type& at(const key_type& key) const {
            auto it = find(key);
            if (it == end()) {
                throw std::out_of_range("SkipList::at: key not found");
            }
            return it->second;
        }

        void swap(SkipList& other) noexcept(
            std::is_nothrow_swappable_v<Compare> &&
            byte_allocator_traits::is_always_equal::value &&
            std::is_nothrow_swappable_v<PromotionPolicy>) {
            using std::swap;
            swap(comp_, other.comp_);
            if constexpr (byte_allocator_traits::propagate_on_container_swap::value) {
                swap(alloc_, other.alloc_);
            }
            swap(promotion_policy_, other.promotion_policy_);
            swap(head_sentinel_.forward_links, other.head_sentinel_.forward_links);
            swap(current_level_, other.current_level_);
            swap(size_, other.size_);
            swap(rng_state_, other.rng_state_);
        }

        void clear() noexcept {
            NodeBase* cur = head_sentinel_.get_forward(0);
            while (cur) {
                NodeBase* next = cur->get_forward(0);
                deallocate_node(cur);
                cur = next;
            }
            head_sentinel_.forward_links.fill(nullptr);
            current_level_ = 1;
            size_ = 0;
        }

        // ------------------------------------------------------------------------
        // Lookup
        // ------------------------------------------------------------------------
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

        // ------------------------------------------------------------------------
        // Iterators & Capacity
        // ------------------------------------------------------------------------
        [[nodiscard]] iterator begin() noexcept { return iterator{head_sentinel_.get_forward(0)}; }
        [[nodiscard]] iterator end() noexcept { return iterator{nullptr}; }
        [[nodiscard]] const_iterator begin() const noexcept { return const_iterator{head_sentinel_.get_forward(0)}; }
        [[nodiscard]] const_iterator end() const noexcept { return const_iterator{nullptr}; }
        [[nodiscard]] const_iterator cbegin() const noexcept { return const_iterator{head_sentinel_.get_forward(0)}; }
        [[nodiscard]] const_iterator cend() const noexcept { return const_iterator{nullptr}; }

        [[nodiscard]] size_type size() const noexcept { return size_; }
        [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

        [[nodiscard]] allocator_type get_allocator() const noexcept { return allocator_type{alloc_}; }

    private:
        Compare comp_;
        byte_allocator_type alloc_;
        PromotionPolicy promotion_policy_;
        HeadSentinel head_sentinel_;
        std::size_t current_level_{1};
        std::size_t size_{0};
        std::uint64_t rng_state_{0x9E3779B97F4A7C15ULL};

        std::size_t random_level() noexcept {
            return promotion_policy_(rng_state_);
        }

        template <typename K, typename V>
        [[nodiscard]] NodeBase* allocate_node(K&& key, V&& value, std::size_t lvl) {
            const std::size_t bytes = total_allocation_size(lvl);
            std::byte* raw_mem = byte_allocator_traits::allocate(alloc_, bytes);

            // Construct NodeBase in place at start of buffer
            NodeBase* base = ::new (static_cast<void*>(raw_mem)) NodeBase(lvl);

            // Initialize forward pointers to nullptr
            for (std::size_t i = 0; i < lvl; ++i) {
                base->set_forward(i, nullptr);
            }

            // Construct value_type in place at computed aligned offset
            value_type* val_dest = get_value_ptr(base);
            try {
                ::new (static_cast<void*>(val_dest)) value_type(
                    std::piecewise_construct,
                    std::forward_as_tuple(std::forward<K>(key)),
                    std::forward_as_tuple(std::forward<V>(value))
                );
            }
            catch (...) {
                base->~NodeBase();
                byte_allocator_traits::deallocate(alloc_, raw_mem, bytes);
                throw;
            }
            return base;
        }

        void deallocate_node(NodeBase* node) noexcept {
            const std::size_t lvl = node->level;
            const std::size_t bytes = total_allocation_size(lvl);

            // Destroy payload
            get_value_ptr(node)->~value_type();

            // Destroy NodeBase
            node->~NodeBase();

            // Deallocate raw storage through rebound allocator
            std::byte* raw_mem = reinterpret_cast<std::byte*>(node);
            byte_allocator_traits::deallocate(alloc_, raw_mem, bytes);
        }

        template <typename A, typename B>
        [[nodiscard]] bool keys_equal(const A& lhs, const B& rhs) const {
            return !comp_(lhs, rhs) && !comp_(rhs, lhs);
        }

        // Shared traversal helper: finds predecessors at each level up to current_level_
        // and returns the immediate candidate node at level 0 (if any)
        template <typename K>
        NodeBase* find_predecessors(const K& key, std::array<NodeBase*, MaxLevel>& update) {
            NodeBase* cur = head_sentinel_.as_node_base();
            for (int i = static_cast<int>(current_level_) - 1; i >= 0; --i) {
                NodeBase* nxt = get_node_forward(cur, i);
                while (nxt && comp_(get_value_ptr(nxt)->first, key)) {
                    cur = nxt;
                    nxt = get_node_forward(cur, i);
                }
                update[i] = cur;
            }
            return get_node_forward(cur, 0);
        }

        template <typename K, typename V>
        NodeBase* link_new_node(K&& key, V&& value, std::array<NodeBase*, MaxLevel>& update) {
            std::size_t new_level = random_level();
            if (new_level > current_level_) {
                for (std::size_t i = current_level_; i < new_level; ++i) {
                    update[i] = head_sentinel_.as_node_base();
                }
                current_level_ = new_level;
            }

            NodeBase* new_node = allocate_node(std::forward<K>(key), std::forward<V>(value), new_level);
            for (std::size_t i = 0; i < new_level; ++i) {
                new_node->set_forward(i, get_node_forward(update[i], i));
                set_node_forward(update[i], i, new_node);
            }
            ++size_;
            return new_node;
        }

        template <typename K>
        [[nodiscard]] NodeBase* find_node(const K& key) noexcept {
            NodeBase* cur = head_sentinel_.as_node_base();
            for (int i = static_cast<int>(current_level_) - 1; i >= 0; --i) {
                NodeBase* nxt = get_node_forward(cur, i);
                while (nxt && comp_(get_value_ptr(nxt)->first, key)) {
                    cur = nxt;
                    nxt = get_node_forward(cur, i);
                }
            }
            cur = get_node_forward(cur, 0);
            if (cur && keys_equal(get_value_ptr(cur)->first, key)) {
                return cur;
            }
            return nullptr;
        }

        template <typename K>
        [[nodiscard]] const NodeBase* find_node(const K& key) const noexcept {
            const NodeBase* cur = head_sentinel_.as_node_base();
            for (int i = static_cast<int>(current_level_) - 1; i >= 0; --i) {
                const NodeBase* nxt = get_node_forward(cur, i);
                while (nxt && comp_(get_value_ptr(nxt)->first, key)) {
                    cur = nxt;
                    nxt = get_node_forward(cur, i);
                }
            }
            cur = get_node_forward(cur, 0);
            if (cur && keys_equal(get_value_ptr(cur)->first, key)) {
                return cur;
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
                if (get_node_forward(update[i], i) != victim) break;
                set_node_forward(update[i], i, victim->get_forward(i));
            }

            while (current_level_ > 1 && head_sentinel_.get_forward(current_level_ - 1) == nullptr) {
                --current_level_;
            }

            deallocate_node(victim);
            --size_;
            return true;
        }

        template <typename K>
        [[nodiscard]] iterator lower_bound_impl(const K& key) noexcept {
            NodeBase* cur = head_sentinel_.as_node_base();
            for (int i = static_cast<int>(current_level_) - 1; i >= 0; --i) {
                NodeBase* nxt = get_node_forward(cur, i);
                while (nxt && comp_(get_value_ptr(nxt)->first, key)) {
                    cur = nxt;
                    nxt = get_node_forward(cur, i);
                }
            }
            return iterator{get_node_forward(cur, 0)};
        }

        template <typename K>
        [[nodiscard]] const_iterator lower_bound_impl(const K& key) const noexcept {
            const NodeBase* cur = head_sentinel_.as_node_base();
            for (int i = static_cast<int>(current_level_) - 1; i >= 0; --i) {
                const NodeBase* nxt = get_node_forward(cur, i);
                while (nxt && comp_(get_value_ptr(nxt)->first, key)) {
                    cur = nxt;
                    nxt = get_node_forward(cur, i);
                }
            }
            return const_iterator{get_node_forward(cur, 0)};
        }

        template <typename K>
        [[nodiscard]] iterator upper_bound_impl(const K& key) noexcept {
            NodeBase* cur = head_sentinel_.as_node_base();
            for (int i = static_cast<int>(current_level_) - 1; i >= 0; --i) {
                NodeBase* nxt = get_node_forward(cur, i);
                while (nxt && !comp_(key, get_value_ptr(nxt)->first)) {
                    cur = nxt;
                    nxt = get_node_forward(cur, i);
                }
            }
            return iterator{get_node_forward(cur, 0)};
        }

        template <typename K>
        [[nodiscard]] const_iterator upper_bound_impl(const K& key) const noexcept {
            const NodeBase* cur = head_sentinel_.as_node_base();
            for (int i = static_cast<int>(current_level_) - 1; i >= 0; --i) {
                const NodeBase* nxt = get_node_forward(cur, i);
                while (nxt && !comp_(key, get_value_ptr(nxt)->first)) {
                    cur = nxt;
                    nxt = get_node_forward(cur, i);
                }
            }
            return const_iterator{get_node_forward(cur, 0)};
        }
    };

    template <typename Key, typename Value, typename Compare, std::size_t MaxLevel, typename Allocator,
              typename PromotionPolicy>
    void swap(SkipList<Key, Value, Compare, MaxLevel, Allocator, PromotionPolicy>& a,
              SkipList<Key, Value, Compare, MaxLevel, Allocator, PromotionPolicy>& b) noexcept(noexcept(a.swap(b))) {
        a.swap(b);
    }

    // ============================================================================
    // PMR (Polymorphic Memory Resource) Aliases
    // ============================================================================
    namespace pmr {
        template <
            typename Key,
            typename Value,
            typename Compare = std::less<>,
            std::size_t MaxLevel = 16,
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


