# B+ Tree (`pebble::containers::BPlusTree`)

**`pebble::containers::BPlusTree`** (`include/containers/tree/bplus_tree.hpp`) is Pebble's high-performance, policy-based, zero-overhead **B+ Tree** container implemented in modern C++23/C++26. It features Structure-of-Arrays (SoA) node layouts, SIMD-accelerated key searches (Google Highway), intrusive freelist node recycling, transparent string-view lookups, and $O(N)$ bottom-up bulk loading.

---

## 1. Architectural Overview & SoA Cache Alignment

```
                         B+ TREE NODE CACHE TOPOLOGY (64-BYTE ALIGNED)
                         
   InnerNode (Branching Routing):
   ┌──────────────────────┬──────────────────────┬──────────────────────┐
   │ Keys (SIMD Aligned)  │ k0 | k1 | k2 | k3    │ (Loaded in 1 AVX/NEON)│
   ├──────────────────────┼──────────────────────┴──────────────────────┤
   │ Child Pointers       │ ptr0 | ptr1 | ptr2 | ptr3 | ptr4            │
   └──────────────────────┴─────────────────────────────────────────────┘
                                  │
                                  ▼
   LeafNode (Data Storage - Structure of Arrays):
   ┌──────────────────────┬─────────────────────────────────────────────┐
   │ Keys Array (Contig.) │ "alpha" | "beta" | "delta" | "gamma"        │ <-- Clean Cache Lines
   ├──────────────────────┼─────────────────────────────────────────────┤
   │ Values Array (Sep.)  │ 100     | 200    | 300     | 400            │ <-- Only read on hit!
   ├──────────────────────┼─────────────────────────────────────────────┤
   │ Linked Leaf Pointers │ PrevLeaf* ◄────────► NextLeaf*              │ <-- Fast O(1) Scans
   └──────────────────────┴─────────────────────────────────────────────┘
```

---

## 2. Key Algorithmic Mechanics

### 2.1 Highway SIMD Key Search
For numeric keys (integers, floats), inner and leaf nodes leverage SIMD vector instructions to evaluate 8 to 16 key comparisons in a single CPU cycle:
```cpp
// Evaluates branch index using vector comparisons
const auto keys_vec = hn::Load(d, &keys_[0]);
const auto target_vec = hn::Set(d, search_key);
const auto mask = hn::Lt(keys_vec, target_vec);
const size_t branch_idx = hn::CountTrue(d, mask);
```

### 2.2 Intrusive Freelist Node Recycling
To eliminate OS memory allocator thrashing during rapid insertion and deletion churn, `BPlusTree` retains an intrusive singly-linked freelist of up to `Traits::MaxRecycleNodes` deallocated nodes. Reallocated nodes reuse warm L1/L2 cache blocks immediately.

---

## 3. End-to-End API Guide

### 3.1 Transparent Zero-Allocation Lookups & Range Scans
```cpp
#include "containers/tree/bplus_tree.hpp"
#include <iostream>
#include <string>
#include <string_view>

int main() {
    // std::less<> enables heterogeneous / transparent string_view lookups
    pebble::containers::BPlusTree<std::string, int, std::less<>> map;

    // 1. Insertions
    map.insert_or_assign("alpha", 100);
    map.insert_or_assign("bravo", 200);
    map.insert_or_assign("charlie", 300);

    // 2. Zero-Allocation Lookup using string_view
    std::string_view query = "bravo";
    if (map.contains(query)) {
        std::cout << "Found " << query << ": " << map.at(query) << "\n";
    }

    // 3. High-Throughput Range Scan
    map.scan("a", "c~", [](std::string_view k, int val) {
        std::cout << "Scan item: " << k << " = " << val << "\n";
    });
}
```

### 3.2 $O(N)$ Bottom-Up Bulk Loading
```cpp
#include "containers/tree/bplus_tree.hpp"
#include <vector>

std::vector<std::pair<const int, std::string>> sorted_entries = {
    {1, "first"}, {2, "second"}, {3, "third"}, {4, "fourth"}
};

// Directly constructs a perfectly balanced B+ Tree in O(N) time
auto tree = pebble::containers::BPlusTree<int, std::string>::from_sorted(
    sorted_entries.begin(), sorted_entries.end()
);
```

### 3.3 Pravaha Parallel Map-Reduce over B+ Tree
```cpp
#include "containers/tree/bplus_tree.hpp"
#include "containers/tree/bplus_tree_pravaha.hpp"

pebble::containers::BPlusMap<int, int> big_tree;
// ... populate tree ...

// Parallel Map-Reduce across tree leaf ranges
long long total_sum = pebble::containers::pravaha::parallel_reduce(
    big_tree, 0, 100000, 0LL,
    [](long long a, long long b) { return a + b; },
    [](int k, int v) { return static_cast<long long>(v); }
);
```
