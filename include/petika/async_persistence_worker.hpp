#pragma once
// ============================================================================
// petika/async_persistence_worker.hpp — Lock-Free SPSC Persistence Worker
// ============================================================================
// Zero-latency background serialization worker using SPSC lock-free ring buffer.
// Decouples hot simulation/commit loop from Glaze JSON/binary disk I/O.
//
// Policies:
//   SerializationPolicy — swap Glaze JSON for binary/CBOR/custom formats.
//     Default: GlazeJsonPolicy (existing behaviour).
//   OverflowPolicy — what enqueue() does when the ring is full.
//     Default: Reject (returns false, as before). See § OverflowPolicy below.
//
// Observability:
//   set_error_callback(cb) — invoked (on the producer thread for overflow, on
//     the worker thread for serialize/write failures) so silent data loss is
//     surfaced rather than dropped on the floor. Optional; unset = legacy
//     silent behaviour.
// ============================================================================

#include "containers/lockfree/RingBuffer.hpp"
#include "glaze/glaze.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <semaphore>
#include <thread>

namespace petika {
    // Default serialization policy: Glaze JSON.
    struct GlazeJsonPolicy {
        template <typename T>
        static bool serialize(const T& data, std::string& out) {
            return !glz::write_json(data, out); // true = success
        }
    };

    // ----------------------------------------------------------------------------
    // § OverflowPolicy — behaviour when the ring is full at enqueue() time.
    // ----------------------------------------------------------------------------
    //   Reject     (default) — drop the incoming item, return false. Zero overhead;
    //                          identical to the pre-policy behaviour.
    //   Block                — spin-wait (with a yield) until a slot frees, then
    //                          push. Never drops; applies producer-side back-pressure.
    //                          Producer-only, so the SPSC contract is preserved.
    //   DropOldest           — evict the oldest queued item to make room for the new
    //                          one, return true. This is the ONLY policy where the
    //                          producer must pop, which would otherwise violate the
    //                          ring's single-consumer contract — so eviction and the
    //                          worker's own pop are serialised under `drop_mutex_`.
    //                          The mutex is engaged ONLY for DropOldest (via
    //                          `if constexpr`); Reject/Block keep the fully
    //                          lock-free consumer path.
    enum class OverflowPolicy { Reject, Block, DropOldest };

    // Reason passed to the error callback so callers can distinguish loss modes.
    enum class WorkerError { OverflowDropped, SerializeFailed, WriteFailed };

    template <
        typename RecordType,
        std::size_t QueueCapacity = 512,
        typename SerializationPolicy = GlazeJsonPolicy,
        OverflowPolicy Overflow = OverflowPolicy::Reject
    >
    class AsyncPersistenceWorker {
    public:
        // Called on drop/failure with the reason and the target path (best effort).
        // Invoked on the producer thread for OverflowDropped, on the worker thread
        // for SerializeFailed / WriteFailed. Keep it cheap and thread-safe.
        using ErrorCallback = std::function<void(WorkerError, std::string_view)>;

        AsyncPersistenceWorker()
            : ring_buffer_{}
              , running_{true}
              , wake_signal_{0}
              , worker_thread_{[this]() { process_queue(); }} {}

        ~AsyncPersistenceWorker() { stop(); }

        AsyncPersistenceWorker(const AsyncPersistenceWorker&) = delete;
        AsyncPersistenceWorker& operator=(const AsyncPersistenceWorker&) = delete;

        // Set before enqueueing (or ensure external synchronisation): the callback
        // is read on both producer and worker threads.
        void set_error_callback(ErrorCallback cb) { error_callback_ = std::move(cb); }

        // Non-blocking main-thread push (< 1 µs) under Reject.
        // Takes RecordType by value — callers should std::move large payloads.
        // Returns true if the item was accepted (or made room for), false only when
        // Reject drops it.
        bool enqueue(RecordType item, std::string target_filepath) {
            WorkItem work{std::move(item), std::move(target_filepath)};

            if constexpr (Overflow == OverflowPolicy::Reject) {
                const bool pushed = ring_buffer_.try_push(std::move(work));
                if (pushed) wake_signal_.release();
                else report(WorkerError::OverflowDropped, work.filepath);
                return pushed;
            }
            else if constexpr (Overflow == OverflowPolicy::Block) {
                // Producer-side back-pressure: yield-spin until a slot frees. Only
                // the producer touches head_, so the SPSC contract holds.
                while (!ring_buffer_.try_push(std::move(work))) {
                    if (!running_.load(std::memory_order_relaxed)) return false;
                    std::this_thread::yield();
                }
                wake_signal_.release();
                return true;
            }
            else { // DropOldest
                // Serialise the producer-side eviction against the worker's pop so
                // two threads never drive tail_ concurrently. DropOldest guarantees
                // acceptance: a failed try_push means the ring is full, so we evict
                // the oldest to free a slot and the next try_push must succeed. The
                // loop therefore always makes progress and needs no running_ guard
                // (unlike Block, which would otherwise spin forever at shutdown).
                while (!ring_buffer_.try_push(std::move(work))) {
                    std::scoped_lock lock{drop_mutex_};
                    if (auto evicted = ring_buffer_.try_pop())
                        report(WorkerError::OverflowDropped, evicted->filepath);
                    // If try_pop also saw it empty (worker drained concurrently),
                    // the next try_push finds a slot regardless.
                }
                wake_signal_.release();
                return true;
            }
        }

        void stop() {
            if (running_.exchange(false)) {
                wake_signal_.release(); // unblock worker if idle
                if (worker_thread_.joinable()) worker_thread_.join();
            }
        }

    private:
        struct WorkItem {
            RecordType data{};
            std::string filepath{};
        };

        void report(WorkerError err, std::string_view path) {
            if (error_callback_) error_callback_(err, path);
        }

        // Pop honouring the DropOldest serialisation: only DropOldest pays the lock.
        std::optional<WorkItem> pop_one() {
            if constexpr (Overflow == OverflowPolicy::DropOldest) {
                std::scoped_lock lock{drop_mutex_};
                return ring_buffer_.try_pop();
            }
            else {
                return ring_buffer_.try_pop();
            }
        }

        void write_item(const WorkItem& item) {
            std::string serialized;
            if (!SerializationPolicy::serialize(item.data, serialized)) {
                report(WorkerError::SerializeFailed, item.filepath);
                return;
            }
            std::filesystem::path p(item.filepath);
            if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path());
            std::ofstream out{item.filepath, std::ios::out | std::ios::trunc | std::ios::binary};
            if (!out.is_open()) {
                report(WorkerError::WriteFailed, item.filepath);
                return;
            }
            out.write(serialized.data(), static_cast<std::streamsize>(serialized.size()));
            if (!out) report(WorkerError::WriteFailed, item.filepath);
        }

        void process_queue() {
            while (running_.load(std::memory_order_relaxed)) {
                wake_signal_.acquire(); // blocks until enqueue() or stop() signals
                while (auto item = pop_one()) {
                    write_item(*item);
                }
            }
            // Drain remaining items on shutdown.
            while (auto item = pop_one()) {
                write_item(*item);
            }
        }

        lockfree::RingBuffer<WorkItem, QueueCapacity> ring_buffer_;
        std::atomic<bool> running_;
        std::binary_semaphore wake_signal_;
        ErrorCallback error_callback_{};
        // Engaged only under DropOldest (see enqueue()/pop_one()); unused otherwise.
        std::mutex drop_mutex_;
        std::thread worker_thread_;
    };
} // namespace petika
