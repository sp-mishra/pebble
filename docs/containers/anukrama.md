# Anukrama (अनुक्रम) — Versioned-State & MVCC Substrate

**Anukrama** (`include/containers/anukrama/anukrama.hpp`) is Pebble's header-only, static-composition versioned-state substrate in modern C++23. It supplies immutable per-key version chains, stable point-in-time snapshots, and optimistic transaction validation with zero virtual dispatch, zero background thread overhead, and zero heap allocation on point reads.

---

## 1. Architectural Architecture & Version Chains

```
                           ANUKRAMA MVCC VERSION TOPOLOGY
                           
   Key Index (SkipList / Map)
   ┌─────────────┬────────────────────────────────────────────────────────┐
   │ "user_1001" │ Head ──► [V3: val=85, ts=12] ──► [V2: val=60, ts=8] ──► [V1: val=20, ts=2] ──► NULL
   ├─────────────┼────────────────────────────────────────────────────────┤
   │ "user_1002" │ Head ──► [V2: TOMB, ts=15]   ──► [V1: val=100, ts=5]  ──► NULL
   └─────────────┴────────────────────────────────────────────────────────┘
                               ▲                         ▲
                               │                         │
                        Snapshot @ ts=10          Snapshot @ ts=4
                        (Reads V2 = 60)           (Reads V1 = 20)
```

---

## 2. Core Mechanics & Algorithmic Guarantees

### 2.1 Snapshot Isolation & Monotonic Clocks
- **Snapshot Creation**: Grabs the current monotonic clock timestamp $T_{\text{snap}} = \text{clock.now()}$ in $O(1)$.
- **Read Path**: Point read `snapshot.get(key)` traverses the singly-linked version chain from head to find the newest node with timestamp $T_{\text{node}} \le T_{\text{snap}}$.
- **Zero Allocations**: Reads perform pure pointer traversals without allocating any memory or acquiring locks.

### 2.2 First-Writer-Wins Optimistic Validation
When a transaction commits:
1. Validates that no key in its write set has been updated at a timestamp $T_{\text{written}} > T_{\text{txn\_snapshot}}$.
2. If a conflict is detected, commit fails immediately with `anukrama::error::conflict`.
3. If valid, atomically publishes new immutable version nodes at commit timestamp $T_{\text{commit}} = \text{clock.next()}$.

---

## 3. End-to-End API Guide

### 3.1 Basic Snapshots & Version Visibility
```cpp
#include "containers/anukrama/anukrama.hpp"
#include <cassert>
#include <iostream>
#include <string>

int main() {
    anukrama::store<std::string, int> store;

    // 1. Transaction 1 writes "visits" = 100
    store.begin().put("visits", 100).commit();

    // 2. Capture Point-in-Time Snapshot
    auto snap1 = store.snapshot_at_current();

    // 3. Transaction 2 writes "visits" = 250
    store.begin().put("visits", 250).commit();

    // Snapshot 1 retains immutable historical state
    assert(snap1.get("visits").value() == 100);

    // Latest store state reflects Transaction 2
    auto snap2 = store.snapshot_at_current();
    assert(snap2.get("visits").value() == 250);

    std::cout << "Snap1: " << *snap1.get("visits") << " | Snap2: " << *snap2.get("visits") << "\n";
}
```

### 3.2 Optimistic Transaction Validation & Conflict Handling
```cpp
#include "containers/anukrama/anukrama.hpp"
#include <iostream>

void demonstrate_conflict() {
    anukrama::store<std::string, float> accounts;
    accounts.begin().put("acc_A", 1000.0f).commit();

    // Begin concurrent transactions T1 and T2 at same snapshot
    auto t1 = accounts.begin();
    auto t2 = accounts.begin();

    // T1 modifies acc_A and commits
    t1.put("acc_A", 900.0f);
    auto res1 = t1.commit();
    std::cout << "T1 Commit: " << (res1.has_value() ? "SUCCESS" : "FAILED") << "\n";

    // T2 tries to modify acc_A based on stale snapshot -> Conflict!
    t2.put("acc_A", 850.0f);
    auto res2 = t2.commit();
    if (!res2) {
        std::cout << "T2 Commit: CONFLICT DETECTED (First-writer-wins validated!)\n";
    }
}
```

### 3.3 Explicit History Pruning
```cpp
// Explicitly reclaim historical version nodes older than retention timestamp
uint64_t safe_min_active_ts = 100;
store.prune(safe_min_active_ts);
```
