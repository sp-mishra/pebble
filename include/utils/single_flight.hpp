#pragma once

// =============================================================================
// utils/single_flight.hpp — generic per-key compute-once-under-contention (G4)
//
// Provides:
//   single_flight_store concept — the storage backend interface
//   single_flight<Key, Value, Store> — per-key lease + compute + publish
//
// Protocol (arch §7):
//   1. lookup(key) → hit  → touch + return value              (zero computes)
//   2. acquire_lease(key) → success → compute outside lock → publish
//   3. acquire_lease(key) → fail   → re-lookup (winner already published)
//   4. lease expiry: crashed winner doesn't wedge the key forever
//
// Key properties:
//   • compile_fn is called OUTSIDE the store's write path (long compiles safe).
//   • publish() is the only atomic critical section.
//   • N concurrent callers on a cold key: exactly one compute, N-1 reuse.
//   • Zero Lithe types: Store concept satisfies any backend (catalog, map, …).
//
// No virtual, no macros. Header-only C++23.
// =============================================================================

#include <chrono>
#include <concepts>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <thread>

namespace utils {
    // =============================================================================
    // lease_token — opaque per-key lease handle
    // =============================================================================

    struct lease_token {
        std::uint64_t id = 0;
        [[nodiscard]] bool valid() const noexcept { return id != 0; }
    };

    // =============================================================================
    // single_flight_error
    // =============================================================================

    enum class single_flight_error_code : std::uint8_t {
        compute_failed,
        publish_failed,
        store_error,
    };

    struct single_flight_error {
        single_flight_error_code code = single_flight_error_code::store_error;
        std::string detail;
    };

    // =============================================================================
    // single_flight_store concept
    //
    // Key/Value/Entry are opaque to this header; the Store binds them.
    // =============================================================================

    template <class Store, class Key, class Entry>
    concept single_flight_store =
        requires(Store& s, const Key& k, const Entry& e, const lease_token& t) {
            { s.lookup(k) } -> std::same_as<std::optional<Entry>>;
            { s.acquire_lease(k) } -> std::convertible_to<std::optional<lease_token>>;
            { s.publish(e, t) } -> std::convertible_to<bool>; // true = success
            { s.touch(k) };
        };

    // =============================================================================
    // single_flight<Key, Entry, Store>
    //
    // Usage:
    //   auto result = sf.get_or_compute(key, [&]() -> std::expected<Entry, E> { … });
    //
    // Template params:
    //   Key   — hashable key type
    //   Entry — the stored/returned entry type
    //   Store — satisfies single_flight_store<Store, Key, Entry>
    // =============================================================================

    template <class Key, class Entry, class Store>
        requires single_flight_store<Store, Key, Entry>
    class single_flight {
    public:
        struct options {
            // How many times to re-lookup before giving up on a contended key.
            std::uint32_t max_retry = 16;
            // Delay between re-lookup polls when waiting on another waiter.
            std::chrono::milliseconds retry_delay{5};
        };

        explicit single_flight(Store& store, options opts = {})
            : store_(store), opts_(opts) {}

        // -------------------------------------------------------------------------
        // get_or_compute
        //
        // compute_fn: () → std::expected<Entry, E>  (any error type)
        //   Called OUTSIDE the store's write path.
        // Returns: std::expected<Entry, single_flight_error>
        // -------------------------------------------------------------------------
        template <class ComputeFn>
        [[nodiscard]] std::expected<Entry, single_flight_error>
        get_or_compute(const Key& key, ComputeFn&& compute_fn) {
            // Fast path: cache hit.
            if (auto e = store_.lookup(key)) {
                store_.touch(key);
                return *e;
            }

            for (std::uint32_t attempt = 0; attempt < opts_.max_retry; ++attempt) {
                // Try to become the winner.
                auto token = store_.acquire_lease(key);
                if (token && token->valid()) {
                    // Winner: compute outside the store's critical section.
                    auto computed = std::invoke(std::forward<ComputeFn>(compute_fn));
                    if (!computed)
                        return std::unexpected(single_flight_error{
                            single_flight_error_code::compute_failed,
                            "compute_fn returned error"
                        });

                    const bool ok = store_.publish(*computed, *token);
                    if (!ok)
                        return std::unexpected(single_flight_error{
                            single_flight_error_code::publish_failed,
                            "store publish failed"
                        });
                    return *computed;
                }

                // Loser: re-lookup — the winner may have finished.
                std::this_thread::sleep_for(opts_.retry_delay);
                if (auto e = store_.lookup(key)) {
                    store_.touch(key);
                    return *e;
                }
            }

            return std::unexpected(single_flight_error{
                single_flight_error_code::store_error,
                "single_flight: max_retry exceeded; lease never released"
            });
        }

    private:
        Store& store_;
        options opts_;
    };
} // namespace utils
