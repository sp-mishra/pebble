# Lock-Free Containers

Header-only, C++23, zero-overhead lock-free primitives in `include/containers/lockfree/`.

---

## Container Selection Guide

| Use case                                      | Container                                     | Concurrency model                       |
|-----------------------------------------------|-----------------------------------------------|-----------------------------------------|
| Bounded FIFO, many readers + many writers     | `MPMCQueue<T, N>`                             | Multi-producer, multi-consumer          |
| Unbounded FIFO, many writers, one reader      | `MPSCQueue<T>`                                | Multi-producer, **single**-consumer     |
| Bounded FIFO, exactly one writer + one reader | `RingBuffer<T, N>`                            | Single-producer, single-consumer (SPSC) |
| Unbounded LIFO stack, any threads             | `AtomicStack<T>`                              | Multi-producer, multi-consumer          |
| Safe memory reclamation                       | `HazardRegistry<MaxThreads, RetireThreshold>` | Support library                         |

---

## RingBuffer\<T, N\>

Wait-free SPSC bounded FIFO. Fastest option when thread ownership is fixed.

```cpp
lockfree::RingBuffer<int, 1024> rb;

// Producer thread only
rb.try_push(42);          // returns bool (false if full)

// Consumer thread only
if (auto v = rb.try_pop()) { use(*v); }  // nullopt if empty
```

**Constraints**

- `N` must be a power of two ≥ 2 (enforced at compile time).
- `T` must be move-constructible; default-constructibility not required.
- `try_push` / `try_pop` are wait-free with no retries.

**Introspection**

- `empty()` / `size_approx()` — both counters read with `acquire`; conservative but correct from any thread. Not precise
  under concurrent access.
- `capacity()` — compile-time constant.

---

## MPMCQueue\<T, N\>

Bounded FIFO based on Dmitry Vyukov's sequence-counter design. Safe for any number of concurrent producers and
consumers.

```cpp
lockfree::MPMCQueue<Task, 512> q;

// Any thread
q.try_push(task);          // returns bool (false if full)
q.push(task);              // spin-waits until space available

// Any thread
if (auto v = q.try_pop()) { use(*v); }
auto item = q.pop();       // spin-waits until item available
```

**Constraints**

- `N` must be a power of two ≥ 2.
- `T` must be move-constructible.
- No dynamic allocation after construction.

**Exception safety**

- If `T`'s constructor throws during `try_push`, the slot sequence is restored so the queue remains fully operational.

**Performance notes**

- Each slot is cache-line aligned to eliminate false sharing.
- `size_approx()` returns 0 if a transient snapshot observes `head < tail` (possible near `SIZE_MAX` wrap); not
  monotonic.

---

## MPSCQueue\<T\>

Unbounded FIFO (Michael-Scott queue with sentinel). Any number of producers; exactly **one** consumer.

```cpp
lockfree::MPSCQueue<Task> q;   // throws std::bad_alloc if sentinel allocation fails

// Any thread — throws std::bad_alloc on allocation failure
q.push(task);

// Consumer thread ONLY
if (auto v = q.pop()) { use(*v); }

// Consumer thread ONLY — reliable emptiness check
q.consumer_empty();
```

**Thread ownership**

- `pop()` and `consumer_empty()` **must** be called from a single designated consumer thread. Calling from multiple
  threads is a data race.
- `empty()` is an alias for `consumer_empty()` retained for compatibility. Prefer `consumer_empty()` in new code.

**Memory reclamation**

- Nodes are heap-allocated and freed via `HazardRegistry`. Payload storage uses placement new; T's lifetime is managed
  explicitly, avoiding `std::optional` overhead.

---

## AtomicStack\<T\>

Treiber lock-free LIFO stack with ABA mitigation via 64-bit tagged pointer.

```cpp
lockfree::AtomicStack<int> s;

// Any thread — throws std::bad_alloc on allocation failure
s.push(42);

// Any thread
if (auto v = s.pop()) { use(*v); }  // nullopt if empty

s.empty();  // relaxed load — best-effort
```

**ABA mitigation**

- A 64-bit stamp is packed with the pointer in a 128-bit pair. The stamp is incremented on **every** `push` and `pop`,
  so a pointer recycled to the same address is always distinguishable by stamp.
- Requires a lock-free 128-bit atomic (`std::atomic<TaggedPtr>::is_always_lock_free`). On Apple Silicon: deployment
  target ≥ macOS 11, SDK ≥ 14. On x86_64: pass `-mcx16`.

**Memory reclamation**

- Popped nodes are retired through `HazardRegistry`. `pop()` publishes a hazard before dereferencing, then retires the
  node after the value is extracted.

---

## HazardRegistry\<MaxThreads, RetireThreshold\>

Hazard-pointer based safe memory reclamation. Used internally by `AtomicStack` and `MPSCQueue`.

```cpp
using HR = lockfree::HazardRegistry<128, 256>;

HR::HazardGuard guard;
Node *p = guard.protect(atomic_ptr, std::memory_order_acquire);
// p is safe to dereference even if another thread retires it

HR::retire(old_node);  // deferred deletion via plain delete
HR::retire(old_node, [](void *p) noexcept { delete static_cast<Node*>(p); }); // custom deleter
```

**Tuning**

- `MaxThreads` — maximum concurrent threads using the registry simultaneously. Default: 128. Increase if you hit the
  `assert` "slot pool exhausted".
- `RetireThreshold` — retire-list length that triggers a reclamation scan. Default: `2 * MaxThreads`. Lower values
  reclaim memory sooner at higher scan frequency; higher values batch more work per scan.

**Design**

- Each thread pre-claims a pool of `kPoolSize` (4) hazard slots on first use. `HazardGuard` construction and destruction
  are O(1) from the thread-local pool.
- `scan()` uses a stack-allocated `std::array<void*, MaxThreads>` — no heap allocation on the scan hot path.
- Retire list is thread-local (`thread_local vector<RetiredPtr>`). Scans reclaim only the calling thread's list;
  concurrent calls from different threads are safe.
- Thread exit: the thread-local `SlotPool` destructor releases all pre-claimed slots back to the global array, so the
  pool is never permanently exhausted by short-lived threads.

**Slot exhaustion**

- If all `MaxThreads` slots are claimed simultaneously, `acquire_slot()` returns `nullptr`. `HazardGuard` tolerates
  this — protection is skipped but the assert fires in debug builds. Increase `MaxThreads` to resolve.

**Convenience alias**

```cpp
using DefaultHazardRegistry = HazardRegistry<128, 256>;
```

Used as the default `HR` parameter in `AtomicStack` and `MPSCQueue`.

---

## Known Limitations

- No blocking API — all operations are try-based or spin-wait. For blocking semantics wrap with a semaphore or condition
  variable.
- `MPSCQueue` is unbounded; backpressure is the caller's responsibility.
- `AtomicStack` and `MPSCQueue` allocate per-element on the heap. For allocation-free operation use `MPMCQueue` or
  `RingBuffer`.
- `size_approx()` / `empty()` on `MPMCQueue` and `RingBuffer` are not linearisable under concurrent access — treat as
  hints only.
