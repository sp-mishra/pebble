#pragma once
// ============================================================================
// bplus_tree.hpp — Generic, High-Performance C++23/C++26 B+ Tree Container
// ============================================================================
// Zero virtual functions, zero RTTI, zero macro anti-patterns.
// Features:
// - Structure-of-Arrays (SoA) Leaf layout for L1/L2 cache efficiency
// - Transparent / Heterogeneous lookups (Compare::is_transparent)
// - Hardware software-prefetch hints for sequential range scans
// - Intrusive node freelist recycling to prevent allocation churn
// - O(N) Bottom-up bulk loading from sorted sequences
// - Policy-based branching factor & Smriti memory arena compatibility
// ============================================================================

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>
#include <span>
#include <vector>

#include "meta/meta.hpp"
#include "mem/smriti.hpp"

#if __has_include(<hwy/highway.h>)
#include <hwy/highway.h>
#define PEBBLE_HAS_HIGHWAY 1
#endif

namespace pebble::containers {

    // ============================================================================
    // SECTION 1 — Traits & Policy Defaults
    // ============================================================================

    namespace detail {
        inline constexpr std::size_t kCacheLineSize = 64;

        inline constexpr std::size_t kMinFanout = 4;
        inline constexpr std::size_t kMaxFanout = 4096;

        // Cache-line-target fanout derivation. Auto-sizes node capacity so a LeafNode/InnerNode
        // stays close to TargetBytes, then clamps into [kMinFanout, kMaxFanout] so pathological
        // (very large or very small) element sizes never produce a degenerate fanout.
        template <typename Key, typename Value, std::size_t TargetBytes = 256>
        consteval std::size_t default_bplus_leaf_capacity() noexcept {
            constexpr std::size_t overhead = sizeof(void*) * 3 + sizeof(std::uint16_t) * 2;
            constexpr std::size_t elem_size = sizeof(Key) + sizeof(Value);
            constexpr std::size_t raw = (TargetBytes > overhead) ? ((TargetBytes - overhead) / elem_size) : kMinFanout;
            return raw < kMinFanout ? kMinFanout : (raw > kMaxFanout ? kMaxFanout : raw);
        }

        template <typename Key, std::size_t TargetBytes = 256>
        consteval std::size_t default_bplus_inner_capacity() noexcept {
            constexpr std::size_t overhead = sizeof(void*) + sizeof(std::uint16_t) * 2;
            constexpr std::size_t elem_size = sizeof(Key) + sizeof(void*);
            constexpr std::size_t raw = (TargetBytes > overhead) ? ((TargetBytes - overhead) / elem_size) : kMinFanout;
            return raw < kMinFanout ? kMinFanout : (raw > kMaxFanout ? kMaxFanout : raw);
        }

        // Branchless hardware prefetch hint
        template <typename T>
        inline void prefetch_read(const T* ptr) noexcept {
#if defined(__GNUC__) || defined(__clang__)
            __builtin_prefetch(ptr, 0, 3);
#endif
        }
    } // namespace detail

    // Default traits: fanout auto-tunes to sizeof(Key)/sizeof(Value) via a TargetNodeBytes
    // budget (capped in the consteval helpers). Provide a custom Traits to override capacities,
    // toggle SIMD, or tune the recycle-pool cap.
    template <typename Key, typename Value, std::size_t TargetNodeBytesV = 256>
    struct DefaultBPlusTreeTraits {
        static constexpr std::size_t TargetNodeBytes = TargetNodeBytesV;
        static constexpr std::size_t LeafCapacity  = detail::default_bplus_leaf_capacity<Key, Value, TargetNodeBytesV>();
        static constexpr std::size_t InnerCapacity = detail::default_bplus_inner_capacity<Key, TargetNodeBytesV>();
        static constexpr bool EnableSIMD = true;
        static constexpr std::size_t MaxRecycleNodes = 64;
    };

    // Formal, discoverable Traits contract. A malformed Traits fails here with a readable
    // message instead of a deep template error deep inside the container body.
    template <typename T>
    concept BPlusTreeTraits = requires {
        { T::LeafCapacity }    -> std::convertible_to<std::size_t>;
        { T::InnerCapacity }   -> std::convertible_to<std::size_t>;
        { T::EnableSIMD }      -> std::convertible_to<bool>;
        { T::MaxRecycleNodes } -> std::convertible_to<std::size_t>;
    };

    // ============================================================================
    // SECTION 2 — SIMD Vector Search Helpers
    // ============================================================================

    namespace simd {
        // Keys for which linear_search_simd has a vectorised path. Single source of truth for
        // the type gate: the container consults this concept, and linear_search_simd's body
        // dispatches on the same set.
        template <typename Key>
        concept simd_searchable =
            std::same_as<Key, std::uint32_t> || std::same_as<Key, std::int32_t> || std::same_as<Key, float> ||
            std::same_as<Key, std::uint64_t> || std::same_as<Key, std::int64_t>;

        template <typename Key>
        [[nodiscard]] inline std::size_t linear_search_simd(const Key* keys, std::size_t count, const Key& target) noexcept {
#if defined(PEBBLE_HAS_HIGHWAY)
            if constexpr (std::same_as<Key, std::uint64_t> || std::same_as<Key, std::int64_t>) {
                namespace hn = hwy::HWY_NAMESPACE;
                const hn::ScalableTag<Key> d;
                const std::size_t N = hn::Lanes(d);

                if (count >= N) {
                    const auto target_vec = hn::Set(d, target);
                    std::size_t i = 0;
                    for (; i + N <= count; i += N) {
                        const auto data = hn::LoadU(d, keys + i);
                        const auto mask = hn::Eq(data, target_vec);
                        if (!hn::AllFalse(d, mask)) {
                            for (std::size_t j = i; j < i + N; ++j) {
                                if (keys[j] == target) return j;
                            }
                        }
                    }
                    for (; i < count; ++i) {
                        if (keys[i] == target) return i;
                    }
                    return count;
                }
            } else if constexpr (std::same_as<Key, std::uint32_t> || std::same_as<Key, std::int32_t> || std::same_as<Key, float>) {
                namespace hn = hwy::HWY_NAMESPACE;
                const hn::ScalableTag<Key> d;
                const std::size_t N = hn::Lanes(d);

                if (count >= N) {
                    const auto target_vec = hn::Set(d, target);
                    std::size_t i = 0;
                    for (; i + N <= count; i += N) {
                        const auto data = hn::LoadU(d, keys + i);
                        const auto mask = hn::Eq(data, target_vec);
                        if (!hn::AllFalse(d, mask)) {
                            for (std::size_t j = i; j < i + N; ++j) {
                                if (keys[j] == target) return j;
                            }
                        }
                    }
                    for (; i < count; ++i) {
                        if (keys[i] == target) return i;
                    }
                    return count;
                }
            }
#endif
            for (std::size_t i = 0; i < count; ++i) {
                if (keys[i] == target) return i;
            }
            return count;
        }
    } // namespace simd

    // ============================================================================
    // SECTION 3 — Node Structures (Structure-of-Arrays Layout)
    // ============================================================================

    enum class NodeType : std::uint8_t {
        Inner = 0,
        Leaf = 1
    };

    struct NodeHeader {
        NodeType type{NodeType::Leaf};
        std::uint16_t count{0};
        std::uint8_t flags{0};

        [[nodiscard]] constexpr bool is_leaf() const noexcept {
            return type == NodeType::Leaf;
        }

        [[nodiscard]] constexpr bool is_inner() const noexcept {
            return type == NodeType::Inner;
        }
    };

    template <typename Key, typename Value, std::size_t LeafCap, typename Allocator>
    struct alignas(detail::kCacheLineSize) LeafNode {
        NodeHeader header{NodeType::Leaf, 0, 0};
        LeafNode* prev{nullptr};
        LeafNode* next{nullptr};
        LeafNode* next_free{nullptr}; // Intrusive freelist link

        // Structure-of-Arrays (SoA) layout: Keys stored contiguously, Values stored contiguously
        alignas(alignof(Key)) std::byte key_storage[LeafCap * sizeof(Key)];
        alignas(alignof(Value)) std::byte val_storage[LeafCap * sizeof(Value)];

