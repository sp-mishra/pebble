#pragma once
// ============================================================================
// HazardRegistry — hazard-pointer based safe memory reclamation
// ============================================================================
//
// Provides safe reclamation for lock-free containers that return raw pointers
// (MPSCQueue, AtomicStack).  A thread that wishes to read a pointer first
// publishes it as a hazard; the retiring thread scans all hazard slots before
// calling delete.
//
// Design
// ------
//   - Static array of HazardSlot objects; each thread claims one slot.
//   - Thread-local cache: each thread remembers its slot pointer after the
//     first claim, eliminating the O(MaxThreads) linear scan on every
//     HazardGuard construction.
//   - On thread exit the thread-local destructor clears and releases the slot,
//     so the slot pool is never permanently exhausted by short-lived threads.
//   - HazardGuard is an RAII handle: protects a pointer for the duration of
//     the guard's lifetime, then clears the hazard on destruction.
//   - retire(ptr, deleter) queues a pointer for deferred deletion; when the
//     retire list exceeds a threshold it scans hazard slots and reclaims all
//     unprotected pointers.
//   - Thread-local retire lists mean retiring is wait-free.
//   - Slot exhaustion: if all slots are claimed, claim_slot() returns nullptr.
//     HazardGuard copes gracefully — protect() still runs but publishes nothing;
//     this is safe because a null-slot guard cannot protect any pointer, meaning
//     callers must not rely on protection when the pool is exhausted.  A debug
//     assertion fires in debug builds to surface the pool-full condition early.
//
// Limits
// ------
//   MaxThreads      — maximum concurrent threads using the registry.
//   RetireThreshold — number of deferred pointers before a scan is triggered.
//
// Usage
//   using HR = lockfree::HazardRegistry<128>;
//   HR::HazardGuard guard;
//   Node *p = guard.protect(atomic_ptr, std::memory_order_acquire);
//   // p is now safe to dereference even if another thread retires it
//   HR::retire(old_node, [](void *q) noexcept { delete static_cast<Node*>(q); });
// ============================================================================

#include <algorithm>
#include <atomic>
#include <array>
#include <cassert>
#include <cstddef>
#include <vector>

namespace lockfree {
    template <std::size_t MaxThreads = 128, std::size_t RetireThreshold = 2 * MaxThreads>
    class HazardRegistry {
    public:
        // ---- Hazard slot — one per thread -----------------------------------

        struct HazardSlot {
            std::atomic<void*> ptr{nullptr};
            std::atomic<bool> claimed{false};
        };

        // ---- RAII guard: protects a single pointer --------------------------

        class HazardGuard {
            HazardSlot* slot_{nullptr};

        public:
            HazardGuard() : slot_(HazardRegistry::acquire_slot()) {}

            ~HazardGuard() noexcept {
                if (slot_) {
                    slot_->ptr.store(nullptr, std::memory_order_release);
                    HazardRegistry::release_slot(slot_);
                    slot_ = nullptr;
                }
            }

            HazardGuard(const HazardGuard&) = delete;

            HazardGuard& operator=(const HazardGuard&) = delete;

            HazardGuard(HazardGuard&& o) noexcept : slot_{o.slot_} { o.slot_ = nullptr; }

            HazardGuard& operator=(HazardGuard&& o) noexcept {
                if (this != &o) {
                    if (slot_) {
                        clear();
                        HazardRegistry::release_slot(slot_);
                    }
                    slot_ = o.slot_;
                    o.slot_ = nullptr;
                }
                return *this;
            }

            // Load *src atomically, publish it as a hazard, then verify it hasn't
            // changed underneath us (classic double-check for safe pointer publish).
            template <typename T>
            T* protect(const std::atomic<T*>& src,
                       std::memory_order order = std::memory_order_acquire) noexcept {
                T* p = nullptr;
                T* q = src.load(std::memory_order_relaxed);
                do {
                    p = q;
                    if (slot_) slot_->ptr.store(p, std::memory_order_release);
                    q = src.load(order);
                }
                while (p != q);
                return p;
            }

            void clear() noexcept {
                if (slot_) slot_->ptr.store(nullptr, std::memory_order_release);
            }
        };

        // ---- Deferred deletion ----------------------------------------------

        struct RetiredPtr {
            void* ptr{nullptr};

            void (*deleter)(void*) noexcept{nullptr};
        };

