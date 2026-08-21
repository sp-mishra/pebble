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
                store_->wal().apply_retention_rules_async(*runner_, max_segment_age, on_archive, on_delete);
                promise->set_value();
            };

            (void)runner_->submit(::pravaha::task("petika_async_compact", std::move(task_fn)));
            return future;
        }

        // Parallel Range Scan: Partitions key-value iteration across Pravaha workers
        template <typename VisitorFn>
        void parallel_for_each(VisitorFn&& visitor) {
            std::vector<typename PetikaStore::entry_type> all_entries;
            store_->for_each([&](const auto& entry) {
                all_entries.push_back({entry.key, entry.value, entry.lsn});
            });

            const std::size_t n = all_entries.size();
            if (n == 0) return;

            const unsigned int hw_threads = std::max(1u, std::thread::hardware_concurrency());
            const std::size_t chunk_sz = std::max<std::size_t>(1, (n + hw_threads - 1) / hw_threads);
            const auto chunks = ::pravaha::StaticChunkingPolicy::chunks(n, chunk_sz);

            for (const auto& ch : chunks) {
                auto task_fn = [&all_entries, &visitor, begin = ch.begin, end = ch.end]() {
                    for (std::size_t i = begin; i < end; ++i) {
                        visitor(all_entries[i]);
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
