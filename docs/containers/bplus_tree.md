# B+ Tree (`pebble::containers::BPlusTree`)

**`pebble::containers::BPlusTree`** (`include/containers/tree/bplus_tree.hpp`) is Pebble's high-performance,
policy-based, zero-overhead **B+ Tree** container implemented in modern C++23/C++26. It features Structure-of-Arrays
(SoA) node layouts, SIMD-accelerated key searches (Google Highway), intrusive freelist node recycling, transparent
string-view lookups, and $O (N)$ bottom-up bulk loading.

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

When `Traits::EnableSIMD` is set (default), Highway is available (`PEBBLE_HAS_HIGHWAY`), the key type is vectorisable
(`u32/i32/float/u64/i64`), and the comparator is the default `std::less<Key>`, the **exact-key membership probe**
(`find` / `contains` / erase-hit test) uses a branchless SIMD linear scan over the 64-byte-packed leaf — optimal for the
small nodes a B+ tree uses:

```cpp
// simd::linear_search_simd — vectorised equality scan over a leaf's contiguous keys
const auto data       = hn::LoadU(d, keys + i);
const auto target_vec = hn::Set(d, target);
const auto mask       = hn::Eq(data, target_vec);
if (!hn::AllFalse(d, mask)) { /* resolve exact index */ }
```

**Ordered navigation stays scalar binary search.** Inner-node routing, `lower_bound`, `upper_bound`, splits, and any
heterogeneous/transparent or custom-`Compare` lookup remain `Compare`-based — SIMD compares with `==`, which only
matches `std::less<Key>` semantics. String keys and custom comparators transparently use the scalar path.

### 2.2 Intrusive Freelist Node Recycling

To eliminate OS memory allocator thrashing during rapid insertion and deletion churn, `BPlusTree` retains an intrusive
singly-linked freelist of up to `Traits::MaxRecycleNodes` deallocated nodes. Reallocated nodes reuse warm L1/L2 cache
blocks immediately.

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

### 3.2 $O (N)$ Bottom-Up Bulk Loading

`from_sorted` builds the tree bottom-up in genuine $O (N)$: sorted entries are packed into leaves left-to-right (to a
fill factor that leaves split headroom), the leaf chain is linked, and inner levels are assembled from the layer below
until a single root remains — perfectly balanced and cache-dense, with no per-insert re-descent. **Precondition**: the
input range is sorted by key and unique (implied by the name).

```cpp
#include "containers/tree/bplus_tree.hpp"
#include <vector>

std::vector<std::pair<const int, std::string>> sorted_entries = {
    {1, "first"}, {2, "second"}, {3, "third"}, {4, "fourth"}
};

// Constructs a perfectly balanced B+ Tree in O(N) time
auto tree = pebble::containers::BPlusTree<int, std::string>::from_sorted(
    sorted_entries.begin(), sorted_entries.end()
);
```

### 3.3 Pravaha Parallel Map-Reduce over B+ Tree

The `pravaha` helpers (`parallel_scan` / `parallel_reduce` / `parallel_find`) run on
`::pravaha::Runner<::pravaha::JThreadBackend>` — the same task-runner seam the Petika adapter uses. Work is partitioned
over the leaf chain (or query batch), each partition submitted as a Pravaha task, and the runner backend drained before
results are read (no reliance on thread-destructor timing). An optional trailing `Runner*` lets callers share a runner;
omit it to use a per-call local runner. **Thread-safety**: user callables run concurrently across partitions and must be
thread-safe;
`reduce_op` must be associative (it also runs in a final serial fold).

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

---

## 4. Traits & Policy Reference

| Template parameter | Default                                       | Purpose                                                            |
|--------------------|-----------------------------------------------|--------------------------------------------------------------------|
| `Key`              | —                                             | Key type. Numeric keys unlock the SIMD membership probe.           |
| `Value`            | —                                             | Mapped type (`std::monostate` for `BPlusSet`).                     |
| `Compare`          | `std::less<Key>`                              | Ordering. Use `std::less<>` for transparent/heterogeneous lookups. |
| `Traits`           | `DefaultBPlusTreeTraits<Key, Value>`          | Policy bundle (below).                                             |
| `Allocator`        | `std::allocator<std::pair<const Key, Value>>` | Node allocator; rebind-based. Swap for the Smriti seam.            |

`DefaultBPlusTreeTraits<Key, Value, TargetNodeBytes = 256>` fields (must satisfy the
`BPlusTreeTraits` concept):

