#ifndef N_ARY_TREE_H
#define N_ARY_TREE_H

#include <memory>
#include <vector>
#include <list>
#include <queue>
#include <functional>
#include <algorithm>
#include <concepts>
#include <utility>
#include <sstream>
#include <expected>
#include <optional>
#include <string>
#include <ranges>
#include <mutex>
#include <shared_mutex>
#include <iostream>

// Google Highway for portable SIMD operations
#include <hwy/highway.h>
#include <hwy/aligned_allocator.h>

// ============================================================================
// SIMD Operations using Google Highway for tree operations
// ============================================================================
namespace tree_simd {
    // SIMD-accelerated search for node ID in an array
    // Returns index of first match or -1 if not found
    inline int find_node_id_simd(const uint64_t* ids, size_t count, uint64_t target) {
        namespace hn = hwy::HWY_NAMESPACE;
        const hn::ScalableTag<uint64_t> d;
        const size_t N = hn::Lanes(d);

        if (count == 0) return -1;

        const auto target_vec = hn::Set(d, target);

        size_t i = 0;
        for (; i + N <= count; i += N) {
            const auto data = hn::LoadU(d, ids + i);
            const auto mask = hn::Eq(data, target_vec);
            if (!hn::AllFalse(d, mask)) {
                // Found match, scan for exact position
                for (size_t j = i; j < i + N && j < count; ++j) {
                    if (ids[j] == target) return static_cast<int>(j);
                }
            }
        }

        // Handle remaining elements
        for (; i < count; ++i) {
            if (ids[i] == target) return static_cast<int>(i);
        }

        return -1;
    }

    // SIMD-accelerated sum for statistics computation (uses uint64_t)
    inline uint64_t sum_simd(const uint64_t* data, size_t count) {
        namespace hn = hwy::HWY_NAMESPACE;
        const hn::ScalableTag<uint64_t> d;
        const size_t N = hn::Lanes(d);

        auto sum_vec = hn::Zero(d);
        size_t i = 0;

        for (; i + N <= count; i += N) {
            const auto v = hn::LoadU(d, data + i);
            sum_vec = hn::Add(sum_vec, v);
        }

        uint64_t result = hn::ReduceSum(d, sum_vec);

        for (; i < count; ++i) {
            result += data[i];
        }

        return result;
    }

    // SIMD-accelerated max finding for tree statistics (uses uint64_t)
    inline uint64_t max_simd(const uint64_t* data, size_t count) {
        if (count == 0) return 0;

        namespace hn = hwy::HWY_NAMESPACE;
        const hn::ScalableTag<uint64_t> d;
        const size_t N = hn::Lanes(d);

        auto max_vec = hn::Set(d, data[0]);
        size_t i = 0;

        for (; i + N <= count; i += N) {
            const auto v = hn::LoadU(d, data + i);
            max_vec = hn::Max(max_vec, v);
        }

        uint64_t result = hn::ReduceMax(d, max_vec);

        for (; i < count; ++i) {
            result = std::max(result, data[i]);
        }

        return result;
    }

    // Convenience wrappers that accept size_t and cast internally
    inline int find_node_id(const size_t* ids, size_t count, size_t target) {
        // On most 64-bit platforms, size_t == uint64_t
        static_assert(sizeof(size_t) == sizeof(uint64_t), "size_t must be 64-bit");
        return find_node_id_simd(reinterpret_cast<const uint64_t*>(ids), count, static_cast<uint64_t>(target));
    }

    inline size_t sum(const size_t* data, size_t count) {
        static_assert(sizeof(size_t) == sizeof(uint64_t), "size_t must be 64-bit");
        return static_cast<size_t>(sum_simd(reinterpret_cast<const uint64_t*>(data), count));
    }

    inline size_t max(const size_t* data, size_t count) {
        static_assert(sizeof(size_t) == sizeof(uint64_t), "size_t must be 64-bit");
        return static_cast<size_t>(max_simd(reinterpret_cast<const uint64_t*>(data), count));
    }
} // namespace tree_simd

// ============================================================================
// Memory pool for optimized node allocation with cache-line alignment
// ============================================================================
template <typename T>
class NodeMemoryPool {
    static constexpr size_t INITIAL_POOL_SIZE = 1024;
    static constexpr size_t ALIGNMENT = alignof(T);

    struct Block {
        std::unique_ptr<std::byte[]> memory;
        size_t size;
        size_t used;

        explicit Block(size_t s) : memory(std::make_unique<std::byte[]>(s)), size(s), used(0) {}
    };

    std::vector<Block> blocks_;
    std::vector<void*> free_list_;
    size_t block_size_ = INITIAL_POOL_SIZE * sizeof(T);

    void* allocate_from_block(Block& block, size_t size, size_t align) {
        size_t aligned_used = (block.used + align - 1) & ~(align - 1);
        if (aligned_used + size <= block.size) {
            void* ptr = block.memory.get() + aligned_used;
            block.used = aligned_used + size;
            return ptr;
        }
        return nullptr;
    }

    void add_new_block() {
        blocks_.emplace_back(std::max(block_size_, sizeof(T) * 64));
        block_size_ *= 2; // Exponential growth
    }

public:
    void* allocate(size_t size, size_t align = ALIGNMENT) {
        // Try free list first
        if (!free_list_.empty()) {
            void* ptr = free_list_.back();
            free_list_.pop_back();
            return ptr;
        }

        // Try current blocks
        for (auto& block : blocks_) {
            if (void* ptr = allocate_from_block(block, size, align)) {
                return ptr;
            }
        }

        // Need new block
        add_new_block();
        return allocate_from_block(blocks_.back(), size, align);
    }

