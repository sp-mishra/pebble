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
// ============================================================================

#include "containers/lockfree/RingBuffer.hpp"
#include "glaze/glaze.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <semaphore>
#include <string>
#include <thread>

namespace petika {

// Default serialization policy: Glaze JSON.
struct GlazeJsonPolicy {
    template <typename T>
    static bool serialize(const T& data, std::string& out) {
        return !glz::write_json(data, out); // true = success
    }
};

template <
    typename RecordType,
    std::size_t QueueCapacity = 512,
    typename SerializationPolicy = GlazeJsonPolicy
>
class AsyncPersistenceWorker {
public:
    AsyncPersistenceWorker()
        : ring_buffer_{}
        , running_{true}
        , wake_signal_{0}
        , worker_thread_{[this]() { process_queue(); }}
    {}

    ~AsyncPersistenceWorker() { stop(); }

    AsyncPersistenceWorker(const AsyncPersistenceWorker&) = delete;
    AsyncPersistenceWorker& operator=(const AsyncPersistenceWorker&) = delete;

    // Non-blocking main-thread push (< 1 µs).
    // Takes RecordType by value — callers should std::move large payloads.
    bool enqueue(RecordType item, std::string target_filepath) {
        WorkItem work{std::move(item), std::move(target_filepath)};
        const bool pushed = ring_buffer_.try_push(std::move(work));
        if (pushed) wake_signal_.release();
        return pushed;
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

    void write_item(const WorkItem& item) {
        std::string serialized;
        if (!SerializationPolicy::serialize(item.data, serialized)) return;
        std::filesystem::path p(item.filepath);
        if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path());
        if (std::ofstream out{item.filepath, std::ios::out | std::ios::trunc | std::ios::binary}; out.is_open()) {
            out.write(serialized.data(), static_cast<std::streamsize>(serialized.size()));
        }
    }

    void process_queue() {
        while (running_.load(std::memory_order_relaxed)) {
            wake_signal_.acquire(); // blocks until enqueue() or stop() signals
            while (auto item = ring_buffer_.try_pop()) {
                write_item(*item);
            }
        }
        // Drain remaining items on shutdown.
        while (auto item = ring_buffer_.try_pop()) {
            write_item(*item);
        }
    }

    lockfree::RingBuffer<WorkItem, QueueCapacity> ring_buffer_;
    std::atomic<bool> running_;
    std::binary_semaphore wake_signal_;
    std::thread worker_thread_;
};

} // namespace petika
