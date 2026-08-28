# B+ Tree (`pebble::containers::BPlusTree`)

A high-performance, policy-based, zero-overhead **B+ Tree** container implemented in modern C++23/C++26 for Pebble.

---

## 🚀 Key Architectural Highlights

- **Structure-of-Arrays (SoA) Node Layout**:
  - Keys and values in `LeafNode` are isolated into separate contiguous cache-aligned buffers.
  - Key search and SIMD sweeps only touch key cache lines, avoiding cache pollutant value loads until values are actually accessed.
- **Zero Virtual & Zero RTTI**: Pure compile-time policy-driven architecture with no dynamic polymorphism.
- **Cache-Line Aligned Nodes**: 64-byte alignment ensures optimal L1/L2 cache locality and eliminates false sharing.
- **SIMD Search Acceleration**: Google Highway SIMD support for vector-accelerated search across integer and float keys.
- **Transparent / Heterogeneous Lookups**:
  - Supports `std::is_transparent` comparators (e.g. `std::less<>`).
  - Query with `std::string_view` or `const char*` on `std::string` keys with **zero allocations**.
- **Hardware Software Prefetching**:
  - Emits prefetch hints on sequential node traversal in `.scan()` and forward iteration.
- **Intrusive Node Freelist Recycling**:
  - Internal freelist retains up to `Traits::MaxRecycleNodes` deallocated nodes, eliminating memory allocator thrashing under heavy insert/delete churn.
- **$O(N)$ Bottom-Up Bulk Loading**:
  - `BPlusTree::from_sorted(begin, end)` builds balanced B+ trees bottom-up with minimal memory allocations.

---

## 📦 Quick Start

### Ordered Map & Heterogeneous Lookups

```cpp
#include "containers/tree/bplus_tree.hpp"
#include <string>
#include <string_view>

using namespace pebble::containers;

int main() {
    // std::less<> enables heterogeneous / transparent lookups
    BPlusTree<std::string, int, std::less<>> map;

    // Insertion
    map.insert_or_assign("alpha", 100);
    map.insert_or_assign("beta", 200);

    // Transparent lookup using string_view without temporary std::string allocation
    std::string_view key = "alpha";
    if (map.contains(key)) {
        std::cout << "Value: " << map.at(key) << "\n";
    }

    // High-throughput range scan callback
    map.scan("a", "b~", [](std::string_view k, int val) {
        std::cout << k << " -> " << val << "\n";
    });

    return 0;
}
```

### Custom Branching Factor / Traits

```cpp
struct CustomTraits {
    static constexpr std::size_t LeafCapacity = 32;
    static constexpr std::size_t InnerCapacity = 32;
    static constexpr bool EnableSIMD = true;
};

pebble::containers::BPlusTree<uint64_t, uint64_t, std::less<uint64_t>, CustomTraits> tree;
```

---

## 🌊 Pravaha Multi-Threaded Parallel Execution

Include `"containers/tree/bplus_tree_pravaha.hpp"` to run multi-threaded range scans and aggregations over `BPlusTree`:

```cpp
#include "containers/tree/bplus_tree.hpp"
#include "containers/tree/bplus_tree_pravaha.hpp"

using namespace pebble::containers;
using namespace pebble::containers::pravaha;

BPlusMap<int, int> map;
// ... populate map ...

// 1. Parallel Range Scan
parallel_scan(map, 1000, 50000, [](int k, int v) {
    // Process item concurrently across available CPU threads
});

// 2. Parallel Map-Reduce Sum
long long sum = parallel_reduce(
    map, 1000, 50000, 0LL,
    [](long long a, long long b) { return a + b; },
    [](int k, int v) { return static_cast<long long>(v); }
);
```

---

## 🧠 Smriti Zero-Heap Arena Allocation

Integrate `BPlusTree` with `Smriti` bump memory pools:

```cpp
#include "containers/tree/bplus_tree.hpp"
#include "mem/smriti.hpp"

using namespace pebble::containers;

smriti::pools::BumpPool<smriti::domains::SystemRAMDomain> pool{1024 * 1024}; // 1MB pool
auto arena_map = make_smriti_bplus_map<int, std::string>(pool);

arena_map.insert_or_assign(1, "instant");
```

### $O(N)$ Bulk Loading

```cpp
std::vector<std::pair<const int, std::string>> sorted_data = {
    {1, "one"}, {2, "two"}, {3, "three"}, {4, "four"}
};

auto tree = BPlusTree<int, std::string>::from_sorted(sorted_data.begin(), sorted_data.end());
```

---

## ⏱️ Algorithmic Complexity

| Operation | Time Complexity | Notes |
|:---|:---:|:---|
| **Point Lookup (`find`, `contains`)** | $O(\log_B N)$ | SoA Cache-line optimized & SIMD-assisted |
| **Insertion (`insert_or_assign`)** | $O(\log_B N)$ | Freelist recycled; auto-splits nodes |
| **Deletion (`erase`)** | $O(\log_B N)$ | Automatic borrow/merge rebalancing |
| **Range Scan (`scan`)** | $O(\log_B N + K)$ | Linear pointer traversal with hardware prefetching |
| **Bulk Loading (`from_sorted`)** | $O(N)$ | Direct bottom-up construction |