    void deallocate(void* ptr) {
        if (ptr) {
            free_list_.push_back(ptr);
        }
    }

    void clear() {
        blocks_.clear();
        free_list_.clear();
        block_size_ = INITIAL_POOL_SIZE * sizeof(T);
    }

    [[nodiscard]] size_t total_allocated() const {
        size_t total = 0;
        for (const auto& block : blocks_) {
            total += block.size;
        }
        return total;
    }

    [[nodiscard]] size_t total_used() const {
        size_t total = 0;
        for (const auto& block : blocks_) {
            total += block.used;
        }
        return total;
    }
};

// An empty struct to use as the default for optional metadata.
struct EmptyMetadata {
    constexpr EmptyMetadata() = default;

    constexpr bool operator==(const EmptyMetadata&) const = default;
};

// Add stream operators for EmptyMetadata
inline std::ostream& operator<<(std::ostream& os, const EmptyMetadata&) {
    return os;
}

inline std::istream& operator>>(std::istream& is, EmptyMetadata&) {
    return is;
}

// Enhanced C++23 Concepts
template <typename T>
concept Serializable = requires(T t, std::ostream& os, std::istream& is) {
    os << t;
    is >> t;
};

template <typename T>
concept TreeData = std::copyable<T> && std::movable<T>;

template <typename M>
concept Metadata = std::default_initializable<M> && std::copyable<M>;

template <typename C, typename NodePtr>
concept NodeContainer = requires(C c, NodePtr p) {
    typename C::iterator;
    typename C::const_iterator;
    { c.begin() } -> std::same_as<typename C::iterator>;
    { c.end() } -> std::same_as<typename C::iterator>;
    c.push_back(std::move(p));
    { c.empty() } -> std::convertible_to<bool>;
    c.erase(c.begin());
};

// Tree error handling
enum class TreeError {
    NodeNotFound,
    InvalidOperation,
    SerializationError,
    IteratorInvalidated,
    ThreadSafetyViolation
};

template <typename T>
using TreeResult = std::expected<T, TreeError>;

// Forward declarations
template <TreeData T, Metadata Metadata = EmptyMetadata, template <typename...> typename Container = std::vector>
class NAryTree;

template <TreeData T, Metadata Metadata, template <typename...> typename Container>
class NAryTree {
public:
    // Statistics for tree analysis
    struct TreeStats {
        size_t node_count = 0;
        size_t leaf_count = 0;
        size_t max_depth = 0;
        size_t min_depth = 0;
        double avg_branching_factor = 0.0;
        size_t max_children = 0;
    };

    struct TreeNode {
        T data;
        Metadata metadata;
        Container<std::unique_ptr<TreeNode>> children;
        TreeNode* parent = nullptr;
        size_t node_id = 0; // Automatic unique node id
        size_t sibling_index = 0; // Index in parent's children for O(1) sibling navigation

        explicit constexpr TreeNode(TreeNode* p, const size_t id, Metadata meta, T value, size_t sib_idx = 0)
            : data(std::move(value)), metadata(std::move(meta)), parent(p), node_id(id), sibling_index(sib_idx) {}

        template <typename... Args>
        constexpr TreeNode(TreeNode* p, Metadata meta, const size_t id, Args&&... args)
            : data(std::forward<Args>(args)...), metadata(std::move(meta)), parent(p), node_id(id), sibling_index(0) {}

        [[nodiscard]] constexpr bool is_leaf() const noexcept { return children.empty(); }
        [[nodiscard]] constexpr size_t child_count() const noexcept { return children.size(); }
        [[nodiscard]] constexpr bool has_parent() const noexcept { return parent != nullptr; }

        // O(1) sibling navigation using stored index
        [[nodiscard]] TreeNode* next_sibling() const noexcept {
            if (!parent || sibling_index + 1 >= parent->children.size()) return nullptr;
            return parent->children[sibling_index + 1].get();
        }

        [[nodiscard]] TreeNode* prev_sibling() const noexcept {
            if (!parent || sibling_index == 0) return nullptr;
            return parent->children[sibling_index - 1].get();
        }

        // Structural equality
        constexpr bool operator==(const TreeNode& other) const {
            return data == other.data && metadata == other.metadata &&
                children.size() == other.children.size();
        }
    };

    // --- ITERATOR SUPPORT ---

    class pre_order_iterator {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = TreeNode;
        using difference_type = std::ptrdiff_t;
        using pointer = TreeNode*;
        using reference = TreeNode&;

        explicit pre_order_iterator(const pointer ptr = nullptr) : current_node(ptr) {}

        reference operator*() const { return *current_node; }
        pointer operator->() const { return current_node; }

        pre_order_iterator& operator++() {
            if (!current_node) return *this;
            if (!current_node->children.empty()) {
                current_node = current_node->children.front().get();
                return *this;
            }
            while (current_node) {
                if (current_node->parent) {
                    auto& siblings = current_node->parent->children;
                    size_t next_idx = current_node->sibling_index + 1;
                    if (next_idx < siblings.size()) {
                        current_node = siblings[next_idx].get();
                        return *this;
                    }
                }
                current_node = current_node->parent;
            }
            return *this;
        }

        pre_order_iterator operator++(int) {
            pre_order_iterator tmp = *this;
            ++*this;
            return tmp;
        }

        friend bool operator==(const pre_order_iterator& a, const pre_order_iterator& b) {
            return a.current_node == b.current_node;
        }

        friend bool operator!=(const pre_order_iterator& a, const pre_order_iterator& b) {
            return a.current_node != b.current_node;
        }

    private:
        pointer current_node;
    };