        [[nodiscard]] Key* keys() noexcept {
            return reinterpret_cast<Key*>(key_storage);
        }

        [[nodiscard]] const Key* keys() const noexcept {
            return reinterpret_cast<const Key*>(key_storage);
        }

        [[nodiscard]] Value* values() noexcept {
            return reinterpret_cast<Value*>(val_storage);
        }

        [[nodiscard]] const Value* values() const noexcept {
            return reinterpret_cast<const Value*>(val_storage);
        }

        [[nodiscard]] Key& key_at(std::size_t idx) noexcept {
            return keys()[idx];
        }

        [[nodiscard]] const Key& key_at(std::size_t idx) const noexcept {
            return keys()[idx];
        }

        [[nodiscard]] Value& val_at(std::size_t idx) noexcept {
            return values()[idx];
        }

        [[nodiscard]] const Value& val_at(std::size_t idx) const noexcept {
            return values()[idx];
        }

        LeafNode() noexcept = default;

        ~LeafNode() {
            destroy_all();
        }

        void destroy_all() noexcept {
            auto* k = keys();
            auto* v = values();
            for (std::size_t i = 0; i < header.count; ++i) {
                k[i].~Key();
                v[i].~Value();
            }
            header.count = 0;
        }

        LeafNode(const LeafNode&) = delete;
        LeafNode& operator=(const LeafNode&) = delete;
    };

    template <typename Key, std::size_t InnerCap, typename Allocator>
    struct alignas(detail::kCacheLineSize) InnerNode {
        NodeHeader header{NodeType::Inner, 0, 0};
        InnerNode* next_free{nullptr}; // Intrusive freelist link

        // Storage for router keys (InnerCap) and child node pointers (InnerCap + 1)
        alignas(alignof(Key)) std::byte key_storage[InnerCap * sizeof(Key)];
        void* children[InnerCap + 1]{nullptr};

        [[nodiscard]] Key* keys() noexcept {
            return reinterpret_cast<Key*>(key_storage);
        }

        [[nodiscard]] const Key* keys() const noexcept {
            return reinterpret_cast<const Key*>(key_storage);
        }

        [[nodiscard]] Key& key_at(std::size_t idx) noexcept {
            return keys()[idx];
        }

        [[nodiscard]] const Key& key_at(std::size_t idx) const noexcept {
            return keys()[idx];
        }

        InnerNode() noexcept = default;

        ~InnerNode() {
            destroy_all_keys();
        }

        void destroy_all_keys() noexcept {
            auto* k = keys();
            for (std::size_t i = 0; i < header.count; ++i) {
                k[i].~Key();
            }
            header.count = 0;
        }

        InnerNode(const InnerNode&) = delete;
        InnerNode& operator=(const InnerNode&) = delete;
    };

    // ============================================================================
    // SECTION 4 — BPlusTree Iterator with Prefetch
    // ============================================================================

    template <typename Key, typename Value, std::size_t LeafCap, typename Allocator, bool IsConst>
    class BPlusTreeIterator {
    public:
        using iterator_category = std::bidirectional_iterator_tag;
        using difference_type   = std::ptrdiff_t;
        using value_type        = std::pair<const Key&, std::conditional_t<IsConst, const Value&, Value&>>;
        using LeafNodePtr       = std::conditional_t<IsConst, const LeafNode<Key, Value, LeafCap, Allocator>*, LeafNode<Key, Value, LeafCap, Allocator>*>;

        struct ReferenceProxy {
            const Key& first;
            std::conditional_t<IsConst, const Value&, Value&> second;

            constexpr ReferenceProxy(const Key& k, std::conditional_t<IsConst, const Value&, Value&> v)
                : first(k), second(v) {}

            constexpr auto* operator->() noexcept { return this; }
            constexpr const auto* operator->() const noexcept { return this; }
        };

        using reference = ReferenceProxy;
        using pointer   = ReferenceProxy*;

    private:
        LeafNodePtr node_{nullptr};
        std::size_t index_{0};

        template <typename, typename, typename, typename, typename>
        friend class BPlusTree;
        template <typename, typename, std::size_t, typename, bool>
        friend class BPlusTreeIterator;

    public:
        constexpr BPlusTreeIterator() noexcept = default;

        constexpr BPlusTreeIterator(LeafNodePtr node, std::size_t idx) noexcept
            : node_(node), index_(idx) {}

        template <bool OtherConst>
        requires (IsConst && !OtherConst)
        constexpr BPlusTreeIterator(const BPlusTreeIterator<Key, Value, LeafCap, Allocator, OtherConst>& other) noexcept
            : node_(other.node_), index_(other.index_) {}

        [[nodiscard]] constexpr ReferenceProxy operator*() const noexcept {
            return ReferenceProxy(node_->key_at(index_), node_->val_at(index_));
        }

        [[nodiscard]] constexpr ReferenceProxy operator->() const noexcept {
            return ReferenceProxy(node_->key_at(index_), node_->val_at(index_));
        }

        constexpr BPlusTreeIterator& operator++() noexcept {
            if (!node_) return *this;
            ++index_;
            if (index_ >= node_->header.count) {
                node_ = node_->next;
                if (node_ && node_->next) {
                    detail::prefetch_read(node_->next);
                }
                index_ = 0;
            }
            return *this;
        }

        constexpr BPlusTreeIterator operator++(int) noexcept {
            auto tmp = *this;
            ++(*this);
            return tmp;
        }

        constexpr BPlusTreeIterator& operator--() noexcept {
            if (!node_) return *this;
            if (index_ > 0) {
                --index_;
            } else if (node_->prev) {
                node_ = node_->prev;
                index_ = node_->header.count > 0 ? (node_->header.count - 1) : 0;
            }
            return *this;
        }

        constexpr BPlusTreeIterator operator--(int) noexcept {
            auto tmp = *this;
            --(*this);
            return tmp;
        }

        [[nodiscard]] constexpr bool operator==(const BPlusTreeIterator& other) const noexcept {
            return node_ == other.node_ && index_ == other.index_;
        }

        [[nodiscard]] constexpr bool operator!=(const BPlusTreeIterator& other) const noexcept {
            return !(*this == other);
        }

        [[nodiscard]] constexpr LeafNodePtr node() const noexcept { return node_; }
        [[nodiscard]] constexpr std::size_t index() const noexcept { return index_; }
    };

    // ============================================================================
    // SECTION 5 — Core BPlusTree Container
    // ============================================================================

    template <
        typename Key,
        typename Value,
        typename Compare = std::less<Key>,
        typename Traits = DefaultBPlusTreeTraits<Key, Value>,
        typename Allocator = std::allocator<std::pair<const Key, Value>>
    >
    class BPlusTree {
    public:
        using key_type        = Key;
        using mapped_type     = Value;
        using value_type      = std::pair<const Key, Value>;
        using size_type       = std::size_t;
        using difference_type = std::ptrdiff_t;
        using key_compare     = Compare;
        using allocator_type  = Allocator;
        using traits_type     = Traits;

        static constexpr size_type LeafCapacity  = Traits::LeafCapacity;
        static constexpr size_type InnerCapacity = Traits::InnerCapacity;
        static constexpr size_type MaxRecycle    = Traits::MaxRecycleNodes;

        static_assert(BPlusTreeTraits<Traits>, "Traits must provide LeafCapacity, InnerCapacity, EnableSIMD, MaxRecycleNodes");
        static_assert(LeafCapacity >= 3, "BPlusTree LeafCapacity must be at least 3");
        static_assert(InnerCapacity >= 3, "BPlusTree InnerCapacity must be at least 3");

        using LeafType  = LeafNode<Key, Value, LeafCapacity, Allocator>;
        using InnerType = InnerNode<Key, InnerCapacity, Allocator>;

        using iterator               = BPlusTreeIterator<Key, Value, LeafCapacity, Allocator, false>;
        using const_iterator         = BPlusTreeIterator<Key, Value, LeafCapacity, Allocator, true>;
        using reverse_iterator       = std::reverse_iterator<iterator>;
        using const_reverse_iterator = std::reverse_iterator<const_iterator>;

