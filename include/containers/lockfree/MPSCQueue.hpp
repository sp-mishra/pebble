#pragma once
// ============================================================================
// MPSCQueue<T, HR> — Multi-Producer / Single-Consumer lock-free queue
// ============================================================================
//
// An unbounded FIFO for arbitrarily many producer threads and exactly one
// consumer thread.  Based on the Michael-Scott queue with a dummy sentinel
// head node; producers CAS the tail, consumer reads from head->next.
//
// Safe memory reclamation
// -----------------------
//   Retired nodes go through HazardRegistry so that a producer that has loaded
//   the old tail but hasn't yet written its next pointer cannot race with the
//   consumer freeing that node.  Pass your HazardRegistry type as the second
//   template parameter (defaults to DefaultHazardRegistry).
//
// Ownership
// ---------
//   Nodes are heap-allocated; the queue owns them.  T is stored via placement
//   new inside each data node; the sentinel node carries no T value.
//
// Thread model
// ------------
//   push() — safe to call from any number of threads concurrently.
//   pop()  — must be called from exactly ONE consumer thread.
//   consumer_empty() — reliable only from the consumer thread.
//
// Usage
//   lockfree::MPSCQueue<MyTask> q;     // throws std::bad_alloc if sentinel fails
//   q.push(task);                      // any thread
//   if (auto v = q.pop()) { use(*v); } // single consumer thread only
// ============================================================================

#include <atomic>
#include <concepts>
#include <new>
#include <optional>
#include <utility>

#include "containers/lockfree/HazardRegistry.hpp"

namespace lockfree {
    template <typename T, typename HR = DefaultHazardRegistry>
        requires std::move_constructible<T>
    class MPSCQueue {
        // NodeBase — linked-list backbone; holds a deleter for correct polymorphic deletion.
        struct NodeBase {
            std::atomic<NodeBase*> next{nullptr};
            void (*deleter)(NodeBase*) noexcept{nullptr};
        };

        // DataNode — carries the actual payload via placement new.
        // T lifetime is manually managed: constructed in ctor, explicitly destroyed in pop().
        struct DataNode : NodeBase {
            alignas(alignof(T)) unsigned char storage[sizeof(T)];

            template <typename U>
            explicit DataNode(U&& v) {
                this->deleter = [](NodeBase* p) noexcept { delete static_cast<DataNode*>(p); };
                ::new(storage) T(std::forward<U>(v));
            }

            T& value() noexcept { return *std::launder(reinterpret_cast<T*>(storage)); }
        };

        // head_ — consumer-only (no write contention); sits on its own cache line.
        alignas(64) std::atomic<NodeBase*> head_;
        // tail_ — hot for producers; separate cache line.
        alignas(64) std::atomic<NodeBase*> tail_;

    public:
        // throws std::bad_alloc if sentinel allocation fails
        MPSCQueue() {
            auto* sentinel = new NodeBase{};
            sentinel->deleter = [](NodeBase* p) noexcept { delete p; };
            head_.store(sentinel, std::memory_order_relaxed);
            tail_.store(sentinel, std::memory_order_relaxed);
        }

        ~MPSCQueue() noexcept {
            // Drain remaining items.
            while (pop()) {}
            // Delete the surviving sentinel using its own deleter.
            NodeBase* s = head_.load(std::memory_order_relaxed);
            s->deleter(s);
        }

        MPSCQueue(const MPSCQueue&) = delete;

        MPSCQueue& operator=(const MPSCQueue&) = delete;

        // Push — safe to call from any thread concurrently. Throws std::bad_alloc.
        template <typename U>
            requires std::constructible_from<T, U>
        void push(U&& value) {
            DataNode* node = new DataNode(std::forward<U>(value));
            // Swing tail to the new node; the previous tail's next points to it.
            NodeBase* prev = tail_.exchange(node, std::memory_order_acq_rel);
            prev->next.store(node, std::memory_order_release);
        }

        // Pop — must be called from exactly one consumer thread.
        // Returns std::nullopt if the queue is empty.
        // Single-consumer: no hazard guard needed — only this thread frees nodes,
        // and producers never free nodes, so the sentinel cannot be freed while
        // we hold its pointer.
        [[nodiscard]] std::optional<T> pop() noexcept {
            NodeBase* head = head_.load(std::memory_order_acquire);
            NodeBase* next = head->next.load(std::memory_order_acquire);
            if (next == nullptr) return std::nullopt;

            // Advance the head; next becomes the new sentinel.
            head_.store(next, std::memory_order_release);

            // Move value out of the data node, then explicitly destroy T in place.
            auto* data = static_cast<DataNode*>(next);
            T result = std::move(data->value());
            data->value().~T();

            // Retire the old sentinel using its stored deleter.
            auto* old_head = head;
            HR::retire(old_head, [](void* p) noexcept {
                auto* n = static_cast<NodeBase*>(p);
                n->deleter(n);
            });
            return result;
        }

        // Reliable only from the consumer thread — producers may observe stale state.
        [[nodiscard]] bool consumer_empty() const noexcept {
            const NodeBase* head = head_.load(std::memory_order_acquire);
            return head->next.load(std::memory_order_acquire) == nullptr;
        }

        // Note: reliable only from the consumer thread — producers may observe stale state.
        // Prefer consumer_empty() in new code.
        [[nodiscard]] bool empty() const noexcept { return consumer_empty(); }
    };
} // namespace lockfree