    class const_pre_order_iterator {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = const TreeNode;
        using difference_type = std::ptrdiff_t;
        using pointer = const TreeNode*;
        using reference = const TreeNode&;

        explicit const_pre_order_iterator(const pointer ptr = nullptr) : current_node(ptr) {}

        reference operator*() const { return *current_node; }
        pointer operator->() const { return current_node; }

        const_pre_order_iterator& operator++() {
            if (!current_node) return *this;
            if (!current_node->children.empty()) {
                current_node = current_node->children.front().get();
                return *this;
            }
            while (current_node) {
                if (current_node->parent) {
                    auto& siblings = current_node->parent->children;
                    size_t next_idx = current_node->sibling_index + 1;
                    if (next_idx < siblings.size()) {
                        current_node = siblings[next_idx].get();
                        return *this;
                    }
                }
                current_node = current_node->parent;
            }
            return *this;
        }

        const_pre_order_iterator operator++(int) {
            const_pre_order_iterator tmp = *this;
            ++*this;
            return tmp;
        }

        friend bool operator==(const const_pre_order_iterator& a, const const_pre_order_iterator& b) {
            return a.current_node == b.current_node;
        }

        friend bool operator!=(const const_pre_order_iterator& a, const const_pre_order_iterator& b) {
            return a.current_node != b.current_node;
        }

    private:
        pointer current_node;
    };

    class post_order_iterator {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = TreeNode;
        using difference_type = std::ptrdiff_t;
        using pointer = TreeNode*;
        using reference = TreeNode&;

        explicit post_order_iterator(const pointer ptr = nullptr) : current_node(ptr) {}

        reference operator*() const { return *current_node; }
        pointer operator->() const { return current_node; }

        post_order_iterator& operator++() {
            if (!current_node || !current_node->parent) {
                current_node = nullptr;
                return *this;
            }
            auto& siblings = current_node->parent->children;
            size_t next_idx = current_node->sibling_index + 1;
            if (next_idx < siblings.size()) {
                current_node = NAryTree::find_leftmost_leaf(siblings[next_idx].get());
            }
            else {
                current_node = current_node->parent;
            }
            return *this;
        }

        post_order_iterator operator++(int) {
            post_order_iterator tmp = *this;
            ++*this;
            return tmp;
        }

        friend bool operator==(const post_order_iterator& a, const post_order_iterator& b) {
            return a.current_node == b.current_node;
        }

        friend bool operator!=(const post_order_iterator& a, const post_order_iterator& b) {
            return a.current_node != b.current_node;
        }

    private:
        pointer current_node;
    };

    class const_post_order_iterator {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = const TreeNode;
        using difference_type = std::ptrdiff_t;
        using pointer = const TreeNode*;
        using reference = const TreeNode&;

        explicit const_post_order_iterator(const pointer ptr = nullptr) : current_node(ptr) {}

        reference operator*() const { return *current_node; }
        pointer operator->() const { return current_node; }

        const_post_order_iterator& operator++() {
            if (!current_node || !current_node->parent) {
                current_node = nullptr;
                return *this;
            }
            auto& siblings = current_node->parent->children;
            size_t next_idx = current_node->sibling_index + 1;
            if (next_idx < siblings.size()) {
                current_node = NAryTree::find_leftmost_leaf(siblings[next_idx].get());
            }
            else {
                current_node = current_node->parent;
            }
            return *this;
        }

        const_post_order_iterator operator++(int) {
            const_post_order_iterator tmp = *this;
            ++*this;
            return tmp;
        }

        friend bool operator==(const const_post_order_iterator& a, const const_post_order_iterator& b) {
            return a.current_node == b.current_node;
        }

        friend bool operator!=(const const_post_order_iterator& a, const const_post_order_iterator& b) {
            return a.current_node != b.current_node;
        }

    private:
        pointer current_node;
    };

    // Breadth-first (level-order) iterator
    class breadth_first_iterator {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = TreeNode;
        using difference_type = std::ptrdiff_t;
        using pointer = TreeNode*;
        using reference = TreeNode&;

        explicit breadth_first_iterator(pointer root_ptr = nullptr) : current_node(nullptr) {
            if (root_ptr) {
                q = std::make_shared<std::queue<TreeNode*>>();
                q->push(root_ptr);
                current_node = q->front();
            }
        }

        reference operator*() const { return *current_node; }
        pointer operator->() const { return current_node; }

        breadth_first_iterator& operator++() {
            if (!q || q->empty()) {
                current_node = nullptr;
                q.reset();
                return *this;
            }

            TreeNode* node = q->front();
            q->pop();
            for (const auto& child : node->children) {
                q->push(child.get());
            }

            if (!q->empty()) current_node = q->front();
            else {
                current_node = nullptr;
                q.reset();
            }
            return *this;
        }

        breadth_first_iterator operator++(int) {
            breadth_first_iterator tmp = *this;
            ++*this;
            return tmp;
        }

        friend bool operator==(const breadth_first_iterator& a, const breadth_first_iterator& b) {
            return a.current_node == b.current_node;
        }

        friend bool operator!=(const breadth_first_iterator& a, const breadth_first_iterator& b) {
            return a.current_node != b.current_node;
        }

    private:
        pointer current_node = nullptr;
        std::shared_ptr<std::queue<TreeNode*>> q;
    };

    class const_breadth_first_iterator {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = const TreeNode;
        using difference_type = std::ptrdiff_t;
        using pointer = const TreeNode*;
        using reference = const TreeNode&;

        explicit const_breadth_first_iterator(pointer root_ptr = nullptr) : current_node(nullptr) {
            if (root_ptr) {
                q = std::make_shared<std::queue<const TreeNode*>>();
                q->push(root_ptr);
                current_node = q->front();
            }
        }