    private:
        using LeafAllocTraits  = std::allocator_traits<typename std::allocator_traits<Allocator>::template rebind_alloc<LeafType>>;
        using InnerAllocTraits = std::allocator_traits<typename std::allocator_traits<Allocator>::template rebind_alloc<InnerType>>;

        using LeafAlloc  = typename std::allocator_traits<Allocator>::template rebind_alloc<LeafType>;
        using InnerAlloc = typename std::allocator_traits<Allocator>::template rebind_alloc<InnerType>;

        [[no_unique_address]] Compare    comp_{};
        [[no_unique_address]] LeafAlloc  leaf_alloc_{};
        [[no_unique_address]] InnerAlloc inner_alloc_{};

        void*      root_{nullptr};
        LeafType*  head_leaf_{nullptr};
        LeafType*  tail_leaf_{nullptr};
        size_type  size_{0};
        size_type  depth_{0};

        // Freelist recycling pools
        LeafType*  free_leaves_{nullptr};
        size_type  free_leaf_count_{0};
        InnerType* free_inners_{nullptr};
        size_type  free_inner_count_{0};

        // Exact-key membership probe within a leaf. Returns the index of an equal key, or
        // header.count if absent. Uses the branchless SIMD linear scan when EnableSIMD is set,
        // the key type is vectorisable, and the comparator is the default homogeneous ordering
        // (SIMD compares with ==, which only matches std::less<Key> semantics — a custom or
        // transparent Compare must fall back to the Compare-based binary search).
        template <typename K>
        [[nodiscard]] std::size_t leaf_find_index(const LeafType* leaf, const K& key) const noexcept {
            if constexpr (Traits::EnableSIMD && simd::simd_searchable<Key> && std::same_as<K, Key> &&
                          std::is_same_v<Compare, std::less<Key>>) {
#if defined(PEBBLE_HAS_HIGHWAY)
                return simd::linear_search_simd<Key>(leaf->keys(), leaf->header.count, key);
#endif
            }
            const std::size_t idx = leaf_lower_bound(leaf, key);
            if (idx < leaf->header.count && !comp_(key, leaf->key_at(idx)) && !comp_(leaf->key_at(idx), key)) {
                return idx;
            }
            return leaf->header.count;
        }

        // Binary search within leaf entries for lower bound (supports heterogeneous comparison)
        template <typename K>
        [[nodiscard]] std::size_t leaf_lower_bound(const LeafType* leaf, const K& key) const noexcept {
            std::size_t low = 0;
            std::size_t high = leaf->header.count;
            const auto* keys = leaf->keys();
            while (low < high) {
                std::size_t mid = low + (high - low) / 2;
                if (comp_(keys[mid], key)) {
                    low = mid + 1;
                } else {
                    high = mid;
                }
            }
            return low;
        }

        // Binary search within inner router keys for child index
        template <typename K>
        [[nodiscard]] std::size_t inner_child_index(const InnerType* inner, const K& key) const noexcept {
            std::size_t low = 0;
            std::size_t high = inner->header.count;
            const auto* keys = inner->keys();
            while (low < high) {
                std::size_t mid = low + (high - low) / 2;
                if (!comp_(key, keys[mid])) {
                    low = mid + 1;
                } else {
                    high = mid;
                }
            }
            return low;
        }

        // Memory allocation with freelist recycling
        [[nodiscard]] LeafType* allocate_leaf() {
            if (free_leaves_) {
                LeafType* ptr = free_leaves_;
                free_leaves_ = free_leaves_->next_free;
                --free_leaf_count_;
                ptr->header.type = NodeType::Leaf;
                ptr->header.count = 0;
                ptr->header.flags = 0;
                ptr->prev = nullptr;
                ptr->next = nullptr;
                ptr->next_free = nullptr;
                return ptr;
            }
            LeafType* ptr = LeafAllocTraits::allocate(leaf_alloc_, 1);
            LeafAllocTraits::construct(leaf_alloc_, ptr);
            return ptr;
        }

        void deallocate_leaf(LeafType* ptr) noexcept {
            if (!ptr) return;
            ptr->destroy_all();
            if (free_leaf_count_ < MaxRecycle) {
                ptr->next_free = free_leaves_;
                free_leaves_ = ptr;
                ++free_leaf_count_;
            } else {
                LeafAllocTraits::destroy(leaf_alloc_, ptr);
                LeafAllocTraits::deallocate(leaf_alloc_, ptr, 1);
            }
        }

        [[nodiscard]] InnerType* allocate_inner() {
            if (free_inners_) {
                InnerType* ptr = free_inners_;
                free_inners_ = free_inners_->next_free;
                --free_inner_count_;
                ptr->header.type = NodeType::Inner;
                ptr->header.count = 0;
                ptr->header.flags = 0;
                ptr->next_free = nullptr;
                for (std::size_t i = 0; i <= InnerCapacity; ++i) ptr->children[i] = nullptr;
                return ptr;
            }
            InnerType* ptr = InnerAllocTraits::allocate(inner_alloc_, 1);
            InnerAllocTraits::construct(inner_alloc_, ptr);
            return ptr;
        }

        void deallocate_inner(InnerType* ptr) noexcept {
            if (!ptr) return;
            ptr->destroy_all_keys();
            if (free_inner_count_ < MaxRecycle) {
                ptr->next_free = free_inners_;
                free_inners_ = ptr;
                ++free_inner_count_;
            } else {
                InnerAllocTraits::destroy(inner_alloc_, ptr);
                InnerAllocTraits::deallocate(inner_alloc_, ptr, 1);
            }
        }

        void drain_freelist() noexcept {
            while (free_leaves_) {
                LeafType* next = free_leaves_->next_free;
                LeafAllocTraits::destroy(leaf_alloc_, free_leaves_);
                LeafAllocTraits::deallocate(leaf_alloc_, free_leaves_, 1);
                free_leaves_ = next;
            }
            free_leaf_count_ = 0;

            while (free_inners_) {
                InnerType* next = free_inners_->next_free;
                InnerAllocTraits::destroy(inner_alloc_, free_inners_);
                InnerAllocTraits::deallocate(inner_alloc_, free_inners_, 1);
                free_inners_ = next;
            }
            free_inner_count_ = 0;
        }

        void destroy_subtree(void* node_ptr, bool is_leaf) noexcept {
            if (!node_ptr) return;
            if (is_leaf) {
                deallocate_leaf(static_cast<LeafType*>(node_ptr));
            } else {
                auto* inner = static_cast<InnerType*>(node_ptr);
                for (std::size_t i = 0; i <= inner->header.count; ++i) {
                    if (inner->children[i]) {
                        destroy_subtree(inner->children[i], static_cast<NodeHeader*>(inner->children[i])->is_leaf());
                    }
                }
                deallocate_inner(inner);
            }
        }

        struct SplitResult {
            Key   promoted_key;
            void* new_right_node;
        };

        // Split a leaf node (SoA layout)
        [[nodiscard]] SplitResult split_leaf(LeafType* left) {
            LeafType* right = allocate_leaf();
            const std::size_t total = left->header.count;
            const std::size_t split_idx = total / 2;
            const std::size_t move_count = total - split_idx;

            auto* left_keys = left->keys();
            auto* left_vals = left->values();
            auto* right_keys = right->keys();
            auto* right_vals = right->values();

            for (std::size_t i = 0; i < move_count; ++i) {
                std::construct_at(&right_keys[i], std::move(left_keys[split_idx + i]));
                left_keys[split_idx + i].~Key();

                std::construct_at(&right_vals[i], std::move(left_vals[split_idx + i]));
                left_vals[split_idx + i].~Value();
            }

            right->header.count = static_cast<std::uint16_t>(move_count);
            left->header.count  = static_cast<std::uint16_t>(split_idx);

            // Sibling linking
            right->next = left->next;
            right->prev = left;
            if (left->next) {
                left->next->prev = right;
            } else {
                tail_leaf_ = right;
            }
            left->next = right;

            return SplitResult{
                .promoted_key = right->key_at(0),
                .new_right_node = right
            };
        }

