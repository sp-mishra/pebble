#pragma once
// ============================================================================
// petika/adapters/pravaha.hpp — Pravaha Background Compaction & Async Store
// ============================================================================
// Modern C++23 / C++26, Header-Only, Zero Virtual Dispatch, Zero Macros.
//
// Bridges Petika storage engines with Pravaha task runners for:
//   1. Non-blocking asynchronous batch commits (commit_async)
//   2. Background periodic log compactions & retention sweeps (compact_async)
//   3. Multi-threaded parallel range scanning (parallel_for_range)
// ============================================================================

#ifndef PEBBLE_PETIKA_ADAPTERS_PRAVAHA_HPP
#define PEBBLE_PETIKA_ADAPTERS_PRAVAHA_HPP

#include "petika/petika.hpp"
#include "nitya/adapters/pravaha.hpp"
#include <pravaha/pravaha.hpp>
#include <chrono>
#include <concepts>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace petika::adapters::pravaha {

    template <typename PetikaStore = petika::StringSkipStore>
    class PravahaAsyncStore {
    public:
        using key_type = typename PetikaStore::key_type;
        using value_type = typename PetikaStore::value_type;
        using mutation_type = typename Transaction<PetikaStore>::Mutation;

        explicit PravahaAsyncStore(
            std::unique_ptr<PetikaStore> store,
            std::shared_ptr<::pravaha::Runner<::pravaha::JThreadBackend>> runner = nullptr
        ) : store_(std::move(store)),
            runner_(runner ? runner : std::make_shared<::pravaha::Runner<::pravaha::JThreadBackend>>()) {}

        explicit PravahaAsyncStore(
            petika::PetikaOptions opts = {},
            std::shared_ptr<::pravaha::Runner<::pravaha::JThreadBackend>> runner = nullptr
        ) : store_(std::make_unique<PetikaStore>(std::move(opts))),
            runner_(runner ? runner : std::make_shared<::pravaha::Runner<::pravaha::JThreadBackend>>()) {}

        // Non-blocking async batch commit dispatched to Pravaha worker
        std::future<petika::Result<void>> commit_async(std::vector<mutation_type> mutations) {
            auto promise = std::make_shared<std::promise<petika::Result<void>>>();
            auto future = promise->get_future();

            auto task_fn = [this, muts = std::move(mutations), promise]() mutable {
                auto res = store_->commit_batch(muts);
                promise->set_value(res);
            };

            (void)runner_->submit(::pravaha::task("petika_async_commit", std::move(task_fn)));
            return future;
        }

        // Asynchronous compaction & segment retention sweep via Pravaha
        std::future<void> compact_async(
            const std::chrono::seconds max_segment_age,
            const std::function<void(const nitya::segment_descriptor&)>& on_archive = nullptr,
            const std::function<void(const nitya::segment_descriptor&)>& on_delete = nullptr
        ) {
            auto promise = std::make_shared<std::promise<void>>();
            auto future = promise->get_future();

            auto task_fn = [this, max_segment_age, on_archive, on_delete, promise]() {
                store_->wal().apply_retention_rules(max_segment_age, on_archive, on_delete);
                promise->set_value();
            };

            (void)runner_->submit(::pravaha::task("petika_async_compact", std::move(task_fn)));
            return future;
        }

        // Parallel Range Scan: Partitions the key namespace into hw_threads sub-ranges,
        // submits each as an independent Pravaha task. No full materialisation of entries.
        // The engine's scan() is called per partition — only entries within the partition
        // are visited within that worker. VisitorFn must be thread-safe across partitions.
        template <typename VisitorFn>
        void parallel_for_each(VisitorFn&& visitor) {
            // Collect all distinct keys to determine partition boundaries — one pass only,
            // no value materialisation.
            std::vector<key_type> keys;
            store_->for_each([&](const auto& entry) {
                keys.push_back(entry.key);
            });

            const std::size_t n = keys.size();
            if (n == 0) return;

            const unsigned int hw_threads = std::max(1u, std::thread::hardware_concurrency());
            const std::size_t chunk_sz = std::max<std::size_t>(1, (n + hw_threads - 1) / hw_threads);

            for (std::size_t start = 0; start < n; start += chunk_sz) {
                const std::size_t end = std::min(start + chunk_sz, n);
                key_type range_start = keys[start];
                // For the last chunk, we do an open-ended scan from range_start to past the last key.
                // We use a for_each on the sub-range: scan(range_start, range_end).
                // For the final chunk, scan past the last key by using store_->scan with end = keys[n-1]+1
                // — instead, scan from range_start and count entries within chunk boundaries.
                key_type range_end = (end < n) ? keys[end] : key_type{};
                const bool is_last = (end == n);

                auto task_fn = [this, &visitor, range_start, range_end, is_last]() {
                    if (is_last) {
                        // Scan from range_start to end of keyspace using the engine's for_each
                        // filtered by range — use scan with a sentinel end key that compares
                        // greater than any key. For string keys this is the empty string trick;
                        // instead we use a separate for_each with a start key filter.
                        store_->for_each([&](const auto& entry) {
                            if (!(entry.key < range_start)) visitor(entry);
                        });
                    } else {
                        store_->scan(range_start, range_end, [&](const auto& entry) {
                            visitor(entry);
                        });
                    }
                };
                (void)runner_->submit(::pravaha::task("petika_parallel_scan", std::move(task_fn)));
            }
            runner_->backend_ref().drain();
        }

        // Synchronous pass-through accessors
        petika::Result<void> put(key_type key, value_type value) {
            return store_->put(std::move(key), std::move(value));
        }

        petika::Result<value_type> get(const key_type& key) const {
            return store_->get(key);
        }

        petika::Result<void> erase(const key_type& key) {
            return store_->erase(key);
        }

        [[nodiscard]] std::size_t size() const noexcept {
            return store_->size();
        }

        PetikaStore& store() noexcept { return *store_; }
        const PetikaStore& store() const noexcept { return *store_; }
        ::pravaha::Runner<::pravaha::JThreadBackend>& runner() noexcept { return *runner_; }

    private:
        std::unique_ptr<PetikaStore> store_;
        std::shared_ptr<::pravaha::Runner<::pravaha::JThreadBackend>> runner_;
    };

} // namespace petika::adapters::pravaha

#endif // PEBBLE_PETIKA_ADAPTERS_PRAVAHA_HPP