        reference operator*() const { return *current_node; }
        pointer operator->() const { return current_node; }

        const_breadth_first_iterator& operator++() {
            if (!q || q->empty()) {
                current_node = nullptr;
                q.reset();
                return *this;
            }

            const TreeNode* node = q->front();
            q->pop();
            for (const auto& child : node->children) {
                q->push(child.get());
            }

            if (!q->empty()) current_node = q->front();
            else {
                current_node = nullptr;
                q.reset();
            }
            return *this;
        }

        const_breadth_first_iterator operator++(int) {
            const_breadth_first_iterator tmp = *this;
            ++*this;
            return tmp;
        }

        friend bool operator==(const const_breadth_first_iterator& a, const const_breadth_first_iterator& b) {
            return a.current_node == b.current_node;
        }

        friend bool operator!=(const const_breadth_first_iterator& a, const const_breadth_first_iterator& b) {
            return a.current_node != b.current_node;
        }

    private:
        pointer current_node = nullptr;
        std::shared_ptr<std::queue<const TreeNode*>> q;
    };

    // Standard begin/end iterate over pre-order by default
    auto begin() { return pre_order_iterator(root.get()); }
    auto end() { return pre_order_iterator(nullptr); }
    auto begin() const { return const_pre_order_iterator(root.get()); }
    auto end() const { return const_pre_order_iterator(nullptr); }
    auto cbegin() const { return const_pre_order_iterator(root.get()); }
    auto cend() const { return const_pre_order_iterator(nullptr); }

    // Breadth-first begin/end and range
    auto breadth_begin() { return breadth_first_iterator(root.get()); }
    auto breadth_end() { return breadth_first_iterator(nullptr); }
    auto breadth_begin() const { return const_breadth_first_iterator(root.get()); }
    auto breadth_end() const { return const_breadth_first_iterator(nullptr); }

    auto breadth_first() {
        struct range {
            NAryTree& t;
            auto begin() { return breadth_first_iterator(t.root.get()); }
            auto end() const { return breadth_first_iterator(nullptr); }
        };
        return range{*this};
    }

    auto breadth_first() const {
        struct range {
            const NAryTree& t;
            auto begin() const { return const_breadth_first_iterator(t.root.get()); }
            auto end() const { return const_breadth_first_iterator(nullptr); }
        };
        return range{*this};
    }

    auto post_order() {
        struct range {
            NAryTree& t;
            auto begin() { return post_order_iterator(t.find_leftmost_leaf(t.root.get())); }
            auto end() const { return post_order_iterator(nullptr); }
        };
        return range{*this};
    }

    auto post_order() const {
        struct range {
            const NAryTree& t;
            auto begin() const { return const_post_order_iterator(t.find_leftmost_leaf(t.root.get())); }
            auto end() const { return const_post_order_iterator(nullptr); }
        };
        return range{*this};
    }

private:
    size_t next_node_id = 1;
    size_t node_count_ = 0;
    std::unique_ptr<TreeNode> root;

    static TreeNode* find_leftmost_leaf(TreeNode* node) {
        if (!node) return nullptr;
        while (!node->is_leaf()) node = node->children.front().get();
        return node;
    }

    size_t count_subtree(const TreeNode* node) const {
        if (!node) return 0;
        size_t n = 1;
        for (const auto& child : node->children) n += count_subtree(child.get());
        return n;
    }

    void copy_children(TreeNode* dest_node, const TreeNode* src_node) {
        for (const auto& src_child : src_node->children) {
            auto new_child = std::make_unique<
                TreeNode>(dest_node, next_node_id++, src_child->metadata, src_child->data);
            new_child->sibling_index = dest_node->children.size();
            copy_children(new_child.get(), src_child.get());
            dest_node->children.push_back(std::move(new_child));
        }
    }

    size_t height_recursive(const TreeNode* node) const {
        if (!node) return 0;
        size_t h = 0;
        for (const auto& child : node->children) { h = std::max(h, height_recursive(child.get())); }
        return 1 + h;
    }

    template <typename Predicate>
    TreeNode* find_if_recursive(TreeNode* current, Predicate predicate) const {
        if (!current) return nullptr;
        if (predicate(*current)) return current;
        for (const auto& child : current->children) {
            if (TreeNode* found = find_if_recursive(child.get(), predicate)) return found;
        }
        return nullptr;
    }

    // --- Serialization helpers ---
    void serialize_node(std::ostream& os, const TreeNode* node) const {
        if (!node) {
            os << "#\n";
            return;
        }
        os << node->node_id << '\n' << node->data << '\n' << node->metadata << '\n' << node->children.size() << '\n';
        for (const auto& child : node->children) {
            serialize_node(os, child.get());
        }
    }

    void deserialize_node_recursive(std::istream& is, TreeNode* parent = nullptr) {
        std::string line;
        if (!std::getline(is, line) || line == "#") return;

        size_t node_id = std::stoull(line);
        std::getline(is, line);
        std::istringstream data_ss(line);
        T data;
        data_ss >> data;
        std::getline(is, line);
        std::istringstream meta_ss(line);
        Metadata meta;
        meta_ss >> meta;
        std::getline(is, line);
        size_t n_children = std::stoul(line);

        auto node = std::make_unique<TreeNode>(parent, node_id, std::move(meta), std::move(data));
        TreeNode* node_ptr = node.get();
        if (node_id >= next_node_id) next_node_id = node_id + 1;

        for (size_t i = 0; i < n_children; ++i) {
            deserialize_node_recursive(is, node_ptr);
        }

        if (!parent) {
            root = std::move(node);
        }
        else {
            node_ptr->sibling_index = parent->children.size();
            parent->children.push_back(std::move(node));
        }
    }

public:
    // --- Constructors and Rule of Five ---
    NAryTree() = default;