        // Split an inner node
        [[nodiscard]] SplitResult split_inner(InnerType* left) {
            InnerType* right = allocate_inner();
            const std::size_t total = left->header.count;
            const std::size_t split_idx = total / 2;

            Key promoted = std::move(left->key_at(split_idx));
            left->key_at(split_idx).~Key();

            const std::size_t move_count = total - split_idx - 1;
            auto* left_keys = left->keys();
            auto* right_keys = right->keys();

            for (std::size_t i = 0; i < move_count; ++i) {
                std::construct_at(&right_keys[i], std::move(left_keys[split_idx + 1 + i]));
                left_keys[split_idx + 1 + i].~Key();
            }

            for (std::size_t i = 0; i <= move_count; ++i) {
                right->children[i] = left->children[split_idx + 1 + i];
                left->children[split_idx + 1 + i] = nullptr;
            }

            right->header.count = static_cast<std::uint16_t>(move_count);
            left->header.count  = static_cast<std::uint16_t>(split_idx);

            return SplitResult{
                .promoted_key = std::move(promoted),
                .new_right_node = right
            };
        }

        // Recursive insert helper (SoA layout)
        template <typename K, typename V>
        std::pair<iterator, std::optional<SplitResult>> insert_recursive(void* current, bool is_leaf, K&& key, V&& val, bool overwrite) {
            if (is_leaf) {
                auto* leaf = static_cast<LeafType*>(current);
                const std::size_t idx = leaf_lower_bound(leaf, key);

                if (idx < leaf->header.count && !comp_(key, leaf->key_at(idx)) && !comp_(leaf->key_at(idx), key)) {
                    if (overwrite) {
                        leaf->val_at(idx) = std::forward<V>(val);
                    }
                    return {iterator(leaf, idx), std::nullopt};
                }

                auto* keys = leaf->keys();
                auto* vals = leaf->values();

                for (std::size_t i = leaf->header.count; i > idx; --i) {
                    std::construct_at(&keys[i], std::move(keys[i - 1]));
                    keys[i - 1].~Key();

                    std::construct_at(&vals[i], std::move(vals[i - 1]));
                    vals[i - 1].~Value();
                }

                std::construct_at(&keys[idx], std::forward<K>(key));
                std::construct_at(&vals[idx], std::forward<V>(val));
                ++leaf->header.count;
                ++size_;

                iterator it(leaf, idx);

                if (leaf->header.count >= LeafCapacity) {
                    return {it, split_leaf(leaf)};
                }
                return {it, std::nullopt};
            }

            auto* inner = static_cast<InnerType*>(current);
            const std::size_t child_idx = inner_child_index(inner, key);
            void* child_node = inner->children[child_idx];
            bool child_is_leaf = static_cast<NodeHeader*>(child_node)->is_leaf();

            auto [result_it, maybe_split] = insert_recursive(child_node, child_is_leaf, std::forward<K>(key), std::forward<V>(val), overwrite);

            if (!maybe_split.has_value()) {
                return {result_it, std::nullopt};
            }

            auto split = std::move(*maybe_split);
            auto* keys = inner->keys();

            for (std::size_t i = inner->header.count; i > child_idx; --i) {
                std::construct_at(&keys[i], std::move(keys[i - 1]));
                keys[i - 1].~Key();
                inner->children[i + 1] = inner->children[i];
            }

            std::construct_at(&keys[child_idx], std::move(split.promoted_key));
            inner->children[child_idx + 1] = split.new_right_node;
            ++inner->header.count;

            if (inner->header.count >= InnerCapacity) {
                return {result_it, split_inner(inner)};
            }

            return {result_it, std::nullopt};
        }

        // Deletion underflow and merge/borrow handlers
        template <typename K>
        bool erase_recursive(void* current, bool is_leaf, const K& key, InnerType* parent, std::size_t parent_idx) {
            if (is_leaf) {
                auto* leaf = static_cast<LeafType*>(current);
                const std::size_t idx = leaf_lower_bound(leaf, key);

                if (idx >= leaf->header.count || comp_(key, leaf->key_at(idx)) || comp_(leaf->key_at(idx), key)) {
                    return false;
                }

                auto* keys = leaf->keys();
                auto* vals = leaf->values();

                keys[idx].~Key();
                vals[idx].~Value();

                for (std::size_t i = idx; i + 1 < leaf->header.count; ++i) {
                    std::construct_at(&keys[i], std::move(keys[i + 1]));
                    keys[i + 1].~Key();

                    std::construct_at(&vals[i], std::move(vals[i + 1]));
                    vals[i + 1].~Value();
                }
                --leaf->header.count;
                --size_;

                if (parent && leaf->header.count < (LeafCapacity / 2)) {
                    rebalance_leaf(leaf, parent, parent_idx);
                }
                return true;
            }

            auto* inner = static_cast<InnerType*>(current);
            const std::size_t child_idx = inner_child_index(inner, key);
            void* child_node = inner->children[child_idx];
            bool child_is_leaf = static_cast<NodeHeader*>(child_node)->is_leaf();

            bool removed = erase_recursive(child_node, child_is_leaf, key, inner, child_idx);
            if (!removed) return false;

            if (parent && inner->header.count < (InnerCapacity / 2)) {
                rebalance_inner(inner, parent, parent_idx);
            }
            return true;
        }

        void rebalance_leaf(LeafType* leaf, InnerType* parent, std::size_t parent_idx) {
            if (parent_idx > 0) {
                auto* left = static_cast<LeafType*>(parent->children[parent_idx - 1]);
                if (left->header.count > (LeafCapacity / 2)) {
                    auto* leaf_keys = leaf->keys();
                    auto* leaf_vals = leaf->values();
                    auto* left_keys = left->keys();
                    auto* left_vals = left->values();

                    for (std::size_t i = leaf->header.count; i > 0; --i) {
                        std::construct_at(&leaf_keys[i], std::move(leaf_keys[i - 1]));
                        leaf_keys[i - 1].~Key();
                        std::construct_at(&leaf_vals[i], std::move(leaf_vals[i - 1]));
                        leaf_vals[i - 1].~Value();
                    }

                    std::construct_at(&leaf_keys[0], std::move(left_keys[left->header.count - 1]));
                    left_keys[left->header.count - 1].~Key();
                    std::construct_at(&leaf_vals[0], std::move(left_vals[left->header.count - 1]));
                    left_vals[left->header.count - 1].~Value();

                    ++leaf->header.count;
                    --left->header.count;

                    parent->key_at(parent_idx - 1) = leaf->key_at(0);
                    return;
                }
            }

            if (parent_idx < parent->header.count) {
                auto* right = static_cast<LeafType*>(parent->children[parent_idx + 1]);
                if (right->header.count > (LeafCapacity / 2)) {
                    auto* leaf_keys = leaf->keys();
                    auto* leaf_vals = leaf->values();
                    auto* right_keys = right->keys();
                    auto* right_vals = right->values();

                    std::construct_at(&leaf_keys[leaf->header.count], std::move(right_keys[0]));
                    right_keys[0].~Key();
                    std::construct_at(&leaf_vals[leaf->header.count], std::move(right_vals[0]));
                    right_vals[0].~Value();

                    for (std::size_t i = 0; i + 1 < right->header.count; ++i) {
                        std::construct_at(&right_keys[i], std::move(right_keys[i + 1]));
                        right_keys[i + 1].~Key();
                        std::construct_at(&right_vals[i], std::move(right_vals[i + 1]));
                        right_vals[i + 1].~Value();
                    }

                    ++leaf->header.count;
                    --right->header.count;

                    parent->key_at(parent_idx) = right->key_at(0);
                    return;
                }
            }

            if (parent_idx > 0) {
                auto* left = static_cast<LeafType*>(parent->children[parent_idx - 1]);
                auto* left_keys = left->keys();
                auto* left_vals = left->values();
                auto* leaf_keys = leaf->keys();
                auto* leaf_vals = leaf->values();

                for (std::size_t i = 0; i < leaf->header.count; ++i) {
                    std::construct_at(&left_keys[left->header.count + i], std::move(leaf_keys[i]));
                    leaf_keys[i].~Key();
                    std::construct_at(&left_vals[left->header.count + i], std::move(leaf_vals[i]));
                    leaf_vals[i].~Value();
                }
                left->header.count += leaf->header.count;
                leaf->header.count = 0;

                left->next = leaf->next;
                if (leaf->next) {
                    leaf->next->prev = left;
                } else {
                    tail_leaf_ = left;
                }

                auto* parent_keys = parent->keys();
                parent_keys[parent_idx - 1].~Key();
                for (std::size_t i = parent_idx - 1; i + 1 < parent->header.count; ++i) {
                    std::construct_at(&parent_keys[i], std::move(parent_keys[i + 1]));
                    parent_keys[i + 1].~Key();
                    parent->children[i + 1] = parent->children[i + 2];
                }
                --parent->header.count;
                deallocate_leaf(leaf);
            } else if (parent_idx < parent->header.count) {
                auto* right = static_cast<LeafType*>(parent->children[parent_idx + 1]);
                auto* leaf_keys = leaf->keys();
                auto* leaf_vals = leaf->values();
                auto* right_keys = right->keys();
                auto* right_vals = right->values();

                for (std::size_t i = 0; i < right->header.count; ++i) {
                    std::construct_at(&leaf_keys[leaf->header.count + i], std::move(right_keys[i]));
                    right_keys[i].~Key();
                    std::construct_at(&leaf_vals[leaf->header.count + i], std::move(right_vals[i]));
                    right_vals[i].~Value();
                }
                leaf->header.count += right->header.count;
                right->header.count = 0;

                leaf->next = right->next;
                if (right->next) {
                    right->next->prev = leaf;
                } else {
                    tail_leaf_ = leaf;
                }

                auto* parent_keys = parent->keys();
                parent_keys[parent_idx].~Key();
                for (std::size_t i = parent_idx; i + 1 < parent->header.count; ++i) {
                    std::construct_at(&parent_keys[i], std::move(parent_keys[i + 1]));
                    parent_keys[i + 1].~Key();
                    parent->children[i + 1] = parent->children[i + 2];
                }
                --parent->header.count;
                deallocate_leaf(right);
            }
        }

