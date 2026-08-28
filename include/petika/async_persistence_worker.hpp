#pragma once
// ============================================================================
// petika/async_persistence_worker.hpp — Lock-Free SPSC Persistence Worker
// ============================================================================
// Zero-latency background serialization worker using SPSC lock-free ring buffer.
// Decouples 60 FPS main simulation loop from Glaze JSON/Petika disk I/O.
// ============================================================================

#include "containers/lockfree/RingBuffer.hpp"
#include "glaze/glaze.hpp"
#include <thread>
#include <atomic>
#include <string>
#include <filesystem>
#include <fstream>
#include <chrono>

namespace petika {

template <typename RecordType, std::size_t QueueCapacity = 512>
class AsyncPersistenceWorker {
public:
    AsyncPersistenceWorker()
        : ring_buffer_(), running_(true),
          worker_thread_([this]() { process_queue(); }) {}

    ~AsyncPersistenceWorker() {
        stop();
    }

    // Non-blocking main-thread push (< 1 µs)
    bool enqueue(const RecordType& item, std::string target_filepath) {
        WorkItem work{item, std::move(target_filepath)};
        return ring_buffer_.try_push(std::move(work));
    }

    void stop() {
        if (running_.exchange(false)) {
            if (worker_thread_.joinable()) {
                worker_thread_.join();
            }
        }
    }

private:
    struct WorkItem {
        RecordType data{};
        std::string filepath{};
    };

    void process_queue() {
        while (running_.load(std::memory_order_relaxed)) {
            if (auto item = ring_buffer_.try_pop()) {
                // Background Glaze serialization and persistent write
                std::string json_str;
                if (!glz::write_json(item->data, json_str)) {
                    std::filesystem::path p(item->filepath);
                    if (p.has_parent_path()) {
                        std::filesystem::create_directories(p.parent_path());
                    }
                    if (std::ofstream out(item->filepath, std::ios::out | std::ios::trunc | std::ios::binary); out.is_open()) {
                        out.write(json_str.data(), static_cast<std::streamsize>(json_str.size()));
                    }
                }
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }

        // Drain remaining items on shutdown
        while (auto item = ring_buffer_.try_pop()) {
            std::string json_str;
            if (!glz::write_json(item->data, json_str)) {
                std::filesystem::path p(item->filepath);
                if (p.has_parent_path()) {
                    std::filesystem::create_directories(p.parent_path());
                }
                if (std::ofstream out(item->filepath, std::ios::out | std::ios::trunc | std::ios::binary); out.is_open()) {
                    out.write(json_str.data(), static_cast<std::streamsize>(json_str.size()));
                }
            }
        }
    }

    lockfree::RingBuffer<WorkItem, QueueCapacity> ring_buffer_;
    std::atomic<bool> running_;
    std::thread worker_thread_;
};

} // namespace petika