| Trait field       | Default                               | Purpose                                                                         |
|-------------------|---------------------------------------|---------------------------------------------------------------------------------|
| `TargetNodeBytes` | `256`                                 | Byte budget the fanout auto-sizes toward.                                       |
| `LeafCapacity`    | auto from `sizeof(Key)+sizeof(Value)` | Keys per leaf. Auto-derived and clamped to `[4, 4096]`.                         |
| `InnerCapacity`   | auto from `sizeof(Key)+sizeof(void*)` | Router keys per inner node, clamped to `[4, 4096]`.                             |
| `EnableSIMD`      | `true`                                | Engage the Highway membership probe for vectorisable keys.                      |
| `MaxRecycleNodes` | `64`                                  | Freelist cap: recycled nodes retained before returning memory to the allocator. |

Fanout auto-tunes to key/value size via the `TargetNodeBytes` budget; provide a custom `Traits` to override any field
(e.g. fixed capacities, disable SIMD, resize the recycle pool). A malformed
`Traits` is rejected at class scope by `static_assert(BPlusTreeTraits<Traits>)` with a readable message.

## 5. Smriti Arena Allocator Seam

Node memory can be sourced from a Smriti resource via the allocator parameter:

```cpp
using PoolType = smriti::pools::BumpPool<smriti::domains::SystemRAMDomain>;
PoolType pool{65536};
auto map = pebble::containers::make_smriti_bplus_map<int, int>(pool);
```

`SmritiBPlusMap` / `SmritiBPlusSet` type aliases and the `make_smriti_bplus_map` /
`make_smriti_bplus_set` factories wire `smriti::SmritiAllocator` in. Combined with the
`MaxRecycleNodes` freelist, node churn under insert/erase storms is bounded to warm arena blocks.

## 6. Production-Scale Usage

This section shows the container running at production scale — millions of live entries, a bounded-memory arena, bulk
import/recovery in $O (N)$, and parallel read fan-out — followed by the seam for adopting it as a Petika storage tier.

### 6.1 Capacity & Depth Budget

Fanout auto-sizes from `TargetNodeBytes` (default `256`, clamped to `[4, 4096]` keys/node). For a
`BPlusMap<std::uint64_t, std::uint64_t>` (16 B/entry) the leaf holds ~14 entries and inner nodes
~15 routers at the default budget; raising `TargetNodeBytes` widens both. Tree height is
$\lceil \log_{B} N \rceil$, so depth stays shallow even at scale:

| Entries $N$ | Fanout $B$ (256 B budget) | Height | Fanout $B$ (4096 B budget) | Height |
|-------------|---------------------------|--------|----------------------------|--------|
| $10^6$      | ~15                       | 5      | ~250                       | 3      |
| $10^7$      | ~15                       | 6      | ~250                       | 3      |
| $10^8$      | ~15                       | 7      | ~250                       | 4      |

Widen `TargetNodeBytes` for read-heavy, mostly-static datasets (fewer levels, fewer cache misses per lookup); keep it
narrow for write-churn workloads (cheaper splits/merges). Override via a custom
`Traits` when the auto-tuned band is wrong for your access pattern:

```cpp
struct WideTraits : pebble::containers::DefaultBPlusTreeTraits<std::uint64_t, std::uint64_t, 4096> {};
using WideMap = pebble::containers::BPlusTree<
    std::uint64_t, std::uint64_t, std::less<>, WideTraits>;
```

### 6.2 Bulk Import & Recovery in $O (N)$

For initial load or crash recovery from a sorted snapshot, **never** replay through
`insert_or_assign` (that is $O (N \log N)$ with per-insert re-descent and split churn). Feed the sorted, unique stream
to `from_sorted`, which packs leaves left-to-right and assembles inner levels bottom-up — one linear pass, perfectly
balanced, cache-dense:

```cpp
#include "containers/tree/bplus_tree.hpp"
#include <vector>

// Sorted, unique snapshot restored from WAL / cold storage (millions of rows).
std::vector<std::pair<const std::uint64_t, Record>> snapshot = load_sorted_snapshot();

auto index = pebble::containers::BPlusMap<std::uint64_t, Record>::from_sorted(
    snapshot.begin(), snapshot.end());   // O(N), single balanced build
```

**Precondition**: the range is sorted by key and unique. For unsorted input, sort first (`std::sort`) — a
sort-then-bulk-load is still cheaper than $N$ ordered inserts at scale.