        void rebalance_inner(InnerType* inner, InnerType* parent, std::size_t parent_idx) {
            if (parent_idx > 0) {
                auto* left = static_cast<InnerType*>(parent->children[parent_idx - 1]);
                if (left->header.count > (InnerCapacity / 2)) {
                    auto* inner_keys = inner->keys();
                    auto* left_keys = left->keys();

                    for (std::size_t i = inner->header.count; i > 0; --i) {
                        std::construct_at(&inner_keys[i], std::move(inner_keys[i - 1]));
                        inner_keys[i - 1].~Key();
                        inner->children[i + 1] = inner->children[i];
                    }
                    inner->children[1] = inner->children[0];

                    std::construct_at(&inner_keys[0], std::move(parent->key_at(parent_idx - 1)));
                    inner->children[0] = left->children[left->header.count];

                    parent->key_at(parent_idx - 1) = std::move(left_keys[left->header.count - 1]);
                    left_keys[left->header.count - 1].~Key();

                    ++inner->header.count;
                    --left->header.count;
                    return;
                }
            }

            if (parent_idx < parent->header.count) {
                auto* right = static_cast<InnerType*>(parent->children[parent_idx + 1]);
                if (right->header.count > (InnerCapacity / 2)) {
                    auto* inner_keys = inner->keys();
                    auto* right_keys = right->keys();

                    std::construct_at(&inner_keys[inner->header.count], std::move(parent->key_at(parent_idx)));
                    inner->children[inner->header.count + 1] = right->children[0];

                    parent->key_at(parent_idx) = std::move(right_keys[0]);
                    right_keys[0].~Key();

                    for (std::size_t i = 0; i + 1 < right->header.count; ++i) {
                        std::construct_at(&right_keys[i], std::move(right_keys[i + 1]));
                        right_keys[i + 1].~Key();
                        right->children[i] = right->children[i + 1];
                    }
                    right->children[right->header.count - 1] = right->children[right->header.count];

                    ++inner->header.count;
                    --right->header.count;
                    return;
                }
            }

            if (parent_idx > 0) {
                auto* left = static_cast<InnerType*>(parent->children[parent_idx - 1]);
                auto* left_keys = left->keys();
                auto* inner_keys = inner->keys();

                std::construct_at(&left_keys[left->header.count], std::move(parent->key_at(parent_idx - 1)));
                left->children[left->header.count + 1] = inner->children[0];
                ++left->header.count;

                for (std::size_t i = 0; i < inner->header.count; ++i) {
                    std::construct_at(&left_keys[left->header.count + i], std::move(inner_keys[i]));
                    inner_keys[i].~Key();
                    left->children[left->header.count + 1 + i] = inner->children[i + 1];
                }
                left->header.count += inner->header.count;

                auto* parent_keys = parent->keys();
                parent_keys[parent_idx - 1].~Key();
                for (std::size_t i = parent_idx - 1; i + 1 < parent->header.count; ++i) {
                    std::construct_at(&parent_keys[i], std::move(parent_keys[i + 1]));
                    parent_keys[i + 1].~Key();
                    parent->children[i + 1] = parent->children[i + 2];
                }
                --parent->header.count;
                deallocate_inner(inner);
            } else if (parent_idx < parent->header.count) {
                auto* right = static_cast<InnerType*>(parent->children[parent_idx + 1]);
                auto* inner_keys = inner->keys();
                auto* right_keys = right->keys();

                std::construct_at(&inner_keys[inner->header.count], std::move(parent->key_at(parent_idx)));
                inner->children[inner->header.count + 1] = right->children[0];
                ++inner->header.count;

                for (std::size_t i = 0; i < right->header.count; ++i) {
                    std::construct_at(&inner_keys[inner->header.count + i], std::move(right_keys[i]));
                    right_keys[i].~Key();
                    inner->children[inner->header.count + 1 + i] = right->children[i + 1];
                }
                inner->header.count += right->header.count;

                auto* parent_keys = parent->keys();
                parent_keys[parent_idx].~Key();
                for (std::size_t i = parent_idx; i + 1 < parent->header.count; ++i) {
                    std::construct_at(&parent_keys[i], std::move(parent_keys[i + 1]));
                    parent_keys[i + 1].~Key();
                    parent->children[i + 1] = parent->children[i + 2];
                }
                --parent->header.count;
                deallocate_inner(right);
            }
        }

        // O(N) bottom-up bulk build from a sorted, unique key range. Fills leaves left-to-right
        // to a target fill factor (leaving split headroom), links the leaf chain, then builds each
        // inner level from the layer below until a single root remains. Tree must be empty.
        template <std::forward_iterator ForwardIt>
        void build_bottom_up(ForwardIt first, ForwardIt last) {
            if (first == last) return;

            constexpr std::size_t leaf_fill  = (LeafCapacity * 9) / 10 >= 1 ? (LeafCapacity * 9) / 10 : 1;
            constexpr std::size_t inner_fill = (InnerCapacity * 9) / 10 >= 2 ? (InnerCapacity * 9) / 10 : 2;

            // Level 0: pack sorted entries into leaves.
            std::vector<LeafType*> leaves;
            LeafType* prev = nullptr;
            LeafType* curr = nullptr;
            std::size_t in_leaf = 0;

            for (; first != last; ++first) {
                if (!curr || in_leaf == leaf_fill) {
                    curr = allocate_leaf();
                    curr->prev = prev;
                    if (prev) prev->next = curr;
                    else head_leaf_ = curr;
                    prev = curr;
                    leaves.push_back(curr);
                    in_leaf = 0;
                }
                std::construct_at(&curr->keys()[in_leaf], first->first);
                std::construct_at(&curr->values()[in_leaf], first->second);
                ++in_leaf;
                curr->header.count = static_cast<std::uint16_t>(in_leaf);
                ++size_;
            }
            tail_leaf_ = prev;

            if (leaves.size() == 1) {
                root_ = leaves.front();
                depth_ = 1;
                return;
            }

            // Build inner levels bottom-up. Each level groups the children below into inner nodes,
            // promoting the first key of every non-leading child as a router.
            std::vector<void*> children(leaves.begin(), leaves.end());
            depth_ = 1;

            while (children.size() > 1) {
                std::vector<void*> parents;
                std::size_t i = 0;
                const std::size_t n = children.size();

                while (i < n) {
                    const std::size_t group = std::min(inner_fill + 1, n - i);
                    InnerType* node = allocate_inner();
                    node->children[0] = children[i];
                    auto* keys = node->keys();
                    for (std::size_t j = 1; j < group; ++j) {
                        std::construct_at(&keys[j - 1], leftmost_key(children[i + j]));
                        node->children[j] = children[i + j];
                    }
                    node->header.count = static_cast<std::uint16_t>(group - 1);
                    parents.push_back(node);
                    i += group;
                }

                children.swap(parents);
                ++depth_;
            }
            root_ = children.front();
        }

