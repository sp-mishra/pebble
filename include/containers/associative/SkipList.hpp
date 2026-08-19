#pragma once
// ============================================================================
// containers/associative/SkipList.hpp — Generic C++23 SkipList Container
// ============================================================================
//
// Modern, policy-based, zero-virtual, high-performance SkipList container.
// Features:
//   - O(log n) search, insert, and delete
//   - O(log n + k) range queries / iteration
//   - Configurable allocator / arena support (Smriti arena-friendly)
//   - Zero macros, zero virtual functions, zero RTTI
// ============================================================================

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace containers {

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
    class SkipList {
    public:
        using key_type = Key;
        using mapped_type = Value;
        using value_type = std::pair<const Key, Value>;
        using key_compare = Compare;
        using size_type = std::size_t;
        using allocator_type = Allocator;

        static_assert(MaxLevel > 0, "SkipList MaxLevel must be > 0");

    private:
        struct NodeBase {
            std::uint8_t level{1};
            std::unique_ptr<NodeBase*[]> forward{};

            explicit NodeBase(const std::size_t lvl = 1)
                : level(static_cast<std::uint8_t>(lvl)), forward(std::make_unique<NodeBase*[]>(lvl)) {
                for (std::size_t i = 0; i < lvl; ++i) {
                    forward[i] = nullptr;
                }
            }

            NodeBase(const NodeBase&) = delete;
            NodeBase& operator=(const NodeBase&) = delete;
            NodeBase(NodeBase&&) noexcept = default;
            NodeBase& operator=(NodeBase&&) noexcept = default;
            ~NodeBase() = default;
        };

        struct DataNode final : NodeBase {
            value_type kv;

            template <typename K, typename V>
            DataNode(K&& key, V&& value, const std::size_t lvl)
                : NodeBase(lvl),
                  kv(std::piecewise_construct,
                     std::forward_as_tuple(std::forward<K>(key)),
                     std::forward_as_tuple(std::forward<V>(value))) {}
        };

        using node_allocator_type = typename std::allocator_traits<allocator_type>::template rebind_alloc<DataNode>;
        using node_allocator_traits = std::allocator_traits<node_allocator_type>;

    public:

        // Forward iterator for SkipList
        class iterator {
        public:
            using iterator_category = std::forward_iterator_tag;
            using value_type = SkipList::value_type;
            using difference_type = std::ptrdiff_t;
            using pointer = value_type*;
            using reference = value_type&;

            iterator() noexcept : node_{nullptr} {}
            explicit iterator(NodeBase* n) noexcept : node_{n} {}

            reference operator*() const noexcept { return as_data(node_)->kv; }

            pointer operator->() const noexcept { return &as_data(node_)->kv; }

            iterator& operator++() noexcept {
                if (node_) node_ = node_->forward[0];
                return *this;
            }

            iterator operator++(int) noexcept {
                iterator tmp = *this;
                ++(*this);
                return tmp;
            }

            bool operator==(const iterator& other) const noexcept { return node_ == other.node_; }
            bool operator!=(const iterator& other) const noexcept { return node_ != other.node_; }

            [[nodiscard]] NodeBase* node() const noexcept { return node_; }

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

            const_iterator() noexcept : node_{nullptr} {}
            explicit const_iterator(const NodeBase* n) noexcept : node_{n} {}
            explicit const_iterator(iterator it) noexcept : node_{it.node()} {}

            reference operator*() const noexcept { return as_data(node_)->kv; }

            pointer operator->() const noexcept { return &as_data(node_)->kv; }

            const_iterator& operator++() noexcept {
                if (node_) node_ = node_->forward[0];
                return *this;
            }

            const_iterator operator++(int) noexcept {
                const_iterator tmp = *this;
                ++(*this);
                return tmp;
            }

            bool operator==(const const_iterator& other) const noexcept { return node_ == other.node_; }
            bool operator!=(const const_iterator& other) const noexcept { return node_ != other.node_; }

        private:
            const NodeBase* node_;
        };

        explicit SkipList(
            Compare comp = Compare{},
            allocator_type alloc = allocator_type{},
            PromotionPolicy promotion_policy = PromotionPolicy{})
            : comp_{std::move(comp)},
              alloc_{std::move(alloc)},
              promotion_policy_{std::move(promotion_policy)},
              head_{MaxLevel},
              rng_state_{0x9E3779B97F4A7C15ULL} {
            for (std::size_t i = 0; i < MaxLevel; ++i) {
                head_.forward[i] = nullptr;
            }
        }

        ~SkipList() {
            clear();
        }

        SkipList(SkipList&& other) noexcept
            : comp_{std::move(other.comp_)},
              alloc_{std::move(other.alloc_)},
              promotion_policy_{std::move(other.promotion_policy_)},
              head_{MaxLevel},
              current_level_{other.current_level_},
              size_{other.size_},
              rng_state_{other.rng_state_} {
            for (std::size_t i = 0; i < MaxLevel; ++i) {
                head_.forward[i] = other.head_.forward[i];
                other.head_.forward[i] = nullptr;
            }
            other.current_level_ = 1;
            other.size_ = 0;
        }

        SkipList(const SkipList& other)
            : comp_{other.comp_},
              alloc_{node_allocator_traits::select_on_container_copy_construction(other.alloc_)},
              promotion_policy_{other.promotion_policy_},
              head_{MaxLevel},
              current_level_{1},
              size_{0},
              rng_state_{other.rng_state_} {
            for (const auto& kv : other) {
                insert(kv);
            }
        }

        SkipList& operator=(const SkipList& other) {
            if (this != &other) {
                clear();
                comp_ = other.comp_;
                if constexpr (node_allocator_traits::propagate_on_container_copy_assignment::value) {
                    alloc_ = other.alloc_;
                }
                promotion_policy_ = other.promotion_policy_;
                rng_state_ = other.rng_state_;
                for (const auto& kv : other) {
                    insert(kv);
                }
            }
            return *this;
        }

        SkipList& operator=(SkipList&& other) noexcept {
            if (this != &other) {
                clear();
                comp_ = std::move(other.comp_);
                alloc_ = std::move(other.alloc_);
                promotion_policy_ = std::move(other.promotion_policy_);
                for (std::size_t i = 0; i < MaxLevel; ++i) {
                    head_.forward[i] = other.head_.forward[i];
                    other.head_.forward[i] = nullptr;
                }
                current_level_ = other.current_level_;
                size_ = other.size_;
                rng_state_ = other.rng_state_;

                other.current_level_ = 1;
                other.size_ = 0;
            }
            return *this;
        }

        // ------------------------------------------------------------------------
        // Modifiers
        // ------------------------------------------------------------------------
        template <typename K, typename V>
        std::pair<iterator, bool> insert_or_assign(K&& key, V&& val) {
            std::array<NodeBase*, MaxLevel> update{};
            DataNode* existing = find_with_update(key, update);

            if (existing && keys_equal(existing->kv.first, key)) {
                existing->kv.second = std::forward<V>(val);
                return {iterator{existing}, false};
            }

            DataNode* new_node = link_new_node(std::forward<K>(key), std::forward<V>(val), update);
            return {iterator{new_node}, true};
        }

        std::pair<iterator, bool> insert(const value_type& value) {
            std::array<NodeBase*, MaxLevel> update{};
            DataNode* existing = find_with_update(value.first, update);
            if (existing && keys_equal(existing->kv.first, value.first)) {
                return {iterator{existing}, false};
            }
            return {iterator{link_new_node(value.first, value.second, update)}, true};
        }

        std::pair<iterator, bool> insert(value_type&& value) {
            std::array<NodeBase*, MaxLevel> update{};
            DataNode* existing = find_with_update(value.first, update);
            if (existing && keys_equal(existing->kv.first, value.first)) {
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
            DataNode* existing = find_with_update(key, update);
            if (existing && keys_equal(existing->kv.first, key)) {
                return {iterator{existing}, false};
            }
            return {iterator{link_new_node(key, Value(std::forward<Args>(args)...), update)}, true};
        }

        template <typename... Args>
        std::pair<iterator, bool> try_emplace(Key&& key, Args&&... args) {
            std::array<NodeBase*, MaxLevel> update{};
            DataNode* existing = find_with_update(key, update);
            if (existing && keys_equal(existing->kv.first, key)) {
                return {iterator{existing}, false};
            }
            return {iterator{link_new_node(std::move(key), Value(std::forward<Args>(args)...), update)}, true};
        }

        bool erase(const Key& key) {
            std::array<NodeBase*, MaxLevel> update{};
            NodeBase* cur = &head_;

            for (int i = static_cast<int>(current_level_) - 1; i >= 0; --i) {
                while (cur->forward[i] && comp_(as_data(cur->forward[i])->kv.first, key)) {
                    cur = cur->forward[i];
                }
                update[i] = cur;
            }

            cur = cur->forward[0];
            DataNode* victim = as_data(cur);

            if (!victim || !keys_equal(victim->kv.first, key)) {
                return false;
            }

            for (std::size_t i = 0; i < current_level_; ++i) {
                if (update[i]->forward[i] != victim) break;
                update[i]->forward[i] = victim->forward[i];
            }

            while (current_level_ > 1 && head_.forward[current_level_ - 1] == nullptr) {
                --current_level_;
            }

            deallocate_node(victim);
            --size_;
            return true;
        }

        template <typename K>
            requires requires(const Compare& c, const Key& a, const K& b) { c(a, b); c(b, a); }
        bool erase(const K& key) {
            std::array<NodeBase*, MaxLevel> update{};
            NodeBase* cur = &head_;

            for (int i = static_cast<int>(current_level_) - 1; i >= 0; --i) {
                while (cur->forward[i] && comp_(as_data(cur->forward[i])->kv.first, key)) {
                    cur = cur->forward[i];
                }
                update[i] = cur;
            }

            cur = cur->forward[0];
            DataNode* victim = as_data(cur);

            if (!victim || !keys_equal(victim->kv.first, key)) {
                return false;
            }

            for (std::size_t i = 0; i < current_level_; ++i) {
                if (update[i]->forward[i] != victim) break;
                update[i]->forward[i] = victim->forward[i];
            }

            while (current_level_ > 1 && head_.forward[current_level_ - 1] == nullptr) {
                --current_level_;
            }

            deallocate_node(victim);
            --size_;
            return true;
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
            std::is_nothrow_swappable_v<node_allocator_type> &&
            std::is_nothrow_swappable_v<PromotionPolicy>) {
            using std::swap;
            swap(comp_, other.comp_);
            swap(alloc_, other.alloc_);
            swap(promotion_policy_, other.promotion_policy_);
            swap(head_, other.head_);
            swap(current_level_, other.current_level_);
            swap(size_, other.size_);
            swap(rng_state_, other.rng_state_);
        }

        void clear() noexcept {
            NodeBase* cur = head_.forward[0];
            while (cur) {
                NodeBase* next = cur->forward[0];
                deallocate_node(as_data(cur));
                cur = next;
            }
            for (std::size_t i = 0; i < MaxLevel; ++i) {
                head_.forward[i] = nullptr;
            }
            current_level_ = 1;
            size_ = 0;
        }

        // ------------------------------------------------------------------------
        // Lookup
        // ------------------------------------------------------------------------
        [[nodiscard]] iterator find(const Key& key) noexcept {
            DataNode* node = find_node(key);
            return node ? iterator{node} : end();
        }

        template <typename K>
            requires requires(const Compare& c, const Key& a, const K& b) { c(a, b); c(b, a); }
        [[nodiscard]] iterator find(const K& key) noexcept {
            DataNode* node = find_node(key);
            return node ? iterator{node} : end();
        }

        [[nodiscard]] const_iterator find(const Key& key) const noexcept {
            const DataNode* node = find_node(key);
            return node ? const_iterator{node} : end();
        }

        template <typename K>
            requires requires(const Compare& c, const Key& a, const K& b) { c(a, b); c(b, a); }
        [[nodiscard]] const_iterator find(const K& key) const noexcept {
            const DataNode* node = find_node(key);
            return node ? const_iterator{node} : end();
        }

        [[nodiscard]] bool contains(const Key& key) const noexcept {
            return find_node(key) != nullptr;
        }

        [[nodiscard]] size_type count(const Key& key) const noexcept {
            return contains(key) ? 1 : 0;
        }

        template <typename K>
            requires requires(const Compare& c, const Key& a, const K& b) { c(a, b); c(b, a); }
        [[nodiscard]] bool contains(const K& key) const noexcept {
            return find_node(key) != nullptr;
        }

        template <typename K>
            requires requires(const Compare& c, const Key& a, const K& b) { c(a, b); c(b, a); }
        [[nodiscard]] size_type count(const K& key) const noexcept {
            return contains(key) ? 1 : 0;
        }

        [[nodiscard]] iterator lower_bound(const Key& key) noexcept {
            NodeBase* cur = &head_;
            for (int i = static_cast<int>(current_level_) - 1; i >= 0; --i) {
                while (cur->forward[i] && comp_(as_data(cur->forward[i])->kv.first, key)) {
                    cur = cur->forward[i];
                }
            }
            return iterator{cur->forward[0]};
        }

        [[nodiscard]] const_iterator lower_bound(const Key& key) const noexcept {
            const NodeBase* cur = &head_;
            for (int i = static_cast<int>(current_level_) - 1; i >= 0; --i) {
                while (cur->forward[i] && comp_(as_data(cur->forward[i])->kv.first, key)) {
                    cur = cur->forward[i];
                }
            }
            return const_iterator{cur->forward[0]};
        }

        template <typename K>
            requires requires(const Compare& c, const Key& a, const K& b) { c(a, b); c(b, a); }
        [[nodiscard]] iterator lower_bound(const K& key) noexcept {
            NodeBase* cur = &head_;
            for (int i = static_cast<int>(current_level_) - 1; i >= 0; --i) {
                while (cur->forward[i] && comp_(as_data(cur->forward[i])->kv.first, key)) {
                    cur = cur->forward[i];
                }
            }
            return iterator{cur->forward[0]};
        }

        template <typename K>
            requires requires(const Compare& c, const Key& a, const K& b) { c(a, b); c(b, a); }
        [[nodiscard]] const_iterator lower_bound(const K& key) const noexcept {
            const NodeBase* cur = &head_;
            for (int i = static_cast<int>(current_level_) - 1; i >= 0; --i) {
                while (cur->forward[i] && comp_(as_data(cur->forward[i])->kv.first, key)) {
                    cur = cur->forward[i];
                }
            }
            return const_iterator{cur->forward[0]};
        }

        [[nodiscard]] iterator upper_bound(const Key& key) noexcept {
            NodeBase* cur = &head_;
            for (int i = static_cast<int>(current_level_) - 1; i >= 0; --i) {
                while (cur->forward[i] && !comp_(key, as_data(cur->forward[i])->kv.first)) {
                    cur = cur->forward[i];
                }
            }
            return iterator{cur->forward[0]};
        }

        [[nodiscard]] const_iterator upper_bound(const Key& key) const noexcept {
            const NodeBase* cur = &head_;
            for (int i = static_cast<int>(current_level_) - 1; i >= 0; --i) {
                while (cur->forward[i] && !comp_(key, as_data(cur->forward[i])->kv.first)) {
                    cur = cur->forward[i];
                }
            }
            return const_iterator{cur->forward[0]};
        }

        template <typename K>
            requires requires(const Compare& c, const Key& a, const K& b) { c(a, b); c(b, a); }
        [[nodiscard]] iterator upper_bound(const K& key) noexcept {
            NodeBase* cur = &head_;
            for (int i = static_cast<int>(current_level_) - 1; i >= 0; --i) {
                while (cur->forward[i] && !comp_(key, as_data(cur->forward[i])->kv.first)) {
                    cur = cur->forward[i];
                }
            }
            return iterator{cur->forward[0]};
        }

        template <typename K>
            requires requires(const Compare& c, const Key& a, const K& b) { c(a, b); c(b, a); }
        [[nodiscard]] const_iterator upper_bound(const K& key) const noexcept {
            const NodeBase* cur = &head_;
            for (int i = static_cast<int>(current_level_) - 1; i >= 0; --i) {
                while (cur->forward[i] && !comp_(key, as_data(cur->forward[i])->kv.first)) {
                    cur = cur->forward[i];
                }
            }
            return const_iterator{cur->forward[0]};
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
        [[nodiscard]] iterator begin() noexcept { return iterator{head_.forward[0]}; }
        [[nodiscard]] iterator end() noexcept { return iterator{nullptr}; }
        [[nodiscard]] const_iterator begin() const noexcept { return const_iterator{head_.forward[0]}; }
        [[nodiscard]] const_iterator end() const noexcept { return const_iterator{nullptr}; }
        [[nodiscard]] const_iterator cbegin() const noexcept { return const_iterator{head_.forward[0]}; }
        [[nodiscard]] const_iterator cend() const noexcept { return const_iterator{nullptr}; }

        [[nodiscard]] size_type size() const noexcept { return size_; }
        [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

        [[nodiscard]] const allocator_type& get_allocator() const noexcept { return alloc_; }

    private:
        Compare comp_;
        node_allocator_type alloc_;
        PromotionPolicy promotion_policy_;
        NodeBase head_;
        std::size_t current_level_{1};
        std::size_t size_{0};
        std::uint64_t rng_state_{0x9E3779B97F4A7C15ULL};

        std::size_t random_level() noexcept {
            return promotion_policy_(rng_state_);
        }

        template <typename K, typename V>
        [[nodiscard]] DataNode* allocate_node(K&& key, V&& value, std::size_t lvl) {
            DataNode* ptr = node_allocator_traits::allocate(alloc_, 1);
            try {
                node_allocator_traits::construct(alloc_, ptr, std::forward<K>(key), std::forward<V>(value), lvl);
            }
            catch (...) {
                node_allocator_traits::deallocate(alloc_, ptr, 1);
                throw;
            }
            return ptr;
        }

        void deallocate_node(DataNode* node) noexcept {
            node_allocator_traits::destroy(alloc_, node);
            node_allocator_traits::deallocate(alloc_, node, 1);
        }

        [[nodiscard]] static DataNode* as_data(NodeBase* base) noexcept {
            return static_cast<DataNode*>(base);
        }

        [[nodiscard]] static const DataNode* as_data(const NodeBase* base) noexcept {
            return static_cast<const DataNode*>(base);
        }

        template <typename A, typename B>
        [[nodiscard]] bool keys_equal(const A& lhs, const B& rhs) const {
            return !comp_(lhs, rhs) && !comp_(rhs, lhs);
        }

        template <typename K>
            requires requires(const Compare& c, const Key& a, const K& b) { c(a, b); c(b, a); }
        DataNode* find_with_update(const K& key, std::array<NodeBase*, MaxLevel>& update) {
            NodeBase* cur = &head_;
            for (int i = static_cast<int>(current_level_) - 1; i >= 0; --i) {
                while (cur->forward[i] && comp_(as_data(cur->forward[i])->kv.first, key)) {
                    cur = cur->forward[i];
                }
                update[i] = cur;
            }
            NodeBase* candidate = cur->forward[0];
            return candidate ? as_data(candidate) : nullptr;
        }

        template <typename K, typename V>
        DataNode* link_new_node(K&& key, V&& value, std::array<NodeBase*, MaxLevel>& update) {
            std::size_t new_level = random_level();
            if (new_level > current_level_) {
                for (std::size_t i = current_level_; i < new_level; ++i) {
                    update[i] = &head_;
                }
                current_level_ = new_level;
            }

            DataNode* new_node = allocate_node(std::forward<K>(key), std::forward<V>(value), new_level);
            for (std::size_t i = 0; i < new_level; ++i) {
                new_node->forward[i] = update[i]->forward[i];
                update[i]->forward[i] = new_node;
            }
            ++size_;
            return new_node;
        }

        [[nodiscard]] DataNode* find_node(const Key& key) noexcept {
            NodeBase* cur = &head_;
            for (int i = static_cast<int>(current_level_) - 1; i >= 0; --i) {
                while (cur->forward[i] && comp_(as_data(cur->forward[i])->kv.first, key)) {
                    cur = cur->forward[i];
                }
            }
            cur = cur->forward[0];
            if (cur && keys_equal(as_data(cur)->kv.first, key)) {
                return as_data(cur);
            }
            return nullptr;
        }

        template <typename K>
            requires requires(const Compare& c, const Key& a, const K& b) { c(a, b); c(b, a); }
        [[nodiscard]] DataNode* find_node(const K& key) noexcept {
            NodeBase* cur = &head_;
            for (int i = static_cast<int>(current_level_) - 1; i >= 0; --i) {
                while (cur->forward[i] && comp_(as_data(cur->forward[i])->kv.first, key)) {
                    cur = cur->forward[i];
                }
            }
            cur = cur->forward[0];
            if (cur && keys_equal(as_data(cur)->kv.first, key)) {
                return as_data(cur);
            }
            return nullptr;
        }

        [[nodiscard]] const DataNode* find_node(const Key& key) const noexcept {
            const NodeBase* cur = &head_;
            for (int i = static_cast<int>(current_level_) - 1; i >= 0; --i) {
                while (cur->forward[i] && comp_(as_data(cur->forward[i])->kv.first, key)) {
                    cur = cur->forward[i];
                }
            }
            cur = cur->forward[0];
            if (cur && keys_equal(as_data(cur)->kv.first, key)) {
                return as_data(cur);
            }
            return nullptr;
        }

        template <typename K>
            requires requires(const Compare& c, const Key& a, const K& b) { c(a, b); c(b, a); }
        [[nodiscard]] const DataNode* find_node(const K& key) const noexcept {
            const NodeBase* cur = &head_;
            for (int i = static_cast<int>(current_level_) - 1; i >= 0; --i) {
                while (cur->forward[i] && comp_(as_data(cur->forward[i])->kv.first, key)) {
                    cur = cur->forward[i];
                }
            }
            cur = cur->forward[0];
            if (cur && keys_equal(as_data(cur)->kv.first, key)) {
                return as_data(cur);
            }
            return nullptr;
        }
    };

    template <typename Key, typename Value, typename Compare, std::size_t MaxLevel, typename Allocator,
              typename PromotionPolicy>
    void swap(SkipList<Key, Value, Compare, MaxLevel, Allocator, PromotionPolicy>& a,
              SkipList<Key, Value, Compare, MaxLevel, Allocator, PromotionPolicy>& b) noexcept(noexcept(a.swap(b))) {
        a.swap(b);
    }

} // namespace containers