    explicit NAryTree(T root_data, Metadata root_metadata = Metadata{})
        : root(std::make_unique<TreeNode>(nullptr, next_node_id++, std::move(root_metadata), std::move(root_data))) {}

    ~NAryTree() = default;

    NAryTree(NAryTree&& other) noexcept
        : next_node_id(other.next_node_id), node_count_(other.node_count_), root(std::move(other.root)) {
        other.next_node_id = 1;
        other.node_count_ = 0;
    }

    NAryTree& operator=(NAryTree&& other) noexcept {
        if (this != &other) {
            root = std::move(other.root);
            next_node_id = other.next_node_id;
            node_count_ = other.node_count_;
            other.next_node_id = 1;
            other.node_count_ = 0;
        }
        return *this;
    }

    NAryTree(const NAryTree& other) {
        next_node_id = 1;
        node_count_ = 0;
        if (other.root) {
            root = std::make_unique<TreeNode>(nullptr, next_node_id++, other.root->metadata, other.root->data);
            copy_children(root.get(), other.root.get());
            node_count_ = count_subtree(root.get());
        }
    }

    // --- FIX: Added constructor to create tree from a specific TreeNode (Subtree Clone) ---
    explicit NAryTree(const TreeNode& subtree_root) {
        next_node_id = 1;
        node_count_ = 0;
        root = std::make_unique<TreeNode>(nullptr, next_node_id++, subtree_root.metadata, subtree_root.data);
        copy_children(root.get(), &subtree_root);
        node_count_ = count_subtree(root.get());
    }

    NAryTree& operator=(const NAryTree& other) {
        if (this != &other) {
            root.reset();
            next_node_id = 1;
            node_count_ = 0;
            if (other.root) {
                root = std::make_unique<TreeNode>(nullptr, next_node_id++, other.root->metadata, other.root->data);
                copy_children(root.get(), other.root.get());
                node_count_ = count_subtree(root.get());
            }
        }
        return *this;
    }

    // --- PUBLIC INTERFACE ---
    TreeNode* get_root() const { return root.get(); }

    // --- Helper to extract root ownership (needed for operators.hpp) ---
    std::unique_ptr<TreeNode> extract_root() {
        return std::move(root);
    }

    TreeNode* insert(TreeNode* parent, T data, Metadata metadata = Metadata{}) {
        if (!parent) {
            if (root) return nullptr;
            root = std::make_unique<TreeNode>(nullptr, next_node_id++, std::move(metadata), std::move(data));
            ++node_count_;
            return root.get();
        }
        auto new_node = std::make_unique<TreeNode>(parent, next_node_id++, std::move(metadata), std::move(data));
        TreeNode* new_node_ptr = new_node.get();
        new_node_ptr->sibling_index = parent->children.size();
        parent->children.push_back(std::move(new_node));
        ++node_count_;
        return new_node_ptr;
    }

    template <typename... Args>
    TreeNode* emplace(TreeNode* parent, Metadata metadata, Args&&... args) {
        if (!parent) {
            if (root) return nullptr;
            root = std::make_unique<TreeNode>(nullptr, next_node_id++, std::move(metadata),
                                              std::forward<Args>(args)...);
            ++node_count_;
            return root.get();
        }
        auto new_node = std::make_unique<TreeNode>(parent, next_node_id++, std::move(metadata),
                                                   std::forward<Args>(args)...);
        TreeNode* new_node_ptr = new_node.get();
        new_node_ptr->sibling_index = parent->children.size();
        parent->children.push_back(std::move(new_node));
        ++node_count_;
        return new_node_ptr;
    }

    // --- Bulk insert: add multiple children to a parent ---
    std::vector<TreeNode*> bulk_insert(TreeNode* parent, const std::vector<std::pair<T, Metadata>>& items) {
        std::vector<TreeNode*> result;
        for (const auto& item : items) {
            result.push_back(insert(parent, item.first, item.second));
        }
        return result;
    }

    bool remove(const T& value) {
        TreeNode* node_to_remove = find(value);
        if (!node_to_remove) return false;
        return remove(node_to_remove);
    }

    bool remove(TreeNode* node_to_remove) {
        if (!node_to_remove) return false;
        if (node_to_remove == root.get()) {
            node_count_ = 0;
            root.reset();
            return true;
        }
        TreeNode* parent = node_to_remove->parent;
        if (!parent) return false;
        auto& siblings = parent->children;
        auto it = std::find_if(siblings.begin(), siblings.end(), [node_to_remove](const auto& child_ptr) {
            return child_ptr.get() == node_to_remove;
        });
        if (it != siblings.end()) {
            size_t removed = count_subtree(node_to_remove);
            size_t erased_idx = static_cast<size_t>(std::distance(siblings.begin(), it));
            siblings.erase(it);
            for (size_t i = erased_idx; i < siblings.size(); ++i)
                siblings[i]->sibling_index = i;
            node_count_ -= removed;
            return true;
        }
        return false;
    }

    TreeNode* find(const T& value) const { return find_if([&](const TreeNode& node) { return node.data == value; }); }

    std::vector<TreeNode*> find_all(const T& value) {
        return find_all_if([&](const TreeNode& node) { return node.data == value; });
    }

    std::vector<const TreeNode*> find_all(const T& value) const {
        return find_all_if([&](const TreeNode& node) { return node.data == value; });
    }