        // Leftmost (smallest) key of a subtree, used to derive router keys during bulk build.
        [[nodiscard]] static const Key& leftmost_key(void* node) noexcept {
            while (!static_cast<NodeHeader*>(node)->is_leaf()) {
                node = static_cast<InnerType*>(node)->children[0];
            }
            return static_cast<LeafType*>(node)->key_at(0);
        }

    public:
        // Constructors & Destructor
        constexpr BPlusTree() noexcept(std::is_nothrow_default_constructible_v<Compare> && std::is_nothrow_default_constructible_v<Allocator>)
            : comp_(), leaf_alloc_(), inner_alloc_(), root_(nullptr), head_leaf_(nullptr), tail_leaf_(nullptr), size_(0), depth_(0),
              free_leaves_(nullptr), free_leaf_count_(0), free_inners_(nullptr), free_inner_count_(0) {}

        explicit BPlusTree(const Compare& comp, const Allocator& alloc = Allocator())
            : comp_(comp), leaf_alloc_(alloc), inner_alloc_(alloc), root_(nullptr), head_leaf_(nullptr), tail_leaf_(nullptr), size_(0), depth_(0),
              free_leaves_(nullptr), free_leaf_count_(0), free_inners_(nullptr), free_inner_count_(0) {}

        explicit BPlusTree(const Allocator& alloc)
            : comp_(), leaf_alloc_(alloc), inner_alloc_(alloc), root_(nullptr), head_leaf_(nullptr), tail_leaf_(nullptr), size_(0), depth_(0),
              free_leaves_(nullptr), free_leaf_count_(0), free_inners_(nullptr), free_inner_count_(0) {}

        template <std::input_iterator InputIt>
        BPlusTree(InputIt first, InputIt last, const Compare& comp = Compare(), const Allocator& alloc = Allocator())
            : BPlusTree(comp, alloc) {
            for (; first != last; ++first) {
                insert(*first);
            }
        }

        BPlusTree(std::initializer_list<value_type> init, const Compare& comp = Compare(), const Allocator& alloc = Allocator())
            : BPlusTree(init.begin(), init.end(), comp, alloc) {}

        ~BPlusTree() {
            clear();
            drain_freelist();
        }

        // Copy constructor & assignment
        BPlusTree(const BPlusTree& other)
            : comp_(other.comp_), leaf_alloc_(other.leaf_alloc_), inner_alloc_(other.inner_alloc_) {
            for (const auto& [k, v] : other) {
                insert_or_assign(k, v);
            }
        }

        BPlusTree& operator=(const BPlusTree& other) {
            if (this != &other) {
                clear();
                comp_ = other.comp_;
                leaf_alloc_ = other.leaf_alloc_;
                inner_alloc_ = other.inner_alloc_;
                for (const auto& [k, v] : other) {
                    insert_or_assign(k, v);
                }
            }
            return *this;
        }

        // Move constructor & assignment
        BPlusTree(BPlusTree&& other) noexcept
            : comp_(std::move(other.comp_)),
              leaf_alloc_(std::move(other.leaf_alloc_)),
              inner_alloc_(std::move(other.inner_alloc_)),
              root_(std::exchange(other.root_, nullptr)),
              head_leaf_(std::exchange(other.head_leaf_, nullptr)),
              tail_leaf_(std::exchange(other.tail_leaf_, nullptr)),
              size_(std::exchange(other.size_, 0)),
              depth_(std::exchange(other.depth_, 0)),
              free_leaves_(std::exchange(other.free_leaves_, nullptr)),
              free_leaf_count_(std::exchange(other.free_leaf_count_, 0)),
              free_inners_(std::exchange(other.free_inners_, nullptr)),
              free_inner_count_(std::exchange(other.free_inner_count_, 0)) {}

        BPlusTree& operator=(BPlusTree&& other) noexcept {
            if (this != &other) {
                clear();
                drain_freelist();
                comp_ = std::move(other.comp_);
                leaf_alloc_ = std::move(other.leaf_alloc_);
                inner_alloc_ = std::move(other.inner_alloc_);
                root_ = std::exchange(other.root_, nullptr);
                head_leaf_ = std::exchange(other.head_leaf_, nullptr);
                tail_leaf_ = std::exchange(other.tail_leaf_, nullptr);
                size_ = std::exchange(other.size_, 0);
                depth_ = std::exchange(other.depth_, 0);
                free_leaves_ = std::exchange(other.free_leaves_, nullptr);
                free_leaf_count_ = std::exchange(other.free_leaf_count_, 0);
                free_inners_ = std::exchange(other.free_inners_, nullptr);
                free_inner_count_ = std::exchange(other.free_inner_count_, 0);
            }
            return *this;
        }

        // O(N) Bottom-Up Bulk Loading Factory Method.
        // Precondition: [first, last) is sorted by key and unique (implied by the name).
        template <std::forward_iterator ForwardIt>
        [[nodiscard]] static BPlusTree from_sorted(ForwardIt first, ForwardIt last, const Compare& comp = Compare(), const Allocator& alloc = Allocator()) {
            BPlusTree tree(comp, alloc);
            tree.build_bottom_up(first, last);
            return tree;
        }

        // Iterators
        [[nodiscard]] iterator begin() noexcept {
            return iterator(head_leaf_, 0);
        }

        [[nodiscard]] const_iterator begin() const noexcept {
            return const_iterator(head_leaf_, 0);
        }

        [[nodiscard]] const_iterator cbegin() const noexcept {
            return const_iterator(head_leaf_, 0);
        }

        [[nodiscard]] iterator end() noexcept {
            return iterator(nullptr, 0);
        }

        [[nodiscard]] const_iterator end() const noexcept {
            return const_iterator(nullptr, 0);
        }

        [[nodiscard]] const_iterator cend() const noexcept {
            return const_iterator(nullptr, 0);
        }

        [[nodiscard]] reverse_iterator rbegin() noexcept {
            return reverse_iterator(end());
        }

        [[nodiscard]] const_reverse_iterator rbegin() const noexcept {
            return const_reverse_iterator(end());
        }

        [[nodiscard]] const_reverse_iterator crbegin() const noexcept {
            return const_reverse_iterator(cend());
        }

        [[nodiscard]] reverse_iterator rend() noexcept {
            return reverse_iterator(begin());
        }

        [[nodiscard]] const_reverse_iterator rend() const noexcept {
            return const_reverse_iterator(begin());
        }

        [[nodiscard]] const_reverse_iterator crend() const noexcept {
            return const_reverse_iterator(cbegin());
        }

        // Capacity
        [[nodiscard]] bool empty() const noexcept {
            return size_ == 0;
        }

        [[nodiscard]] size_type size() const noexcept {
            return size_;
        }

        [[nodiscard]] size_type depth() const noexcept {
            return depth_;
        }

        // Modifiers
        void clear() noexcept {
            if (root_) {
                destroy_subtree(root_, static_cast<NodeHeader*>(root_)->is_leaf());
                root_ = nullptr;
                head_leaf_ = nullptr;
                tail_leaf_ = nullptr;
                size_ = 0;
                depth_ = 0;
            }
        }

        template <typename K, typename V>
        std::pair<iterator, bool> insert_or_assign(K&& key, V&& val) {
            if (!root_) {
                LeafType* leaf = allocate_leaf();
                auto* keys = leaf->keys();
                auto* vals = leaf->values();
                std::construct_at(&keys[0], std::forward<K>(key));
                std::construct_at(&vals[0], std::forward<V>(val));
                leaf->header.count = 1;
                root_ = leaf;
                head_leaf_ = leaf;
                tail_leaf_ = leaf;
                size_ = 1;
                depth_ = 1;
                return {iterator(leaf, 0), true};
            }

            const size_type prev_size = size_;
            bool root_is_leaf = static_cast<NodeHeader*>(root_)->is_leaf();

            auto [it, maybe_split] = insert_recursive(root_, root_is_leaf, std::forward<K>(key), std::forward<V>(val), true);

            if (maybe_split.has_value()) {
                auto split = std::move(*maybe_split);
                InnerType* new_root = allocate_inner();
                auto* root_keys = new_root->keys();
                std::construct_at(&root_keys[0], std::move(split.promoted_key));
                new_root->children[0] = root_;
                new_root->children[1] = split.new_right_node;
                new_root->header.count = 1;

                root_ = new_root;
                ++depth_;
            }

            return {it, size_ > prev_size};
        }