        // Queue ptr for deferred deletion.  deleter must be noexcept.
        template <typename T>
        static void retire(T* ptr, void (*deleter)(void*) noexcept) noexcept {
            retire_list_().push_back(RetiredPtr{ptr, deleter});
            if (retire_list_().size() >= RetireThreshold) scan();
        }

        // Helper: retire a heap-allocated T* with plain delete.
        template <typename T>
        static void retire(T* ptr) noexcept {
            retire(ptr, [](void* p) noexcept { delete static_cast<T*>(p); });
        }

        // Reclaims only the calling thread's retire list. Thread-safe to call concurrently.
        static void scan() noexcept {
            std::array<void*, MaxThreads> hazards{};
            std::size_t hcount = 0;
            for (auto& slot : slots_()) {
                if (void* p = slot.ptr.load(std::memory_order_acquire); p != nullptr)
                    hazards[hcount++] = p;
            }

            auto& rl = retire_list_();
            auto it = std::remove_if(rl.begin(), rl.end(), [&](RetiredPtr& rp) {
                const bool prot = std::find(hazards.data(), hazards.data() + hcount, rp.ptr)
                    != hazards.data() + hcount;
                if (!prot) {
                    rp.deleter(rp.ptr);
                    return true;
                }
                return false;
            });
            rl.erase(it, rl.end());
            if (rl.empty()) rl.shrink_to_fit();
        }

    private:
        static std::array<HazardSlot, MaxThreads>& slots_() noexcept {
            static std::array<HazardSlot, MaxThreads> s{};
            return s;
        }

        static std::vector<RetiredPtr>& retire_list_() noexcept {
            thread_local std::vector<RetiredPtr> list;
            return list;
        }

        // ---- Thread-local slot cache ----------------------------------------
        //
        // Each thread pre-claims a small pool of slots the first time it enters
        // the registry (via a thread_local SlotPool).  HazardGuard construction
        // takes from the pool in O(1); destruction returns to the pool in O(1).
        // On thread exit the SlotPool destructor releases all pre-claimed slots.
        //
        // Pool size = kPoolSize (enough for nested guards on one thread).

        static constexpr std::size_t kPoolSize = 4;

        struct SlotPool {
            std::array<HazardSlot*, kPoolSize> slots{};
            std::size_t size = 0; // number of cached (free) slots

            SlotPool() noexcept { refill(); }

            ~SlotPool() noexcept {
                // Release all cached (unused) slots back to the global array.
                for (std::size_t i = 0; i < size; ++i) {
                    if (slots[i]) {
                        slots[i]->ptr.store(nullptr, std::memory_order_release);
                        slots[i]->claimed.store(false, std::memory_order_release);
                        slots[i] = nullptr;
                    }
                }
                size = 0;
            }

            HazardSlot* take() noexcept {
                if (size > 0) return slots[--size];
                // Pool empty — do a slow scan.
                return scan_global();
            }

            void put(HazardSlot* s) noexcept {
                if (!s) return;
                if (size < kPoolSize) {
                    // Park it back into the pool (still claimed, ptr cleared).
                    s->ptr.store(nullptr, std::memory_order_release);
                    slots[size++] = s;
                }
                else {
                    // Pool full — release the slot entirely.
                    s->ptr.store(nullptr, std::memory_order_release);
                    s->claimed.store(false, std::memory_order_release);
                }
            }

        private:
            void refill() noexcept {
                for (std::size_t i = 0; i < kPoolSize; ++i) {
                    HazardSlot* s = scan_global();
                    if (!s) break;
                    s->ptr.store(nullptr, std::memory_order_release);
                    slots[size++] = s;
                }
            }

            static HazardSlot* scan_global() noexcept {
                for (auto& slot : HazardRegistry::slots_()) {
                    if (bool expected = false; slot.claimed.compare_exchange_strong(expected, true,
                        std::memory_order_acq_rel,
                        std::memory_order_relaxed))
                        return &slot;
                }
                assert(false && "HazardRegistry: slot pool exhausted — increase MaxThreads");
                return nullptr;
            }
        };

        static SlotPool& thread_pool() noexcept {
            thread_local SlotPool pool;
            return pool;
        }

        static HazardSlot* acquire_slot() noexcept {
            return thread_pool().take();
        }

        static void release_slot(HazardSlot* s) noexcept {
            thread_pool().put(s);
        }
    };

    // Convenience alias for typical use (128 threads, retire threshold 256).
    using DefaultHazardRegistry = HazardRegistry<>;
} // namespace lockfree