    template <typename Predicate>
    TreeNode* find_if(Predicate predicate) const { return find_if_recursive(root.get(), predicate); }

    // Non-const version (already present)
    template <typename Predicate>
    std::vector<TreeNode*> find_all_if(Predicate predicate) {
        std::vector<TreeNode*> results;
        for (auto& node : *this) {
            if (predicate(node)) {
                results.push_back(&node);
            }
        }
        return results;
    }

    // Const version (fixes your error)
    template <typename Predicate>
    std::vector<const TreeNode*> find_all_if(Predicate predicate) const {
        std::vector<const TreeNode*> results;
        for (const auto& node : *this) {
            if (predicate(node)) {
                results.push_back(&node);
            }
        }
        return results;
    }

    void traverse_depth_first(const std::function<void(const TreeNode*)>& visit) const {
        for (const auto& node : *this) { visit(&node); }
    }

    void traverse_breadth_first(const std::function<void(const TreeNode*)>& visit) const {
        if (!root) return;
        std::queue<const TreeNode*> q;
        q.push(root.get());
        while (!q.empty()) {
            const TreeNode* current = q.front();
            q.pop();
            visit(current);
            for (const auto& child : current->children) { q.push(child.get()); }
        }
    }

    [[nodiscard]] size_t size() const { return node_count_; }

    [[nodiscard]] size_t height() const { return height_recursive(root.get()); }

    void clear() {
        root.reset();
        next_node_id = 1;
        node_count_ = 0;
    }

    // --- Advanced Tree Operations ---

    // Tree comparison and structural operations
    [[nodiscard]] constexpr bool empty() const { return !root; }

    bool structural_equal(const NAryTree& other) const {
        if (!root && !other.root) return true;
        if (!root || !other.root) return false;
        return structural_equal_recursive(root.get(), other.root.get());
    }

    [[nodiscard]] size_t structural_hash() const {
        return structural_hash_recursive(root.get());
    }

    // Tree transformations with C++23 explicit object parameter
    template <typename Self, typename F>
    auto map(this Self&& self, F&& func) -> NAryTree<std::invoke_result_t<F, T>, Metadata, Container> {
        using NewT = std::invoke_result_t<F, T>;
        NAryTree<NewT, Metadata, Container> result;
        if (self.root) {
            result.root = self.map_recursive(self.root.get(), std::forward<F>(func));
        }
        return result;
    }

    template <typename Predicate>
    NAryTree filter(Predicate&& pred) const {
        NAryTree result;
        if (root) {
            result.root = filter_recursive(root.get(), std::forward<Predicate>(pred));
        }
        return result;
    }

    // Path operations
    std::vector<TreeNode*> path_to_root(TreeNode* node) const {
        std::vector<TreeNode*> path;
        while (node) {
            path.push_back(node);
            node = node->parent;
        }
        std::reverse(path.begin(), path.end());
        return path;
    }

    std::optional<std::vector<size_t>> path_between(TreeNode* from, TreeNode* to) const {
        if (!from || !to) return std::nullopt;

        auto from_path = path_to_root(from);
        auto to_path = path_to_root(to);

        // Find common ancestor
        size_t common = 0;
        while (common < from_path.size() && common < to_path.size() &&
            from_path[common] == to_path[common]) {
            common++;
        }

        if (common == 0) return std::nullopt; // No common ancestor

        std::vector<size_t> result;
        // Path up from 'from' to common ancestor
        for (size_t i = from_path.size(); i > common; --i) {
            result.push_back(SIZE_MAX); // Sentinel for "go up"
        }
        // Path down from common ancestor to 'to'
        for (size_t i = common; i < to_path.size(); ++i) {
            auto parent = (i == 0) ? nullptr : to_path[i - 1];
            if (parent) {
                auto& children = parent->children;
                for (size_t j = 0; j < children.size(); ++j) {
                    if (children[j].get() == to_path[i]) {
                        result.push_back(j);
                        break;
                    }
                }
            }
        }
        return result;
    }

    TreeNode* lowest_common_ancestor(TreeNode* a, TreeNode* b) const {
        if (!a || !b) return nullptr;

        auto path_a = path_to_root(a);
        auto path_b = path_to_root(b);

        TreeNode* lca = nullptr;
        size_t min_len = std::min(path_a.size(), path_b.size());
        for (size_t i = 0; i < min_len && path_a[i] == path_b[i]; ++i) {
            lca = path_a[i];
        }
        return lca;
    }

    // Advanced tree operations
    void graft(TreeNode* target, NAryTree&& subtree) {
        if (!target || !subtree.root) return;
        node_count_ += subtree.node_count_;
        subtree.root->parent = target;
        subtree.root->sibling_index = target->children.size();
        target->children.push_back(std::move(subtree.root));
        subtree.node_count_ = 0;
    }

    NAryTree split(TreeNode* node) {
        NAryTree result;
        if (!node || node == root.get()) return result;

        // Remove from parent
        if (node->parent) {
            auto& siblings = node->parent->children;
            auto it = std::find_if(siblings.begin(), siblings.end(),
                                   [node](const auto& child) { return child.get() == node; });
            if (it != siblings.end()) {
                size_t split_count = count_subtree(node);
                size_t erased_idx = static_cast<size_t>(std::distance(siblings.begin(), it));
                result.root = std::move(*it);
                result.root->parent = nullptr;
                siblings.erase(it);
                for (size_t i = erased_idx; i < siblings.size(); ++i)
                    siblings[i]->sibling_index = i;
                node_count_ -= split_count;
                result.node_count_ = split_count;
            }
        }
        return result;
    }