        template <typename... Args>
        std::pair<iterator, bool> emplace(Args&&... args) {
            value_type val(std::forward<Args>(args)...);
            return insert_or_assign(std::move(const_cast<Key&>(val.first)), std::move(val.second));
        }

        std::pair<iterator, bool> insert(const value_type& val) {
            return insert_or_assign(val.first, val.second);
        }

        std::pair<iterator, bool> insert(value_type&& val) {
            return insert_or_assign(std::move(const_cast<Key&>(val.first)), std::move(val.second));
        }

        // Heterogeneous / Transparent Erase
        template <typename K>
        size_type erase(const K& key) {
            if (!root_) return 0;

            const size_type prev_size = size_;
            bool root_is_leaf = static_cast<NodeHeader*>(root_)->is_leaf();

            erase_recursive(root_, root_is_leaf, key, nullptr, 0);

            if (root_ && !static_cast<NodeHeader*>(root_)->is_leaf()) {
                auto* inner_root = static_cast<InnerType*>(root_);
                if (inner_root->header.count == 0) {
                    void* old_root = root_;
                    root_ = inner_root->children[0];
                    deallocate_inner(inner_root);
                    --depth_;
                }
            }

            if (size_ == 0) {
                clear();
            }

            return prev_size - size_;
        }

        iterator erase(const_iterator pos) {
            if (pos == end()) return end();
            Key k = pos->first;
            auto next_it = std::next(pos);
            // Capture the successor key without requiring Key to be default-constructible.
            // A re-descent after erase is necessary because erase may merge/rebalance the
            // successor's leaf, invalidating a cached node pointer.
            std::optional<Key> succ;
            if (next_it != end()) {
                succ.emplace(next_it->first);
            }
            erase(k);
            return succ ? find(*succ) : end();
        }

        // Heterogeneous / Transparent Lookups
        template <typename K>
        [[nodiscard]] iterator find(const K& key) noexcept {
            if (!root_) return end();

            void* curr = root_;
            while (!static_cast<NodeHeader*>(curr)->is_leaf()) {
                auto* inner = static_cast<InnerType*>(curr);
                const std::size_t idx = inner_child_index(inner, key);
                curr = inner->children[idx];
            }

            auto* leaf = static_cast<LeafType*>(curr);
            const std::size_t idx = leaf_find_index(leaf, key);
            if (idx < leaf->header.count) {
                return iterator(leaf, idx);
            }
            return end();
        }

        template <typename K>
        [[nodiscard]] const_iterator find(const K& key) const noexcept {
            if (!root_) return end();

            const void* curr = root_;
            while (!static_cast<const NodeHeader*>(curr)->is_leaf()) {
                const auto* inner = static_cast<const InnerType*>(curr);
                const std::size_t idx = inner_child_index(inner, key);
                curr = inner->children[idx];
            }

            const auto* leaf = static_cast<const LeafType*>(curr);
            const std::size_t idx = leaf_find_index(leaf, key);
            if (idx < leaf->header.count) {
                return const_iterator(leaf, idx);
            }
            return end();
        }

        template <typename K>
        [[nodiscard]] bool contains(const K& key) const noexcept {
            return find(key) != end();
        }

        template <typename K>
        [[nodiscard]] iterator lower_bound(const K& key) noexcept {
            if (!root_) return end();

            void* curr = root_;
            while (!static_cast<NodeHeader*>(curr)->is_leaf()) {
                auto* inner = static_cast<InnerType*>(curr);
                const std::size_t idx = inner_child_index(inner, key);
                curr = inner->children[idx];
            }

            auto* leaf = static_cast<LeafType*>(curr);
            const std::size_t idx = leaf_lower_bound(leaf, key);
            if (idx < leaf->header.count) {
                return iterator(leaf, idx);
            }
            if (leaf->next) {
                return iterator(leaf->next, 0);
            }
            return end();
        }

        template <typename K>
        [[nodiscard]] const_iterator lower_bound(const K& key) const noexcept {
            if (!root_) return end();

            const void* curr = root_;
            while (!static_cast<const NodeHeader*>(curr)->is_leaf()) {
                const auto* inner = static_cast<const InnerType*>(curr);
                const std::size_t idx = inner_child_index(inner, key);
                curr = inner->children[idx];
            }

            const auto* leaf = static_cast<const LeafType*>(curr);
            const std::size_t idx = leaf_lower_bound(leaf, key);
            if (idx < leaf->header.count) {
                return const_iterator(leaf, idx);
            }
            if (leaf->next) {
                return const_iterator(leaf->next, 0);
            }
            return end();
        }

        template <typename K>
        [[nodiscard]] iterator upper_bound(const K& key) noexcept {
            auto it = lower_bound(key);
            if (it != end() && !comp_(key, it->first) && !comp_(it->first, key)) {
                ++it;
            }
            return it;
        }

        template <typename K>
        [[nodiscard]] const_iterator upper_bound(const K& key) const noexcept {
            auto it = lower_bound(key);
            if (it != end() && !comp_(key, it->first) && !comp_(it->first, key)) {
                ++it;
            }
            return it;
        }

        template <typename K>
        [[nodiscard]] std::pair<iterator, iterator> equal_range(const K& key) noexcept {
            return {lower_bound(key), upper_bound(key)};
        }

        template <typename K>
        [[nodiscard]] std::pair<const_iterator, const_iterator> equal_range(const K& key) const noexcept {
            return {lower_bound(key), upper_bound(key)};
        }

        // Element access
        template <typename K>
        [[nodiscard]] Value& at(const K& key) {
            auto it = find(key);
            if (it == end()) {
                throw std::out_of_range("BPlusTree::at: key not found");
            }
            return it->second;
        }

        template <typename K>
        [[nodiscard]] const Value& at(const K& key) const {
            auto it = find(key);
            if (it == end()) {
                throw std::out_of_range("BPlusTree::at: key not found");
            }
            return it->second;
        }

        [[nodiscard]] Value& operator[](const Key& key) {
            auto [it, _] = insert_or_assign(key, Value{});
            return it->second;
        }

        [[nodiscard]] Value& operator[](Key&& key) {
            auto [it, _] = insert_or_assign(std::move(key), Value{});
            return it->second;
        }

        // High-throughput Sequential Range Scan with Prefetching
        template <typename K1, typename K2, typename Fn>
        void scan(const K1& min_key, const K2& max_key, Fn&& fn) const {
            auto it = lower_bound(min_key);
            while (it != end()) {
                if (comp_(max_key, it->first)) {
                    break;
                }
                fn(it->first, it->second);
                ++it;
            }
        }

        template <typename Fn>
        void scan_all(Fn&& fn) const {
            for (const auto* curr = head_leaf_; curr != nullptr; curr = curr->next) {
                if (curr->next) {
                    detail::prefetch_read(curr->next);
                }
                const auto* keys = curr->keys();
                const auto* vals = curr->values();
                for (std::size_t i = 0; i < curr->header.count; ++i) {
                    fn(keys[i], vals[i]);
                }
            }
        }

