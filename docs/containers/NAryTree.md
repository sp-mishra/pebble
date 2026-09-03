# `NAryTree` — High-Performance N-Ary Tree Container

> **Header:**
> - `include/containers/tree/NAryTree.hpp`
>
> **Namespace:** Global (root namespace)  
> **Standard required:** C++23 (`-std=c++2b`)  
> **Dependencies:** Standard library, Google Highway (for SIMD operations)

---

## Table of Contents

1. [Introduction](#1-introduction)
2. [Philosophy](#2-philosophy)
3. [Core Components](#3-core-components)
4. [Tree Types & Templates](#4-tree-types--templates)
5. [Node and Tree Management](#5-node-and-tree-management)
6. [Tree Navigation](#6-tree-navigation)
7. [Iteration & Traversal](#7-iteration--traversal)
8. [Tree Analysis & Statistics](#8-tree-analysis--statistics)
9. [Tree Transformations](#9-tree-transformations)
10. [Serialization](#10-serialization)
11. [Advanced Features](#11-advanced-features)
12. [Thread-Safe Operations](#12-thread-safe-operations)
13. [Error Handling](#13-error-handling)
14. [Examples](#14-examples)

---

## 1.5 N-Ary Trees vs Binary Trees vs std::set/std::map

### Why Choose NAryTree Over Binary Trees?

**Binary trees** (e.g., `std::set`, `std::map`, `std::unordered_set`) optimize for fast lookup and maintain sorted
order:

```cpp
// Binary tree: O(log n) search, O(n) space for pointers
std::set<int> binary_set;
auto it = binary_set.find(42);  // O(log n)
```

**N-ary trees** excel where:

1. **Variable branching factor**: Each node has an arbitrary number of children
    - File systems: directories with 0-1000+ files/subdirectories
    - DOM/XML: elements with variable children
    - Scene graphs: transforms with many entities

2. **Structural hierarchy preservation**: Parent-child relationships are explicit and traversable
   ```cpp
   // N-ary: navigate parent/children directly
   auto parent = node->parent;
   auto children = node->children;
   for (const auto &child_ptr : children) {
       // ... visit children
   }
   
   // Binary tree can't directly express this; must rebuild from pointers
   ```

3. **Batch operations on subtrees**: Graft, split, merge entire branches
   ```cpp
   // N-ary: efficient
   tree1.graft(target_node, std::move(subtree));
   
   // Binary tree: no native support; must rebuild
   ```

4. **Traversal order matters**: Pre-order, post-order, breadth-first across levels
   ```cpp
   // N-ary: level-based traversal natural
   for (const auto *node : tree.level(depth)) { /* ... */ }
   
   // Binary tree: less natural; must implement queues/stacks
   ```

### Use Cases

| Use Case                    | Best Choice             | Reason                                            |
|-----------------------------|-------------------------|---------------------------------------------------|
| **File system hierarchy**   | NAryTree                | Arbitrary children, directory trees common        |
| **DOM/XML parsing**         | NAryTree                | Elements have variable children                   |
| **Organization charts**     | NAryTree                | Managers with many subordinates                   |
| **Scene graphs**            | NAryTree                | Transform nodes with multiple entities            |
| **Sorted key storage**      | `std::set` / `std::map` | O(log n) lookup; ordering guaranteed              |
| **Fast membership test**    | `std::unordered_set`    | Hash-based; O(1) average lookup                   |
| **Task DAG dependencies**   | NAryTree (or graph)     | Task hierarchies with children                    |
| **Parse trees (compilers)** | NAryTree                | Grammar rules produce variable-length productions |

### Comparison Table

| Feature                     | NAryTree            | Binary Tree (std::set)    | N-ary w/ ordering    |
|-----------------------------|---------------------|---------------------------|----------------------|
| **Find arbitrary value**    | O(n)                | O(log n)                  | O(n log n) or custom |
| **Branching factor**        | Arbitrary           | Fixed (2)                 | Arbitrary            |
| **Parent/child navigation** | O(1)                | Pointer lookup            | O(1)                 |
| **Extract subtree**         | O(1)                | N/A (no subtree concept)  | O(1)                 |
| **Breadth-first traversal** | O(n) native         | O(n) with queue           | O(n) native          |
| **Space overhead**          | ~40 bytes/node      | ~48 bytes/node            | ~40 bytes/node       |
| **Cache locality**          | Good (vec children) | Fair (pointers scattered) | Good (vec children)  |

**Conclusion:** Use `NAryTree` for hierarchical/structural data; use `std::set`/`std::map` for sorted key-value
collections.

---

`NAryTree` is a **modern, high-performance N-ary tree (multi-child tree) container** for C++23 that enables:

- **Flexible structure** — Each node can have an arbitrary number of children
- **Rich payload storage** — Attach arbitrary data types and metadata to nodes
- **SIMD acceleration** — Built-in Google Highway support for statistics computation
- **Multiple traversal strategies** — Pre-order, post-order, breadth-first, and custom traversals
- **Structural operations** — Graft, split, merge, and filter entire subtrees
- **Memory efficiency** — Cache-aligned allocation pools with automatic reuse
- **Thread-safe variants** — Optional `ThreadSafeNAryTree` wrapper
- **Full serialization** — Support for plain text, JSON, and versioned formats
- **C++23 features** — Explicit object parameters for transformation methods, ranges, views

---

## 2. Philosophy

### N-Ary Structure

Unlike binary trees, `NAryTree` allows each node to have any number of children. This flexibility makes it ideal for:

- File system hierarchies (directories with multiple files)
- Organization charts (managers with many subordinates)
- XML/DOM trees (elements with variable child counts)
- Scene graphs (transforms with multiple entities)
- Parse trees (where grammar rules can have many alternatives)

### Pointer-Based Storage

Nodes are stored as `std::unique_ptr<TreeNode>` within a `Container` (default: `std::vector`). This design:

- Prevents moving old pointers (node pointers remain valid across insertions)
- Enables precise parent/child relationship tracking
- Allows O (1) sibling navigation via stored sibling indices

### SIMD-Accelerated Statistics

The `tree_simd` namespace provides Google Highway acceleration for:

- Node ID searches via vectorized equality comparison
- Sum and max aggregation for tree analysis
- Accessible to users for custom statistics computation

### Lazy Memory Pools

The optional `NodeMemoryPool<T>` provides:

- Cache-line aligned allocation for better performance
- Automatic exponential block growth
- Free-list reuse for deallocated nodes
- Visible to algorithms that require custom memory strategies

---

## 3. Core Components

### 3.1 Error Types

```cpp
enum class TreeError {
    NodeNotFound,        // Requested node does not exist
    InvalidOperation,    // Operation not allowed in current state
    SerializationError,  // Serialization/deserialization failed
    IteratorInvalidated, // Iterator was invalidated
    ThreadSafetyViolation // Concurrent access detected
};

template<typename T>
using TreeResult = std::expected<T, TreeError>;
```

### 3.2 Concepts

```cpp
// Data types must be copyable and movable
template<typename T>
concept TreeData = std::copyable<T> && std::movable<T>;

// Metadata must be default-initializable and copyable
template<typename M>
concept Metadata = std::default_initializable<M> && std::copyable<M>;

// Serializable types must support stream I/O
template<typename T>
concept Serializable = requires(T t, std::ostream &os, std::istream &is) {
    os << t;
    is >> t;
};

// Container must support iterator protocol and basic vector-like operations
template<typename C, typename NodePtr>
concept NodeContainer = requires(C c, NodePtr p) {
    typename C::iterator;
    typename C::const_iterator;
    { c.begin() } -> std::same_as<typename C::iterator>;
    { c.end() } -> std::same_as<typename C::iterator>;
    c.push_back(std::move(p));
    { c.empty() } -> std::convertible_to<bool>;
    c.erase(c.begin());
};
```

### 3.3 Metadata

```cpp
struct EmptyMetadata {
    constexpr EmptyMetadata() = default;
    constexpr bool operator==(const EmptyMetadata &) const = default;
};

inline std::ostream &operator<<(std::ostream &os, const EmptyMetadata &);
inline std::istream &operator>>(std::istream &is, EmptyMetadata &);
```

Empty metadata is the default, useful when you only need to store node data without auxiliary information. For custom
metadata, define your own serializable type.

**Metadata Usage Examples:**

Custom metadata types attach auxiliary information to nodes:

```cpp
// Node weights (for weighted tree algorithms)
struct NodeWeight {
    double weight = 1.0;
    constexpr bool operator==(const NodeWeight &other) const = default;
    friend std::ostream &operator<<(std::ostream &os, const NodeWeight &w) {
        return os << w.weight;
    }
    friend std::istream &operator>>(std::istream &is, NodeWeight &w) {
        return is >> w.weight;
    }
};

using WeightedTree = NAryTree<std::string, NodeWeight>;
WeightedTree tree("root");
auto child = tree.insert(tree.get_root(), "child", NodeWeight{2.5});

// Node labels (for annotated hierarchies)
struct NodeLabel {
    std::string label;
    std::optional<size_t> category;
    // ... serialization operators
};

using LabeledTree = NAryTree<int, NodeLabel>;

// Node timestamps (for tracking modification history)
struct NodeTimestamp {
    long long created_at = 0;
    long long modified_at = 0;
    // ... serialization operators
};

using VersionedTree = NAryTree<std::string, NodeTimestamp>;
```

Metadata is copied/moved with nodes and preserved during tree transformations (map, filter, graft).

### 3.4 Memory Pool

```cpp
template<typename T>
class NodeMemoryPool {
    static constexpr size_t INITIAL_POOL_SIZE = 1024;
    static constexpr size_t ALIGNMENT = alignof(T);
    
    // Allocates aligned memory; tries free list first, then pool blocks
    void *allocate(size_t size, size_t align = ALIGNMENT);
    
    // Adds address to reuse free list
    void deallocate(void *ptr);
    
    // Clears all blocks and free list
    void clear();
    
    // Query allocated/used memory
    [[nodiscard]] size_t total_allocated() const;
    [[nodiscard]] size_t total_used() const;
};
```

**Usage:** Provide custom allocators to tree algorithms for fine-grained memory control. Not typically used directly by
end users.

**Custom Allocator Integration:**

`NodeMemoryPool` enables efficient memory reuse when building and destroying large trees:

```cpp
// Create a pool for nodes storing large data
NodeMemoryPool<std::vector<double>> pool;

// Custom allocator using the pool
struct CustomAllocator {
    NodeMemoryPool<std::vector<double>> *pool_ptr;
    
    std::vector<double> *allocate(size_t count) {
        return static_cast<std::vector<double> *>(
            pool_ptr->allocate(count * sizeof(std::vector<double>))
        );
    }
    
    void deallocate(std::vector<double> *ptr) {
        pool_ptr->deallocate(ptr);
    }
};

// Use pool with algorithms
NAryTree<std::vector<double>> tree(std::vector<double>{1.0, 2.0, 3.0});
auto root = tree.get_root();

// Insert many nodes; allocations reused from pool free list
for (int i = 0; i < 1000; ++i) {
    tree.insert(root, std::vector<double>(100, static_cast<double>(i)));
}

// Pool statistics
std::cout << "Allocated: " << pool.total_allocated() << " bytes\n";
std::cout << "Used: " << pool.total_used() << " bytes\n";

// Clear pool for next batch
pool.clear();
```

**Benefits:**

- **Cache locality**: Pool allocates aligned blocks; nodes stored contiguously reduce cache misses
- **Fragmentation reduction**: Free list reuse minimizes heap fragmentation
- **Predictable latency**: Pre-allocated pools avoid allocation spikes during insertions

---

### 3.5 SIMD Operations

```cpp
namespace tree_simd {
    // Find node ID in array (vectorized with Highway)
    int find_node_id(const size_t* ids, size_t count, size_t target);
    
    // Sum array elements (vectorized reduction)
    size_t sum(const size_t* data, size_t count);
    
    // Find maximum element (vectorized reduction)
    size_t max(const size_t* data, size_t count);
}
```

**Internal Use:** These are called by `analyze()` to efficiently compute tree statistics. Can also be called directly
for custom analysis.

**SIMD Usage Guide:**

Google Highway acceleration is transparent but available for custom extensions:

```cpp
// analyze() uses tree_simd internally
auto stats = tree.analyze();  // SIMD sum/max for statistics

// Direct SIMD calls for custom analysis
std::vector<size_t> node_ids;
std::vector<size_t> weights;
// ... populate arrays ...

// Vectorized search
int idx = tree_simd::find_node_id(node_ids.data(), node_ids.size(), target_id);

// Vectorized reductions
size_t total_weight = tree_simd::sum(weights.data(), weights.size());
size_t max_weight = tree_simd::max(weights.data(), weights.size());
```

**When to Use SIMD Acceleration:**

- Analyzing large trees (1000+ nodes): SIMD speedups become measurable (2-4x for vectorizable operations)
- Computing aggregate statistics: sum, max, count operations across node properties
- Custom tree algorithms requiring array scans: implement custom loops using `tree_simd` primitives

**Extending SIMD Acceleration:**

Highway provides portable SIMD across x86_64, ARM, WebAssembly. Add custom operations by:

1. Define operation in `tree_simd` namespace using Highway types (`hwy::ScalableTag`, `hwy::Vec<...>`)
2. Instantiate for target architecture at compile time
3. Fall back to scalar loops for unsupported platforms

Reference: `include/containers/tree/NAryTree.hpp` for `tree_simd` implementation patterns.

---

## 4. Tree Types & Templates

### 4.1 Main NAryTree Class

```cpp
template<
    TreeData T,
    Metadata Metadata = EmptyMetadata,
    template <typename...> typename Container = std::vector
>
    requires (std::move_constructible<T> && std::move_constructible<Metadata>)
class NAryTree {
    // Tree structure and operations
};
```

**Template Parameters:**

- `T` — Data type stored in each node
- `Metadata` — Optional metadata attached to nodes (default: `EmptyMetadata`)
- `Container` — Child container type (default: `std::vector`, must satisfy `NodeContainer`)

### 4.2 Pre-Defined Type Aliases

While not explicitly defined in the header, common usage patterns are:

```cpp
using SimpleTree = NAryTree<int>;
using StringTree = NAryTree<std::string>;
using LabeledTree = NAryTree<std::string, std::string>;  // data + metadata
using VectorTree = NAryTree<std::vector<double>>;
```

### 4.3 TreeNode Structure

```cpp
struct TreeNode {
    T data;                                        // Node payload
    Metadata metadata;                             // Optional metadata
    Container<std::unique_ptr<TreeNode>> children; // Child nodes
    TreeNode *parent = nullptr;                    // Parent node (null for root)
    size_t node_id = 0;                           // Unique node identifier
    size_t sibling_index = 0;                     // Index in parent's children list
    
    // Constructors (move-based for efficiency)
    explicit constexpr TreeNode(T value, Metadata meta, TreeNode *p, size_t id, size_t sib_idx = 0);
    template<typename... Args>
    constexpr TreeNode(TreeNode *p, Metadata meta, size_t id, Args &&... args);
    
    // Query methods (O(1))
    [[nodiscard]] constexpr bool is_leaf() const noexcept;
    [[nodiscard]] constexpr size_t child_count() const noexcept;
    [[nodiscard]] constexpr bool has_parent() const noexcept;
    
    // Sibling navigation (O(1) via stored indices)
    [[nodiscard]] TreeNode* next_sibling() const noexcept;
    [[nodiscard]] TreeNode* prev_sibling() const noexcept;
    
    // Structural equality (recursive)
    constexpr bool operator==(const TreeNode &other) const;
};
```

**Key Design Points:**

- `sibling_index` stored explicitly for O (1) sibling navigation
- Unique `node_id` for debugging and serialization
- Destructors automatically cascade (unique_ptr cleanup)

---

## 5. Node and Tree Management

### 5.1 Constructors and Assignment

```cpp
// Default constructor: empty tree
NAryTree() = default;

// Single-node tree with root data
explicit NAryTree(T root_data, Metadata root_metadata = Metadata{});

// Destructor (automatic cleanup via unique_ptr)
~NAryTree() = default;

// Move semantics (efficient for large trees)
NAryTree(NAryTree &&other) noexcept = default;
NAryTree &operator=(NAryTree &&other) noexcept = default;

// Copy semantics (deep copy)
NAryTree(const NAryTree &other);
NAryTree &operator=(const NAryTree &other);

// Subtree clone: create new tree from existing subtree
explicit NAryTree(const TreeNode &subtree_root);
```

### 5.2 Inserting Nodes

```cpp
// Insert node: returns pointer to new node (or nullptr if insert fails)
TreeNode *insert(TreeNode *parent, T data, Metadata metadata = Metadata{});

// Emplace construct node in-place
template<typename... Args>
TreeNode *emplace(TreeNode *parent, Metadata metadata, Args &&... args);

// Bulk insert multiple children (vector of data, metadata pairs)
std::vector<TreeNode *> bulk_insert(TreeNode *parent, 
                                     const std::vector<std::pair<T, Metadata>> &items);

// Insert from range (arbitrary input range)
template<std::ranges::range Range>
std::vector<TreeNode *> bulk_insert_range(TreeNode *parent, Range &&data_range);
```

**Special Handling:**

- Inserting into `nullptr` as parent inserts at root (if tree is empty)
- Multiple root insertions return `nullptr` (root already exists)
- Nodes are automatically assigned unique `node_id` values

```cpp
// Example: Build a simple tree
NAryTree<int> tree(10);  // root = 10
auto child1 = tree.insert(tree.get_root(), 20);
auto child2 = tree.insert(tree.get_root(), 30);
auto grandchild = tree.insert(child1, 40);
```

### 5.3 Removing Nodes

```cpp
// Remove node by value (first match)
bool remove(const T &value);

// Remove specific node
bool remove(TreeNode *node_to_remove);
```

**Semantics:**

- Removing root deletes entire tree
- Removing non-root node removes that node and its entire subtree
- Returns `true` if successful, `false` if node not found or invalid

### 5.4 Finding Nodes

```cpp
// Find first node with value (depth-first pre-order)
TreeNode *find(const T &value) const;

// Find all nodes with value (mutable version)
std::vector<TreeNode *> find_all(const T &value);

// Find all nodes with value (const version)
std::vector<const TreeNode *> find_all(const T &value) const;

// Find first node matching predicate
template<typename Predicate>
TreeNode *find_if(Predicate predicate) const;

// Find all nodes matching predicate (mutable)
template<typename Predicate>
std::vector<TreeNode *> find_all_if(Predicate predicate);

// Find all nodes matching predicate (const)
template<typename Predicate>
std::vector<const TreeNode *> find_all_if(Predicate predicate) const;
```

### 5.5 Clearing the Tree

```cpp
void clear();  // Erase all nodes; resets next_node_id to 1
```

### 5.6 Access Methods

```cpp
// Get root node pointer
TreeNode *get_root() const;

// Extract root ownership (for advanced operations)
std::unique_ptr<TreeNode> extract_root();
```

---

## 6. Tree Navigation

### 6.1 Introspection

```cpp
// Query tree size and structure
[[nodiscard]] size_t size() const;                    // Total node count
[[nodiscard]] size_t height() const;                  // Tree height (1 if root only, 0 if empty)
[[nodiscard]] bool empty() const;                     // Check if tree is empty

// Access data and metadata
node->data;         // Mutable (non-const tree)
node->metadata;     // Mutable (non-const tree)
node->children;     // Container of child unique_ptrs
```

### 6.2 Path Operations

```cpp
// Get path from node to root
std::vector<TreeNode *> path_to_root(TreeNode *node) const;

// Find path between two nodes (if connected)
std::optional<std::vector<size_t>> path_between(TreeNode *from, TreeNode *to) const;
// Returns indices in parent's children list; SIZE_MAX = "go up one level"

// Find lowest common ancestor
TreeNode *lowest_common_ancestor(TreeNode *a, TreeNode *b) const;
```

### 6.3 Sibling Navigation

```cpp
// O(1) sibling operations (stored in node)
node->next_sibling();   // Next sibling or nullptr
node->prev_sibling();   // Previous sibling or nullptr
node->sibling_index;    // Index in parent's children
```

---

## 7. Iteration & Traversal

### 7.1 Iterator Types

```cpp
// Pre-order traversal iterator (root before children)
class pre_order_iterator { /* ... */ };
class const_pre_order_iterator { /* ... */ };

// Post-order traversal iterator (children before root)
class post_order_iterator { /* ... */ };
// No separate const_post_order_iterator; use filtered const range

// Breadth-first (level-order) iterator
class breadth_first_iterator { /* ... */ };
class const_breadth_first_iterator { /* ... */ };
```

**Iterator Semantics:**

- Forward iterators (support `++` but not `--`)
- Dereference yields `TreeNode &` or `const TreeNode &`
- Equality/inequality comparable

### 7.2 Standard Iteration (Pre-Order)

```cpp
// Default: pre-order iteration
for (auto node : tree) {
    std::cout << node.data << "\n";  // Visit root, then all children recursively
}

// Range interface
auto nodes = tree.nodes();  // returns range of all nodes
for (const auto &node : tree.nodes()) {
    // ...
}

// Leaf-only filtering
for (const auto &node : tree.leaves()) {  // returns filtered range
    // ... only leaf nodes
}
```

### 7.3 Breadth-First Iteration

```cpp
// Iterator interface
for (auto it = tree.breadth_begin(); it != tree.breadth_end(); ++it) {
    std::cout << it->data << "\n";
}

// Range interface (preferred)
for (auto node : tree.breadth_first()) {
    std::cout << node.data << "\n";  // Level-order traversal
}
```

### 7.4 Post-Order Iteration

```cpp
for (auto node : tree.post_order()) {
    std::cout << node.data << "\n";  // Children before parents
}
```

### 7.5 Callback-Based Traversal

```cpp
// Depth-first pre-order with callback
tree.traverse_depth_first([](const TreeNode *node) {
    std::cout << node->data << "\n";
});

// Breadth-first with callback
tree.traverse_breadth_first([](const TreeNode *node) {
    std::cout << node->data << "\n";
});

// Custom traversal with ranges
for (size_t depth = 0; depth < tree.height(); ++depth) {
    auto level_nodes = tree.level(depth);
    for (const auto *node : level_nodes) {
        std::cout << node->data << " ";
    }
    std::cout << "\n";
}
```

### 7.6 Levels by Depth

```cpp
// Get all nodes at a specific depth
auto level_3_nodes = tree.level(3);  // returns std::vector<const TreeNode *>
```

### 7.7 Iterator Invalidation and Safety

**Invalidation Rules:**

Iterators become invalid when:

1. **Tree cleared**: `tree.clear()` invalidates all iterators
2. **Node in traversal path removed**: Removing a node along the iterator's path invalidates that iterator
3. **Tree structure modified during iteration**: Inserting/removing/grafting while iterating can invalidate forward
   iterators
4. **Node modified in-place**: Reassigning `node->data` is safe; modifying `node->children` directly is not

**Safe Practices:**

```cpp
// SAFE: collect node pointers before iteration
std::vector<TreeNode *> nodes_to_process;
for (auto node : tree) {
    nodes_to_process.push_back(&*node);
}
// Now tree can be modified; pointers still valid (unique_ptr design)

// SAFE: iterate over const tree (readers don't race with writers)
for (const auto &node : tree) {
    // Copy node data if needed; don't hold iterator across await points
    auto data = node.data;
    // ... use data safely
}

// UNSAFE: modifying tree during iteration
for (auto node : tree) {
    tree.insert(tree.get_root(), 42);  // Invalidates iterator!
    // ++node may crash or skip/revisit nodes
}

// SAFE: defer modifications
std::vector<TreeNode *> to_remove;
for (auto node : tree) {
    if (node.data > 100) {
        to_remove.push_back(&*node);
    }
}
for (auto node : to_remove) {
    tree.remove(node);  // Now safe to modify
}
```

**Thread-Safe Iteration:**

With `ThreadSafeNAryTree`, iterators are only valid within `with_lock()` scope:

```cpp
// Iterator valid only inside lock
safe_tree.with_lock([&](const auto &t) {
    for (auto node : t) {
        process(node.data);  // Safe; lock held
    }
    // Iterator invalid here
});
```

---

## 8. Tree Analysis & Statistics

### 8.1 Statistics Structure

```cpp
struct TreeStats {
    size_t node_count = 0;              // Total nodes
    size_t leaf_count = 0;              // Number of leaf nodes
    size_t max_depth = 0;               // Maximum depth from root
    size_t min_depth = 0;               // Minimum depth from root
    double avg_branching_factor = 0.0;  // (node_count - 1) / (node_count - leaf_count)
    size_t max_children = 0;            // Maximum children of any node
};
```

### 8.2 Computing Statistics

```cpp
// Analyze tree structure (SIMD-accelerated where applicable)
TreeStats stats = tree.analyze();

std::cout << "Nodes: " << stats.node_count << "\n";
std::cout << "Height: " << stats.max_depth << "\n";
std::cout << "Branching factor: " << stats.avg_branching_factor << "\n";
```

### 8.3 Structural Comparison

```cpp
// Check if two trees have identical structure
bool equal = tree1.structural_equal(tree2);

// Compute structural hash (for indexing, caching)
size_t hash = tree.structural_hash();
```

---

## 9. Tree Transformations

### 9.1 Mapping (Using C++23 Explicit Object Parameter)

```cpp
// Transform tree data: map(func) returns new tree with transformed values
template<typename Self, typename F>
auto map(this Self &&self, F &&func) -> NAryTree<std::invoke_result_t<F, T>>;

// Example: double all integer values
NAryTree<int> tree(10);
tree.insert(tree.get_root(), 20);  // child

auto doubled = tree.map([](int x) { return x * 2; });
// doubled has root = 20, child = 40
```

### 9.2 Filtering

```cpp
// Create new tree with only nodes matching predicate
template<typename Predicate>
NAryTree filter(Predicate &&pred) const;

// Example: keep only values > 15
auto filtered = tree.filter([](const TreeNode &node) {
    return node.data > 15;  // Only root (10) filtered out in this case
});
```

### 9.3 Grafting (Attaching Subtrees)

```cpp
// Attach entire subtree to target node
void graft(TreeNode *target, NAryTree &&subtree);

// Example: graft subtree into tree
NAryTree<int> main_tree(1);
auto child = main_tree.insert(main_tree.get_root(), 2);

NAryTree<int> subtree(100);
subtree.insert(subtree.get_root(), 101);

main_tree.graft(child, std::move(subtree));
// main_tree now: 1 -> 2 -> 100 -> 101
```

### 9.4 Splitting (Extracting Subtrees)

```cpp
// Extract subtree rooted at node into new tree
NAryTree split(TreeNode *node);

// Example: extract subtree starting at child
auto extracted = main_tree.split(child);
// extracted contains child and its descendants
// main_tree no longer has child
```

### 9.5 Merging Trees

```cpp
// Merge other into this tree (other becomes child of this root)
void merge(NAryTree &&other);

// Example: merge two trees
NAryTree<int> tree1(1);
tree1.insert(tree1.get_root(), 2);

NAryTree<int> tree2(10);
tree2.insert(tree2.get_root(), 11);

tree1.merge(std::move(tree2));
// tree1 root now has children: 2 and 10 (with 10's subtree)
```

---

## 10. Serialization

### 10.1 Basic Text Serialization

```cpp
// Serialize tree to output stream
void serialize(std::ostream &os) const;

// Deserialize tree from input stream
void deserialize(std::istream &is);

// Example
std::ofstream file("tree.txt");
tree.serialize(file);
file.close();

std::ifstream file2("tree.txt");
NAryTree<int> loaded_tree;
loaded_tree.deserialize(file2);
```

### 10.2 Versioned Serialization

```cpp
static constexpr uint32_t SERIALIZATION_VERSION = 1;

// Serialize with version header
void serialize_versioned(std::ostream &os) const;

// Deserialize with version check
bool deserialize_versioned(std::istream &is);
// Returns false if version mismatch
```

### 10.3 JSON Serialization

```cpp
// Serialize tree to JSON format
void serialize_json(std::ostream &os) const;

// Example
std::ofstream json_file("tree.json");
tree.serialize_json(json_file);
```

**Format:** Recursive JSON with "data", "id", and optional "children" fields.

---

## 11. Advanced Features

### 11.1 Builder Pattern

```cpp
class TreeBuilder {
public:
    TreeBuilder &root(T value, Metadata meta = Metadata{});
    TreeBuilder &child(T value, Metadata meta = Metadata{});
    TreeBuilder &sibling(T value, Metadata meta = Metadata{});
    TreeBuilder &parent();
    
    NAryTree build() &&;  // Takes ownership; rvalues only
};

// Example: fluent tree construction
auto tree = TreeBuilder<int>()
    .root(1)
    .child(2)
    .child(3)
    .sibling(4)
    .parent()
    .child(5)
    .build();

// Result:        1
//               /|\
//              2 4 5
//             /
//            3
```

### 11.2 Try-Catch Operations

```cpp
// Wrapped operations returning std::expected
TreeResult<TreeNode *> try_insert(TreeNode *parent, T data, Metadata metadata = Metadata{});
TreeResult<void> try_remove(TreeNode *node);

// Generic catch-all
template<typename F>
auto try_operation(F &&func) const -> TreeResult<std::invoke_result_t<F>>;

// Example
auto result = tree.try_insert(node, 42);
if (result) {
    std::cout << "Inserted node " << result->node_id << "\n";
} else {
    std::cerr << "Error: " << static_cast<int>(result.error()) << "\n";
}
```

### 11.3 Visualization

```cpp
// ASCII tree display
std::string to_string() const;
void print_tree(std::ostream &os = std::cout) const;

// Graphviz DOT format for external visualization
std::string to_dot() const;

// Example
std::cout << tree.to_string();
// Output:
// └── 1
//     ├── 2
//     │   └── 3
//     └── 4
```

---

## 12. Thread-Safe Operations

### 12.1 ThreadSafeNAryTree Wrapper

```cpp
template<
    TreeData T,
    Metadata Metadata = EmptyMetadata,
    template <typename...> typename Container = std::vector,
    typename Mutex = std::shared_mutex
>
class ThreadSafeNAryTree {
    // Thread-guarded wrapper around NAryTree<T, Metadata, Container>
};
```

**Design:**

- Uses `std::shared_mutex` by default (allows concurrent reads, exclusive writes)
- Configurable mutex type via template parameter
- Automatically acquires locks for all operations

### 12.2 Thread-Safe Interface

```cpp
// Mutating operations use unique_lock
template<typename... Args>
auto insert(Args &&... args);

template<typename... Args>
bool remove(Args &&... args);

// Read operations use shared_lock
template<typename... Args>
auto find(Args &&... args) const;

[[nodiscard]] size_t size() const;

// Custom operations with explicit locking
template<typename F>
auto with_lock(F &&func) -> std::invoke_result_t<F, const NAryTree<...> &>;

template<typename F>
auto with_unique_lock(F &&func) -> std::invoke_result_t<F, NAryTree<...> &>;
```

### 12.3 Example: Concurrent Access

```cpp
ThreadSafeNAryTree<std::string> safe_tree("root");

// Thread A: insert
std::thread t1([&]() {
    safe_tree.insert(safe_tree.with_lock([](const auto &t) { return t.get_root(); }), "child1");
});

// Thread B: read
std::thread t2([&]() {
    auto size = safe_tree.size();
    std::cout << "Tree size: " << size << "\n";
});

t1.join();
t2.join();
```

### 12.4 Thread Safety Best Practices and Limitations

**Safe Patterns:**

- Concurrent reads: Multiple threads can call `find()`, `size()`, `analyze()` simultaneously
- Exclusive writes: Mutations (insert, remove, graft) require unique lock; serialized with readers
- Lock composition: Use `with_lock()` and `with_unique_lock()` to perform multi-step operations atomically

```cpp
// Safe: reads and modifications properly serialized
ThreadSafeNAryTree<int> tree(1);

std::thread reader([&]() {
    for (int i = 0; i < 100; ++i) {
        auto size = tree.size();  // shared_lock acquired/released per call
        std::cout << "Size: " << size << "\n";
    }
});

std::thread writer([&]() {
    tree.with_unique_lock([&](auto &t) {
        auto root = t.get_root();
        t.insert(root, 10);
        t.insert(root, 20);  // Both inserts atomic w.r.t. readers
    });
});

reader.join();
writer.join();
```

**Critical Limitations:**

- **Node pointers not safe outside lock**: A pointer returned by `find()` is only valid within the lock scope. Released
  pointers must not be dereferenced.
- **Iterator invalidation under concurrent access**: Iterators obtained inside `with_lock()` become invalid after lock
  release; tree may be modified by other threads.
- **Metadata races on non-atomic types**: If metadata contains non-atomic fields and multiple threads modify the same
  node's metadata, external synchronization required.

**Unsafe Pattern (DO NOT USE):**

```cpp
// WRONG: pointer escapes lock scope
TreeNode *escaped_ptr = nullptr;
tree.with_lock([&](const auto &t) {
    escaped_ptr = t.find(42);  // Pointer released with lock
});
// escaped_ptr may be dangling if tree modified by another thread

// WRONG: iterator used after lock release
auto node_data = *tree.with_lock([&](const auto &t) {
    return t.begin();  // Iterator invalid after lock release
});
```

**Best Practice:**

Perform all node access within `with_lock()` scope. Copy node data out; never export node pointers or iterators.

```cpp
// CORRECT: data copied within lock
int node_value = 0;
tree.with_lock([&](const auto &t) {
    if (auto node = t.find(42)) {
        node_value = node->data;  // Copy data, not pointer
    }
});
// node_value safe to use outside lock
```

---

## 13. Error Handling

### 13.1 Exception-Based (Implicit Errors)

Most operations that can fail follow the pattern:

```cpp
try {
    TreeNode *node = tree.insert(parent, value);
    if (!node) {
        std::cerr << "Cannot insert (root already exists or parent invalid)\n";
    }
} catch (const std::exception &e) {
    std::cerr << "Exception during insert: " << e.what() << "\n";
}
```

### 13.2 Expected-Based (Explicit Errors)

Use try-* variants for explicit error handling:

```cpp
auto result = tree.try_insert(parent, value);
if (result) {
    std::cout << "Inserted successfully\n";
} else {
    switch (result.error()) {
        case TreeError::NodeNotFound:
            std::cerr << "Parent node not found\n";
            break;
        case TreeError::InvalidOperation:
            std::cerr << "Cannot insert at this location\n";
            break;
        default:
            std::cerr << "Unknown error\n";
    }
}
```

### 13.3 Iterator Invalidation

Iterators are **invalidated** when:

- Tree is cleared
- A node in the traversal path is removed
- Tree structure is modified during iteration

**Safe:** Copy node pointers before iteration; iterate over const tree if possible.

---

## 14. Examples

### Example 1: Simple Hierarchy (File System)

```cpp
#include "containers/tree/NAryTree.hpp"
#include <iostream>

int main() {
    NAryTree<std::string> fs("root/");
    auto root = fs.get_root();
    
    auto dir1 = fs.insert(root, "documents/");
    auto dir2 = fs.insert(root, "downloads/");
    
    fs.insert(dir1, "resume.pdf");
    fs.insert(dir1, "letter.txt");
    fs.insert(dir2, "image.jpg");
    
    fs.print_tree();
    // Output:
    // └── root/
    //     ├── documents/
    //     │   ├── resume.pdf
    //     │   └── letter.txt
    //     └── downloads/
    //         └── image.jpg
    
    return 0;
}
```

### Example 2: Building with TreeBuilder

```cpp
#include "containers/tree/NAryTree.hpp"

int main() {
    auto tree = TreeBuilder<int>()
        .root(1, EmptyMetadata{})
        .child(2)
        .child(3)
        .sibling(4)
        .parent()
        .child(5)
        .build();
    
    std::cout << "Tree size: " << tree.size() << "\n";
    std::cout << "Tree height: " << tree.height() << "\n";
    
    for (const auto &node : tree.breadth_first()) {
        std::cout << node.data << " ";
    }
    std::cout << "\n";  // Output: 1 2 4 5 3
    
    return 0;
}
```

### Example 3: Finding and Filtering

```cpp
#include "containers/tree/NAryTree.hpp"

int main() {
    NAryTree<int> tree(10);
    auto r = tree.get_root();
    auto c1 = tree.insert(r, 20);
    auto c2 = tree.insert(r, 15);
    auto c3 = tree.insert(c1, 25);
    
    // Find first node > 20
    auto found = tree.find_if([](const auto &node) { return node.data > 20; });
    if (found) {
        std::cout << "Found: " << found->data << "\n";  // 25
    }
    
    // Find all nodes >= 20
    auto all_found = tree.find_all_if([](const auto &node) { return node.data >= 20; });
    std::cout << "Count >= 20: " << all_found.size() << "\n";  // 2 (20, 25)
    
    // Filter tree to nodes >= 15
    auto filtered = tree.filter([](const auto &node) { return node.data >= 15; });
    std::cout << "Filtered size: " << filtered.size() << "\n";  // 4 (10, 20, 15, 25)
    
    return 0;
}
```

### Example 4: Tree Statistics

```cpp
#include "containers/tree/NAryTree.hpp"

int main() {
    NAryTree<int> tree(1);
    auto r = tree.get_root();
    
    auto c1 = tree.insert(r, 2);
    auto c2 = tree.insert(r, 3);
    auto c3 = tree.insert(r, 4);
    auto c4 = tree.insert(c1, 5);
    auto c5 = tree.insert(c1, 6);
    
    auto stats = tree.analyze();
    
    std::cout << "Nodes: " << stats.node_count << "\n";           // 6
    std::cout << "Leaves: " << stats.leaf_count << "\n";          // 4 (3, 4, 5, 6)
    std::cout << "Max depth: " << stats.max_depth << "\n";        // 2
    std::cout << "Max children: " << stats.max_children << "\n";  // 3
    std::cout << "Avg branching: " << stats.avg_branching_factor << "\n";
    
    return 0;
}
```

### Example 5: Serialization and Roundtrip

```cpp
#include "containers/tree/NAryTree.hpp"
#include <fstream>

int main() {
    // Create and populate tree
    NAryTree<int> tree(1);
    auto r = tree.get_root();
    tree.insert(r, 2);
    tree.insert(r, 3);
    
    // Serialize
    std::ofstream out("tree.txt");
    tree.serialize_versioned(out);
    out.close();
    
    // Deserialize
    NAryTree<int> loaded;
    std::ifstream in("tree.txt");
    if (loaded.deserialize_versioned(in)) {
        std::cout << "Loaded successfully\n";
        loaded.print_tree();
    } else {
        std::cerr << "Version mismatch\n";
    }
    
    return 0;
}
```

### Example 6: Graft and Split Operations

```cpp
#include "containers/tree/NAryTree.hpp"

int main() {
    NAryTree<int> tree1(1);
    auto r1 = tree1.get_root();
    auto n2 = tree1.insert(r1, 2);
    
    NAryTree<int> tree2(10);
    auto r2 = tree2.get_root();
    tree2.insert(r2, 11);
    tree2.insert(r2, 12);
    
    // Graft tree2 into tree1 at node n2
    tree1.graft(n2, std::move(tree2));
    std::cout << "After graft:\n";
    tree1.print_tree();
    // 1
    // └── 2
    //     └── 10
    //         ├── 11
    //         └── 12
    
    // Split subtree at n2
    auto extracted = tree1.split(n2);
    std::cout << "\nExtracted:\n";
    extracted.print_tree();
    // 2
    // └── 10
    //     ├── 11
    //     └── 12
    
    std::cout << "\nRemaining tree:\n";
    tree1.print_tree();
    // 1
    
    return 0;
}
```

### Example 7: Path Operations

```cpp
#include "containers/tree/NAryTree.hpp"

int main() {
    NAryTree<int> tree(1);
    auto r = tree.get_root();
    auto c1 = tree.insert(r, 2);
    auto c2 = tree.insert(r, 3);
    auto g1 = tree.insert(c1, 4);
    auto g2 = tree.insert(c1, 5);
    
    // Path from leaf to root
    auto path = tree.path_to_root(g1);
    std::cout << "Path to root: ";
    for (const auto *node : path) {
        std::cout << node->data << " <- ";
    }
    std::cout << "\n";  // 4 <- 2 <- 1 <-
    
    // LCA
    auto lca = tree.lowest_common_ancestor(g1, g2);
    std::cout << "LCA of 4 and 5: " << lca->data << "\n";  // 2
    
    return 0;
}
```

### Example 8: Thread-Safe Tree

```cpp
#include "containers/tree/NAryTree.hpp"
#include <thread>
#include <vector>

int main() {
    ThreadSafeNAryTree<int> safe_tree(1);
    
    // Writer thread
    auto writer = [&]() {
        for (int i = 0; i < 10; ++i) {
            safe_tree.with_unique_lock([&](auto &t) {
                if (auto root = t.get_root()) {
                    t.insert(root, 100 + i);
                }
            });
        }
    };
    
    // Reader thread
    auto reader = [&]() {
        for (int i = 0; i < 20; ++i) {
            auto size = safe_tree.size();
            std::cout << "Size: " << size << "\n";
        }
    };
    
    std::thread w(writer);
    std::thread r(reader);
    
    w.join();
    r.join();
    
    std::cout << "Final size: " << safe_tree.size() << "\n";
    
    return 0;
}
```

### Example 9: Mapping and Transformation

```cpp
#include "containers/tree/NAryTree.hpp"

int main() {
    NAryTree<int> int_tree(10);
    auto r = int_tree.get_root();
    int_tree.insert(r, 20);
    int_tree.insert(r, 30);
    
    // Map integers to strings
    auto str_tree = int_tree.map([](int x) {
        return "Value: " + std::to_string(x);
    });
    
    // Result: NAryTree<std::string>
    str_tree.print_tree();
    // └── Value: 10
    //     ├── Value: 20
    //     └── Value: 30
    
    return 0;
}
```

### Example 10: DOT Export for Visualization

```cpp
#include "containers/tree/NAryTree.hpp"
#include <fstream>

int main() {
    NAryTree<std::string> tree("root");
    auto r = tree.get_root();
    tree.insert(r, "child1");
    tree.insert(r, "child2");
    tree.insert(tree.insert(r, "parent_of_grandchild"), "grandchild");
    
    // Export DOT for Graphviz
    std::ofstream dot_file("tree.dot");
    dot_file << tree.to_dot();
    dot_file.close();
    
    // Visualize: dot -Tpng tree.dot -o tree.png
    
    return 0;
}
```

---

## Appendix: Quick Reference

| Feature           | Function               | Return Type                          | Signature                                                                                                       |
|-------------------|------------------------|--------------------------------------|-----------------------------------------------------------------------------------------------------------------|
| **Construction**  | Default                | `NAryTree`                           | `NAryTree()`                                                                                                    |
|                   | Root node              | `NAryTree`                           | `explicit NAryTree(T root_data, Metadata meta = Metadata{})`                                                    |
|                   | Subtree clone          | `NAryTree`                           | `explicit NAryTree(const TreeNode &subtree_root)`                                                               |
| **Insertion**     | Insert single          | `TreeNode *`                         | `TreeNode *insert(TreeNode *parent, T data, Metadata meta = Metadata{})`                                        |
|                   | Emplace construct      | `TreeNode *`                         | `template<typename... Args> TreeNode *emplace(TreeNode *parent, Metadata meta, Args &&... args)`                |
|                   | Bulk insert            | `std::vector<TreeNode *>`            | `std::vector<TreeNode *> bulk_insert(TreeNode *parent, const std::vector<std::pair<T, Metadata>> &items)`       |
|                   | Insert from range      | `std::vector<TreeNode *>`            | `template<std::ranges::range Range> std::vector<TreeNode *> bulk_insert_range(TreeNode *parent, Range &&range)` |
| **Removal**       | By value               | `bool`                               | `bool remove(const T &value)`                                                                                   |
|                   | By pointer             | `bool`                               | `bool remove(TreeNode *node_to_remove)`                                                                         |
| **Finding**       | By value (first)       | `TreeNode *`                         | `TreeNode *find(const T &value) const`                                                                          |
|                   | By value (all)         | `std::vector<TreeNode *>`            | `std::vector<...> find_all(const T &value)`                                                                     |
|                   | By predicate           | `TreeNode *`                         | `TreeNode *find_if(Predicate pred) const`                                                                       |
|                   | By predicate (all)     | `std::vector<TreeNode *>`            | `std::vector<TreeNode *> find_all_if(Predicate pred)`                                                           |
| **Queries**       | Size                   | `size_t`                             | `[[nodiscard]] size_t size() const`                                                                             |
|                   | Height                 | `size_t`                             | `[[nodiscard]] size_t height() const`                                                                           |
|                   | Empty check            | `bool`                               | `[[nodiscard]] bool empty() const`                                                                              |
|                   | Root access            | `TreeNode *`                         | `TreeNode *get_root() const`                                                                                    |
| **Traversal**     | Pre-order iter         | `iterator/const_iterator`            | `begin()`, `end()`, `cbegin()`, `cend()`                                                                        |
|                   | Breadth-first          | `breadth_first_iterator`             | `breadth_begin()`, `breadth_end()`, `breadth_first()`                                                           |
|                   | Post-order             | `post_order_iterator`                | `post_order()`                                                                                                  |
|                   | Depth-first callback   | `void`                               | `void traverse_depth_first(const std::function<void(const TreeNode *)> &visit)`                                 |
|                   | Breadth-first callback | `void`                               | `void traverse_breadth_first(const std::function<void(const TreeNode *)> &visit)`                               |
| **Paths**         | To root                | `std::vector<TreeNode *>`            | `std::vector<TreeNode *> path_to_root(TreeNode *node)`                                                          |
|                   | Between nodes          | `std::optional<std::vector<size_t>>` | `std::optional<std::vector<size_t>> path_between(TreeNode *from, TreeNode *to)`                                 |
|                   | LCA                    | `TreeNode *`                         | `TreeNode *lowest_common_ancestor(TreeNode *a, TreeNode *b)`                                                    |
| **Transforms**    | Map                    | `NAryTree<mapped_type>`              | `auto map(this Self &&self, F &&func)`                                                                          |
|                   | Filter                 | `NAryTree`                           | `NAryTree filter(Predicate &&pred)`                                                                             |
|                   | Graft                  | `void`                               | `void graft(TreeNode *target, NAryTree &&subtree)`                                                              |
|                   | Split                  | `NAryTree`                           | `NAryTree split(TreeNode *node)`                                                                                |
|                   | Merge                  | `void`                               | `void merge(NAryTree &&other)`                                                                                  |
| **Analysis**      | Stats                  | `TreeStats`                          | `TreeStats analyze()`                                                                                           |
|                   | Structural equal       | `bool`                               | `bool structural_equal(const NAryTree &other)`                                                                  |
|                   | Structural hash        | `size_t`                             | `[[nodiscard]] size_t structural_hash()`                                                                        |
| **Serialization** | Text                   | `void`                               | `void serialize(std::ostream &os)`, `void deserialize(std::istream &is)`                                        |
|                   | Versioned              | `bool`                               | `void serialize_versioned(std::ostream &os)`, `bool deserialize_versioned(std::istream &is)`                    |
|                   | JSON                   | `void`                               | `void serialize_json(std::ostream &os)`                                                                         |
| **Display**       | ASCII                  | `std::string/void`                   | `[[nodiscard]] std::string to_string()`, `void print_tree(std::ostream &os = std::cout)`                        |
|                   | DOT                    | `std::string`                        | `[[nodiscard]] std::string to_dot()`                                                                            |
| **Builder**       | `TreeBuilder`          | `NAryTree`                           | Fluent tree construction                                                                                        |
| **Thread-Safe**   | `ThreadSafeNAryTree`   | Thread-guarded                       | Mutex-guarded wrapper                                                                                           |
| **Try-Catch**     | `try_insert`           | `TreeResult<TreeNode *>`             | `TreeResult<TreeNode *> try_insert(...)`                                                                        |
|                   | `try_remove`           | `TreeResult<void>`                   | `TreeResult<void> try_remove(...)`                                                                              |
|                   | `try_operation`        | `TreeResult<T>`                      | `template<typename F> auto try_operation(F &&func)`                                                             |

---

## Performance Characteristics

| Operation                               | Complexity             | Notes                                             |
|-----------------------------------------|------------------------|---------------------------------------------------|
| Insert node                             | O(1)                   | Amortized; appends to parent's children           |
| Remove node                             | O(n)                   | n = size of subtree; must erase from parent       |
| Find (value)                            | O(n)                   | n = tree size; pre-order traversal                |
| Find (predicate)                        | O(n)                   | n = tree size                                     |
| Height                                  | O(n)                   | Recursive; can be cached                          |
| Analyze                                 | O(n)                   | SIMD-accelerated reductions                       |
| Sibling nav                             | O(1)                   | Pre-computed sibling_index                        |
| LCA                                     | O(h)                   | h = height of tree                                |
| Path to root                            | O(h)                   | h = height                                        |
| Serialize                               | O(n)                   | Single pass traversal                             |
| Map transform                           | O(n)                   | New tree construction                             |
| Filter                                  | O(n)                   | Recursive predicate check                         |
| **Pre-order iterator construction**     | **O(1)**               | **Stack-based; minimal setup**                    |
| **Breadth-first iterator construction** | **O(1)**               | **Queue pre-allocated for initial level**         |
| **BFS queue overhead**                  | **O(max_level_width)** | **Memory: queue holds all nodes at widest level** |

---

## Design Decisions

1. **Pointer-based storage**: Nodes accessed by pointer, not index. Enables parent/child tracking without rehashing.

2. **Unique IDs**: Every node has auto-incremented unique ID for debugging, serialization, and structural hashing.

3. **Sibling indices**: O (1) sibling navigation via stored index in parent's children list.

4. **Copy semantics**: Copy constructor performs deep copy (independent tree); move semantics transfer ownership
   cheaply.

5. **Container template**: Default `std::vector` for cache locality; can use `std::list` for cheap removals.

6. **Google Highway SIMD**: Accelerates statistics (sum, max, search) but not tree structure (inherently
   pointer-chased).

7. **Thread-safe wrapper**: Separate `ThreadSafeNAryTree` class rather than intrusive locking; users choose when to
   incur lock overhead.

8. **Builder pattern**: Fluent interface for readable tree construction in single expression.

9. **Try-operations**: `expected`-based versions for explicit error handling; traditional operations for ergonomics.

---

## Compatibility

- **C++ Standard**: C++23 (`-std=c++2b`)
- **Platforms**: Any with Google Highway support (x86_64, ARM, etc.)
- **Headers**: `<memory>`, `<vector>`, `<queue>`, `<functional>`, `<ranges>`, `<expected>`, etc.
- **Concepts**: Requires C++20+ compiler (concepts support)

**macOS C++23 Compiler Requirements:**

| macOS Version | Clang/LLVM Version    | C++23 Support                               | Recommendation                            |
|---------------|-----------------------|---------------------------------------------|-------------------------------------------|
| 14 Sonoma     | clang 15.0+ (default) | Full                                        | ✅ Supported                              |
| 13 Ventura    | clang 14.0+ (default) | Partial (requires `-fexperimental-library`) | ⚠️ Test carefully                         |
| 12 Monterey   | clang 13.0 (default)  | Limited                                     | ❌ Upgrade or use `-std=c++2b` explicitly |

**Build Configuration:**

```bash
# macOS with C++23 support
clang++ -std=c++2b -I/opt/homebrew/include -L/opt/homebrew/lib \
    -fexperimental-library my_program.cpp -o my_program

# With GCC on macOS (if available)
g++ -std=c++2b -fconcepts my_program.cpp -o my_program

# CMake
cmake -DCMAKE_CXX_STANDARD=23 -DCMAKE_CXX_FLAGS="-fexperimental-library" ..
```

**Known Compatibility Issues:**

- **macOS clang < 15.0**: C++23 concepts may not be fully supported; fallback to C++20 or upgrade toolchain
- **Google Highway on Apple Silicon**: Ensure Highway built with correct architecture flags (`-arch arm64`)
- **Ranges library**: Standard library ranges may require `-fexperimental-library` on older clang versions

**Verification:**

```cpp
// Check C++23 support at compile time
#if __cplusplus >= 202302L
    // C++23 features available
#else
    #error "C++23 or later required"
#endif
```

---

## 15. `ScalableNAryTree` — High-Scale Flat LCRS SoA Tree

For extreme multi-million-scale scenarios ($10\text{M}+$ nodes) where traditional pointer-chased recursive trees induce cache misses and allocator overhead, Pebble provides `pebble::containers::ScalableNAryTree<T, Allocator>` in `include/containers/tree/NAryTree.hpp`.

### Key Capabilities

1. **Flat Left-Child Right-Sibling (LCRS) Array SoA**: Decouples tree topology (`NodeHeader` vectors) from node payload data (`data_` vector).
2. **True $O(1)$ Child Appends**: `NodeHeader` maintains `last_child` index for instant tail insertion without linear traversal over siblings.
3. **Zero-Heap Traversal Stack**: `remove_subtree` and `for_each_dfs` use Small-Buffer Optimized `SmallVector<NodeId, 64>` stacks, completely eliminating call-stack recursion and heap allocations.
4. **Smriti Arena Support**: Parameterized with `Allocator = std::allocator<T>`, accepting `smriti::SmritiAllocator` for zero-cost bump/arena allocation.

```cpp
#include "containers/tree/NAryTree.hpp"

pebble::containers::ScalableNAryTree<int> tree;
tree.reserve(1000000);

auto root = tree.insert_root(1);
auto c1 = tree.append_child(root, 10); // O(1)
auto c2 = tree.append_child(root, 20); // O(1)

tree.for_each_dfs([](const int& val, const auto& header) {
    // Process nodes with zero heap allocation
});
```

---

## License and Attribution

This tree container is part of the Pebble library.