    void merge(NAryTree&& other) {
        if (!root) {
            *this = std::move(other);
            return;
        }
        if (other.root) {
            node_count_ += other.node_count_;
            other.root->parent = root.get();
            other.root->sibling_index = root->children.size();
            root->children.push_back(std::move(other.root));
            other.node_count_ = 0;
        }
    }

    // Statistics and analysis
    TreeStats analyze() const {
        TreeStats stats;
        if (!root) return stats;

        analyze_recursive(root.get(), stats, 0);
        if (stats.node_count > 0 && stats.node_count - stats.leaf_count > 0) {
            stats.avg_branching_factor = static_cast<double>(stats.node_count - 1) /
                (stats.node_count - stats.leaf_count);
        }
        return stats;
    }

    // Debug and visualization
    [[nodiscard]] std::string to_string() const {
        if (!root) return "Empty tree";
        std::string result;
        to_string_recursive(result, root.get(), "", true);
        return result;
    }

    [[nodiscard]] std::string to_dot() const {
        std::ostringstream ss;
        ss << "digraph Tree {\n";
        if (root) {
            to_dot_recursive(root.get(), ss);
        }
        ss << "}\n";
        return ss.str();
    }

    void print_tree(std::ostream& os = std::cout) const {
        os << to_string() << std::endl;
    }

    // Enhanced serialization with versioning
    static constexpr uint32_t SERIALIZATION_VERSION = 1;

    void serialize_versioned(std::ostream& os) const {
        os << SERIALIZATION_VERSION << '\n';
        serialize(os);
    }

    bool deserialize_versioned(std::istream& is) {
        uint32_t version;
        is >> version;
        is.ignore(); // Skip newline
        if (version != SERIALIZATION_VERSION) return false;
        deserialize(is);
        return true;
    }

    // JSON serialization
    void serialize_json(std::ostream& os) const {
        os << "{\n";
        if (root) {
            serialize_json_node(os, root.get(), 1);
        }
        os << "}\n";
    }

    // Thread-safe error handling
    template <typename F>
    auto try_operation(F&& func) const -> TreeResult<std::invoke_result_t<F>> {
        try {
            if constexpr (std::is_void_v<std::invoke_result_t<F>>) {
                func();
                return TreeResult<void>{};
            }
            else {
                return TreeResult<std::invoke_result_t<F>>{func()};
            }
        }
        catch (...) {
            return std::unexpected(TreeError::InvalidOperation);
        }
    }

    TreeResult<TreeNode*> try_insert(TreeNode* parent, T data, Metadata metadata = Metadata{}) {
        return try_operation([&]() { return insert(parent, std::move(data), std::move(metadata)); });
    }

    TreeResult<bool> try_remove(TreeNode* node) {
        return try_operation([&]() { return remove(node); });
    }

    // Range and view support
    auto nodes() const {
        return std::ranges::subrange(begin(), end());
    }

    auto leaves() const {
        return nodes() | std::views::filter([](const auto& node) { return node.is_leaf(); });
    }

    auto level(size_t depth) const {
        std::vector<const TreeNode*> result;
        if (root && depth == 0) {
            result.push_back(root.get());
        }
        else if (depth > 0) {
            collect_at_depth(root.get(), depth, 0, result);
        }
        return result;
    }

    // Bulk operations with ranges
    template <std::ranges::range Range>
    std::vector<TreeNode*> bulk_insert_range(TreeNode* parent, Range&& data_range) {
        std::vector<TreeNode*> result;
        for (auto&& item : data_range) {
            result.push_back(insert(parent, std::forward<decltype(item)>(item)));
        }
        return result;
    }

private:
    // Helper functions for advanced operations
    bool structural_equal_recursive(const TreeNode* a, const TreeNode* b) const {
        if (!a && !b) return true;
        if (!a || !b) return false;
        if (a->data != b->data || a->children.size() != b->children.size()) return false;

        for (size_t i = 0; i < a->children.size(); ++i) {
            if (!structural_equal_recursive(a->children[i].get(), b->children[i].get())) {
                return false;
            }
        }
        return true;
    }

    size_t structural_hash_recursive(const TreeNode* node) const {
        if (!node) return 0;
        size_t hash = std::hash<T>{}(node->data);
        for (const auto& child : node->children) {
            hash ^= structural_hash_recursive(child.get()) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
        }
        return hash;
    }

    template <typename F, typename NewT = std::invoke_result_t<F, T>>
    std::unique_ptr<typename NAryTree<NewT, Metadata, Container>::TreeNode>
    map_recursive(const TreeNode* node, F&& func) const {
        if (!node) return nullptr;

        auto new_node = std::make_unique<typename NAryTree<NewT, Metadata, Container>::TreeNode>(
            nullptr, node->node_id, node->metadata, func(node->data));

        for (const auto& child : node->children) {
            if (auto mapped_child = map_recursive(child.get(), func)) {
                mapped_child->parent = new_node.get();
                new_node->children.push_back(std::move(mapped_child));
            }
        }
        return new_node;
    }

    template <typename Predicate>
    std::unique_ptr<TreeNode> filter_recursive(const TreeNode* node, Predicate&& pred) const {
        if (!node || !pred(*node)) return nullptr;

        auto new_node = std::make_unique<TreeNode>(nullptr, node->node_id, node->metadata, node->data);
        for (const auto& child : node->children) {
            if (auto filtered_child = filter_recursive(child.get(), pred)) {
                filtered_child->parent = new_node.get();
                new_node->children.push_back(std::move(filtered_child));
            }
        }
        return new_node;
    }

