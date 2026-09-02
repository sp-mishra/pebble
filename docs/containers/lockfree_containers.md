# Lock-Free Containers (`include/containers/lockfree/`)

Pebble's lock-free container library provides header-only, zero-overhead, multi-core concurrent primitives in modern
C++23. It features wait-free single-producer single-consumer ring buffers, Dmitry Vyukov bounded multi-producer
multi-consumer queues, unbounded Michael-Scott MPSC queues, Treiber atomic stacks with 128-bit ABA mitigation, and
Hazard Pointer safe memory reclamation (`HazardRegistry`).

---

## 1. Concurrency Model & Selection Matrix

| Container              |           Concurrency Model            |       Complexity (Push / Pop)       |     Dynamic Allocations      |        ABA Protection        | Best Use Case                         |
|:-----------------------|:--------------------------------------:|:-----------------------------------:|:----------------------------:|:----------------------------:|:--------------------------------------|
| **`RingBuffer<T, N>`** | Single-Producer Single-Consumer (SPSC) | Wait-Free $O(1)$ / Wait-Free $O(1)$ |     None (Stack / Fixed)     | N/A (Single thread per side) | Thread-to-thread streaming, telemetry |
| **`MPMCQueue<T, N>`**  |  Multi-Producer Multi-Consumer (MPMC)  | Lock-Free $O(1)$ / Lock-Free $O(1)$ | None (Fixed circular buffer) |  Sequence counter per slot   | Task graph job stealing, thread pools |
| **`MPSCQueue<T>`**     | Multi-Producer Single-Consumer (MPSC)  | Lock-Free $O(1)$ / Wait-Free $O(1)$ |           Per-node           |       Hazard Pointers        | Logging pipelines, command queues     |
| **`AtomicStack<T>`**   |  Multi-Producer Multi-Consumer (LIFO)  | Lock-Free $O(1)$ / Lock-Free $O(1)$ |           Per-node           |   128-bit Tagged Pointers    | Object pooling, free-lists            |

---

## 2. Algorithmic Mechanics & Invariants

### 2.1 Dmitry Vyukov's MPMC Sequence Counter Invariant

`MPMCQueue<T, N>` avoids false sharing and lock contention by equipping each slot with an atomic sequence number
`std::atomic<size_t> sequence`:

- A slot at index `i` is ready for writing by turn `pos` when:
  $$\text{slot.sequence.load (acquire)} == \text{pos}$$
- On successful CAS of the queue's `enqueue_pos`, the producer writes the payload and sets:
  $$\text{slot.sequence.store (pos + 1, release)}$$
- A consumer can read the slot when:
  $$\text{slot.sequence.load (acquire)} == \text{pos} + 1$$
- After reading, the consumer resets:
  $$\text{slot.sequence.store (pos + \text{Capacity}, release)}$$

```
                   VYUKOV MPMC QUEUE MEMORY TOPOLOGY (CACHE-ALIGNED)
                   
   ┌────────────────────────┬────────────────────────┬────────────────────────┐
   │ Slot 0 (64-byte align) │ Slot 1 (64-byte align) │ Slot 2 (64-byte align) │
   │ ├─ atomic<size_t> seq  │ ├─ atomic<size_t> seq  │ ├─ atomic<size_t> seq  │
   │ └─ Storage T payload   │ └─ Storage T payload   │ └─ Storage T payload   │
   └────────────────────────┴────────────────────────┴────────────────────────┘
```

### 2.2 Hazard Pointer Safe Memory Reclamation (`HazardRegistry`)

When popping from `AtomicStack` or `MPSCQueue`:

1. The consumer acquires a hazard slot from its thread-local pool: `guard.protect(atomic_ptr)`.
2. The pointer is guaranteed not to be deleted by other threads while protected in the global hazard registry.
3. When a node is popped, it is placed on the thread-local retire list.
4. When the retire list exceeds `RetireThreshold`, `scan()` reclaims all retired nodes not currently protected by any
   active thread's hazard pointers.

---

## 3. End-to-End API Guide

### 3.1 Vyukov MPMC Bounded Task Queue

```cpp
#include "containers/lockfree/MPMCQueue.hpp"
#include <iostream>
#include <thread>
#include <vector>

struct Job {
    int id;
    int payload;
};

int main() {
    // 512 capacity power-of-two bounded queue
    containers::lockfree::MPMCQueue<Job, 512> queue;

    std::vector<std::jthread> workers;

    // 4 Producer Threads
    for (int p = 0; p < 4; ++p) {
        workers.emplace_back([&queue, p]() {
            for (int i = 0; i < 1000; ++i) {
                queue.push(Job{.id = p * 1000 + i, .payload = i * 2});
            }
        });
    }

    // 4 Consumer Threads
    for (int c = 0; c < 4; ++c) {
        workers.emplace_back([&queue]() {
            for (int i = 0; i < 1000; ++i) {
                Job job = queue.pop();
                // Process job concurrently
            }
        });
    }

    // std::jthread automatically joins on scope exit
}
```

### 3.2 Wait-Free SPSC RingBuffer Telemetry Pipeline

```cpp
#include "containers/lockfree/RingBuffer.hpp"
#include <iostream>
#include <thread>

int main() {
    containers::lockfree::RingBuffer<float, 1024> telemetry_stream;

    std::jthread producer([&telemetry_stream](std::stop_token st) {
        float temp = 300.0f;
        while (!st.stop_requested()) {
            telemetry_stream.try_push(temp += 0.5f);
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    });

    std::jthread consumer([&telemetry_stream](std::stop_token st) {
        while (!st.stop_requested()) {
            if (auto val = telemetry_stream.try_pop()) {
                std::cout << "Received telemetry sample: " << *val << " K\n";
            }
        }
    });
}
```