        // Invariant Validation
        [[nodiscard]] bool validate_invariants() const noexcept {
            if (!root_) {
                return size_ == 0 && head_leaf_ == nullptr && tail_leaf_ == nullptr && depth_ == 0;
            }

            size_type counted_elements = 0;
            const LeafType* prev_leaf = nullptr;
            for (const LeafType* curr = head_leaf_; curr != nullptr; curr = curr->next) {
                if (curr->prev != prev_leaf) return false;
                if (!curr->header.is_leaf()) return false;

                const auto* keys = curr->keys();
                for (std::size_t i = 0; i < curr->header.count; ++i) {
                    if (i > 0 && !comp_(keys[i - 1], keys[i])) {
                        return false;
                    }
                }

                if (prev_leaf && prev_leaf->header.count > 0 && curr->header.count > 0) {
                    if (!comp_(prev_leaf->key_at(prev_leaf->header.count - 1), curr->key_at(0))) {
                        return false;
                    }
                }

                counted_elements += curr->header.count;
                prev_leaf = curr;
            }

            if (prev_leaf != tail_leaf_) return false;
            if (counted_elements != size_) return false;

            return true;
        }

        [[nodiscard]] allocator_type get_allocator() const noexcept {
            return Allocator(leaf_alloc_);
        }
    };

    // ============================================================================
    // SECTION 6 — BPlusMap & BPlusSet Aliases
    // ============================================================================

    template <
        typename Key,
        typename Value,
        typename Compare = std::less<Key>,
        typename Traits = DefaultBPlusTreeTraits<Key, Value>,
        typename Allocator = std::allocator<std::pair<const Key, Value>>
    >
    using BPlusMap = BPlusTree<Key, Value, Compare, Traits, Allocator>;

    template <
        typename Key,
        typename Compare = std::less<Key>,
        typename Traits = DefaultBPlusTreeTraits<Key, std::monostate>,
        typename Allocator = std::allocator<std::pair<const Key, std::monostate>>
    >
    class BPlusSet {
    private:
        using TreeType = BPlusTree<Key, std::monostate, Compare, Traits, Allocator>;
        TreeType tree_;

    public:
        using key_type        = Key;
        using value_type      = Key;
        using size_type       = std::size_t;
        using difference_type = std::ptrdiff_t;
        using key_compare     = Compare;
        using allocator_type  = Allocator;

        class const_iterator {
        private:
            typename TreeType::const_iterator it_;
            friend class BPlusSet;

        public:
            using iterator_category = std::bidirectional_iterator_tag;
            using difference_type   = std::ptrdiff_t;
            using value_type        = Key;
            using pointer           = const Key*;
            using reference         = const Key&;

            constexpr const_iterator() noexcept = default;
            constexpr explicit const_iterator(typename TreeType::const_iterator it) noexcept : it_(it) {}

            [[nodiscard]] constexpr const Key& operator*() const noexcept { return it_->first; }
            [[nodiscard]] constexpr const Key* operator->() const noexcept { return &it_->first; }

            constexpr const_iterator& operator++() noexcept { ++it_; return *this; }
            constexpr const_iterator operator++(int) noexcept { auto tmp = *this; ++it_; return tmp; }
            constexpr const_iterator& operator--() noexcept { --it_; return *this; }
            constexpr const_iterator operator--(int) noexcept { auto tmp = *this; --it_; return tmp; }

            [[nodiscard]] constexpr bool operator==(const const_iterator& other) const noexcept { return it_ == other.it_; }
            [[nodiscard]] constexpr bool operator!=(const const_iterator& other) const noexcept { return it_ != other.it_; }
        };

        using iterator = const_iterator;

        constexpr BPlusSet() = default;
        explicit BPlusSet(const Compare& comp, const Allocator& alloc = Allocator()) : tree_(comp, alloc) {}
        explicit BPlusSet(const Allocator& alloc) : tree_(alloc) {}

        template <std::input_iterator InputIt>
        BPlusSet(InputIt first, InputIt last, const Compare& comp = Compare(), const Allocator& alloc = Allocator())
            : tree_(comp, alloc) {
            for (; first != last; ++first) {
                insert(*first);
            }
        }

        BPlusSet(std::initializer_list<Key> init, const Compare& comp = Compare(), const Allocator& alloc = Allocator())
            : BPlusSet(init.begin(), init.end(), comp, alloc) {}

        template <std::forward_iterator ForwardIt>
        [[nodiscard]] static BPlusSet from_sorted(ForwardIt first, ForwardIt last, const Compare& comp = Compare(), const Allocator& alloc = Allocator()) {
            std::vector<std::pair<const Key, std::monostate>> pairs;
            for (; first != last; ++first) {
                pairs.emplace_back(*first, std::monostate{});
            }
            BPlusSet set(comp, alloc);
            set.tree_ = TreeType::from_sorted(pairs.begin(), pairs.end(), comp, alloc);
            return set;
        }

        [[nodiscard]] iterator begin() const noexcept { return iterator(tree_.begin()); }
        [[nodiscard]] iterator end() const noexcept { return iterator(tree_.end()); }
        [[nodiscard]] iterator cbegin() const noexcept { return iterator(tree_.cbegin()); }
        [[nodiscard]] iterator cend() const noexcept { return iterator(tree_.cend()); }

        [[nodiscard]] bool empty() const noexcept { return tree_.empty(); }
        [[nodiscard]] size_type size() const noexcept { return tree_.size(); }
        void clear() noexcept { tree_.clear(); }

        std::pair<iterator, bool> insert(const Key& key) {
            auto [it, inserted] = tree_.insert_or_assign(key, std::monostate{});
            return {iterator(it), inserted};
        }

        std::pair<iterator, bool> insert(Key&& key) {
            auto [it, inserted] = tree_.insert_or_assign(std::move(key), std::monostate{});
            return {iterator(it), inserted};
        }

        template <typename K>
        size_type erase(const K& key) { return tree_.erase(key); }
        iterator erase(const_iterator pos) { return iterator(tree_.erase(pos.it_)); }

        template <typename K>
        [[nodiscard]] iterator find(const K& key) const noexcept { return iterator(tree_.find(key)); }
        template <typename K>
        [[nodiscard]] bool contains(const K& key) const noexcept { return tree_.contains(key); }
        template <typename K>
        [[nodiscard]] iterator lower_bound(const K& key) const noexcept { return iterator(tree_.lower_bound(key)); }
        template <typename K>
        [[nodiscard]] iterator upper_bound(const K& key) const noexcept { return iterator(tree_.upper_bound(key)); }
        template <typename K>
        [[nodiscard]] std::pair<iterator, iterator> equal_range(const K& key) const noexcept {
            auto [l, u] = tree_.equal_range(key);
            return {iterator(l), iterator(u)};
        }

        [[nodiscard]] bool validate_invariants() const noexcept { return tree_.validate_invariants(); }
    };

    // ============================================================================
    // SECTION 7 — Smriti Memory Resource Type Aliases & Factory Helpers
    // ============================================================================

    template <
        typename Key,
        typename Value,
        typename ResourceT,
        typename Compare = std::less<Key>,
        typename Traits = DefaultBPlusTreeTraits<Key, Value>
    >
    using SmritiBPlusMap = BPlusMap<Key, Value, Compare, Traits, smriti::SmritiAllocator<std::pair<const Key, Value>, ResourceT>>;

    template <
        typename Key,
        typename ResourceT,
        typename Compare = std::less<Key>,
        typename Traits = DefaultBPlusTreeTraits<Key, std::monostate>
    >
    using SmritiBPlusSet = BPlusSet<Key, Compare, Traits, smriti::SmritiAllocator<std::pair<const Key, std::monostate>, ResourceT>>;

    template <
        typename Key,
        typename Value,
        typename ResourceT,
        typename Compare = std::less<Key>,
        typename Traits = DefaultBPlusTreeTraits<Key, Value>
    >
    [[nodiscard]] inline auto make_smriti_bplus_map(ResourceT& res, const Compare& comp = Compare()) {
        using Alloc = smriti::SmritiAllocator<std::pair<const Key, Value>, ResourceT>;
        return BPlusMap<Key, Value, Compare, Traits, Alloc>(comp, Alloc(res));
    }

    template <
        typename Key,
        typename ResourceT,
        typename Compare = std::less<Key>,
        typename Traits = DefaultBPlusTreeTraits<Key, std::monostate>
    >
    [[nodiscard]] inline auto make_smriti_bplus_set(ResourceT& res, const Compare& comp = Compare()) {
        using Alloc = smriti::SmritiAllocator<std::pair<const Key, std::monostate>, ResourceT>;
        return BPlusSet<Key, Compare, Traits, Alloc>(comp, Alloc(res));
    }

} // namespace pebble::containers