    void analyze_recursive(const TreeNode* node, TreeStats& stats, size_t depth) const {
        if (!node) return;

        stats.node_count++;
        stats.max_depth = std::max(stats.max_depth, depth);
        if (depth == 0 || stats.min_depth == 0) stats.min_depth = depth;
        else stats.min_depth = std::min(stats.min_depth, depth);

        if (node->is_leaf()) stats.leaf_count++;
        stats.max_children = std::max(stats.max_children, node->children.size());

        for (const auto& child : node->children) {
            analyze_recursive(child.get(), stats, depth + 1);
        }
    }

    void to_string_recursive(std::string& out, const TreeNode* node, const std::string& prefix, bool is_last) const {
        if (!node) return;

        out += prefix;
        out += (is_last ? "\xe2\x94\x94\xe2\x94\x80\xe2\x94\x80 " : "\xe2\x94\x9c\xe2\x94\x80\xe2\x94\x80 ");
        std::ostringstream ss;
        ss << node->data;
        out += ss.str();
        out += '\n';

        for (size_t i = 0; i < node->children.size(); ++i) {
            bool child_is_last = (i == node->children.size() - 1);
            std::string child_prefix = prefix + (is_last ? "    " : "\xe2\x94\x82   ");
            to_string_recursive(out, node->children[i].get(), child_prefix, child_is_last);
        }
    }

    void to_dot_recursive(const TreeNode* node, std::ostringstream& ss) const {
        if (!node) return;

        ss << "  " << node->node_id << " [label=\"" << node->data << "\"];\n";
        for (const auto& child : node->children) {
            ss << "  " << node->node_id << " -> " << child->node_id << ";\n";
            to_dot_recursive(child.get(), ss);
        }
    }

    void serialize_json_node(std::ostream& os, const TreeNode* node, int indent) const {
        if (!node) return;

        std::string spaces(indent * 2, ' ');
        os << spaces << R"("data": ")" << node->data << "\",\n";
        os << spaces << "\"id\": " << node->node_id;

        if (!node->children.empty()) {
            os << ",\n" << spaces << "\"children\": [\n";
            for (size_t i = 0; i < node->children.size(); ++i) {
                os << spaces << "  {\n";
                serialize_json_node(os, node->children[i].get(), indent + 2);
                os << "\n" << spaces << "  }";
                if (i < node->children.size() - 1) os << ",";
                os << "\n";
            }
            os << spaces << "]";
        }
    }

    void collect_at_depth(const TreeNode* node, size_t target_depth, size_t current_depth,
                          std::vector<const TreeNode*>& result) const {
        if (!node) return;

        if (current_depth == target_depth) {
            result.push_back(node);
        }
        else if (current_depth < target_depth) {
            for (const auto& child : node->children) {
                collect_at_depth(child.get(), target_depth, current_depth + 1, result);
            }
        }
    }

public:
    // --- Serialization ---
    void serialize(std::ostream& os) const { serialize_node(os, root.get()); }

    void deserialize(std::istream& is) {
        root.reset();
        next_node_id = 1;
        node_count_ = 0;
        deserialize_node_recursive(is, nullptr);
        node_count_ = count_subtree(root.get());
    }
};

// Builder pattern for easy tree construction
template <TreeData T, Metadata Metadata = EmptyMetadata, template <typename...> typename Container = std::vector>
class TreeBuilder {
    NAryTree<T, Metadata, Container> tree;
    NAryTree<T, Metadata, Container>::TreeNode* current = nullptr;

public:
    TreeBuilder& root(T value, Metadata meta = Metadata{}) {
        current = tree.insert(nullptr, std::move(value), std::move(meta));
        return *this;
    }

    TreeBuilder& child(T value, Metadata meta = Metadata{}) {
        if (current) {
            tree.insert(current, std::move(value), std::move(meta));
        }
        return *this;
    }

    TreeBuilder& sibling(T value, Metadata meta = Metadata{}) {
        if (current && current->parent) {
            current = tree.insert(current->parent, std::move(value), std::move(meta));
        }
        else if (current) {
            // current is root, add as child of root
            current = tree.insert(current, std::move(value), std::move(meta));
        }
        return *this;
    }

    TreeBuilder& parent() {
        if (current && current->parent) {
            current = current->parent;
        }
        return *this;
    }

    NAryTree<T, Metadata, Container> build() && {
        return std::move(tree);
    }
};

// Thread-safe wrapper
template <TreeData T, Metadata Metadata = EmptyMetadata, template <typename...> typename Container = std::vector,
                                                         typename Mutex = std::shared_mutex>
class ThreadSafeNAryTree {
    mutable Mutex mutex_;
    NAryTree<T, Metadata, Container> tree_;

public:
    template <typename... Args>
    auto insert(Args&&... args) {
        std::unique_lock lock(mutex_);
        return tree_.insert(std::forward<Args>(args)...);
    }

    template <typename... Args>
    bool remove(Args&&... args) {
        std::unique_lock lock(mutex_);
        return tree_.remove(std::forward<Args>(args)...);
    }

    template <typename... Args>
    auto find(Args&&... args) const {
        std::shared_lock lock(mutex_);
        return tree_.find(std::forward<Args>(args)...);
    }

    size_t size() const {
        std::shared_lock lock(mutex_);
        return tree_.size();
    }

    // Add other thread-safe methods as needed...

    template <typename F>
    auto with_lock(F&& func) -> std::invoke_result_t<F, const NAryTree<T, Metadata, Container>&> {
        std::shared_lock lock(mutex_);
        return func(tree_);
    }

    template <typename F>
    auto with_unique_lock(F&& func) -> std::invoke_result_t<F, NAryTree<T, Metadata, Container>&> {
        std::unique_lock lock(mutex_);
        return func(tree_);
    }
};

#endif // N_ARY_TREE_H