### 6.3 Bounded Memory Under Sustained Churn

At production scale the OS allocator becomes the bottleneck under insert/erase storms. Source nodes from a Smriti arena
and let the `MaxRecycleNodes` freelist absorb the churn so steady-state allocation approaches zero:

```cpp
using PoolType = smriti::pools::BumpPool<smriti::domains::SystemRAMDomain>;
PoolType pool{1u << 26};   // 64 MiB arena, sized to peak live-node count

// Custom Traits: deeper freelist for high-churn tiers.
struct ChurnTraits : pebble::containers::DefaultBPlusTreeTraits<std::uint64_t, Record> {
    static constexpr std::size_t MaxRecycleNodes = 1024;
};

auto hot_index = pebble::containers::make_smriti_bplus_map<std::uint64_t, Record>(pool);
```

Size the arena to the *peak* live-node count ($\approx N / (\text{leaf fill} \times 0.7)$ leaves plus inner nodes); the
freelist then recycles warm blocks across delete/insert waves rather than returning them to the arena.

### 6.4 Parallel Read Fan-Out

For analytics-style aggregation or large batch point-lookups over a *quiescent* (read-only) tree, the pravaha helpers
partition the leaf chain / query batch across a shared runner:

```cpp
#include "containers/tree/bplus_tree_pravaha.hpp"

namespace pv = pebble::containers::pravaha;
pv::DefaultRunner runner;   // reuse one runner across calls to amortise thread setup

// Aggregate a key range in parallel (reduce_op must be associative).
long long total = pv::parallel_reduce(
    hot_index, lo, hi, 0LL,
    [](long long a, long long b) { return a + b; },
    [](std::uint64_t, const Record& r) { return r.weight; },
    /*leaf_grain_size=*/64, &runner);

// Batch point-lookups (results align with queries by index).
auto hits = pv::parallel_find(hot_index, std::span<const std::uint64_t>(query_keys), &runner);
```

**Thread-safety**: the helpers only *read* the tree — do not mutate it concurrently. User callables run on multiple
threads and must be thread-safe; `reduce_op` must be associative. Tune
`leaf_grain_size` up (fewer, larger tasks) for cheap callables and down for expensive ones.

### 6.5 Adopting the B+ Tree as a Petika Storage Tier

The B+ tree is a *container*, not a Petika `StorageEngine` — its surface is
`insert_or_assign` / `at` / `contains` / `erase` / `scan`, whereas `petika::StorageEngine` requires
`put` / `get` / `erase` / `contains` / `size` / `empty` / `clear` / `apply_log_record`, each returning
`petika::Result<T>` and taking a `nitya::lsn_t`. To wire it in as a storage tier, add a thin adapter that owns a
`BPlusMap` and satisfies the concept:

```cpp
// Sketch — a Petika engine backed by BPlusTree (adapter, not part of the container).
template <typename Key, typename Value>
class BTreeEngine {
    pebble::containers::BPlusMap<Key, Value> index_;
public:
    petika::Result<void> put(const Key& k, const Value& v, nitya::lsn_t) {
        index_.insert_or_assign(k, v);
        return {};
    }
    petika::Result<Value> get(const Key& k) const {
        auto it = index_.find(k);
        if (it == index_.end()) return std::unexpected(petika::StorageError::NotFound);
        return it->second;
    }
    petika::Result<void> erase(const Key& k, nitya::lsn_t) {
        return index_.erase(k) ? petika::Result<void>{}
                               : std::unexpected(petika::StorageError::NotFound);
    }
    bool contains(const Key& k) const { return index_.contains(k); }
    std::size_t size() const noexcept { return index_.size(); }
    bool empty() const noexcept { return index_.empty(); }
    // ... clear(lsn), apply_log_record(op, k, v, lsn) → replay by EntryOp ...
    // Recovery fast-path: rebuild from a sorted checkpoint via BPlusMap::from_sorted (§6.2).
};
```

The recovery replay path should prefer `from_sorted` over per-record `put` (§6.2), and the engine can pass a Smriti
arena into the `BPlusMap` (§6.3) so a `BTreeStore = Petika<BTreeEngine<...>, ...>`
tier inherits bounded-memory node recycling. See `scratch/petika_next.md` item 8 for the full integration plan — the
adapter itself is a code addition, not yet present in `include/petika/`.

