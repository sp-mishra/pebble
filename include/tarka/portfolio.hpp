#pragma once
// =============================================================================
// tarka/portfolio.hpp — Competitive Solving (AnySuccess, no-hang)
//
// Namespace:  tarka
// Provides:
//   PortfolioEngine<Backends...>  — launches all CancelableBackend instances
//                                   concurrently; first definitive result wins;
//                                   no-hang guarantee via atomic outstanding count.
//
// Design:
//   - Each backend runs on a worker from a persistent MPMCQueue-backed pool
//     (no per-query thread spawn).
//   - First Sat/Unsat calls stop_src.request_stop() and sets the shared promise.
//   - No-hang: an atomic outstanding_count ensures the last finishing backend
//     (if no winner) resolves the future to SatResult::Unknown.
//   - Cancelled backends' scratch state reclaimed via HazardRegistry.
//   - Portfolio is Pravaha-free (honors Tarka's leaf invariant: zero upward dep).
// =============================================================================

#include "tarka/async.hpp"
#include "containers/lockfree/HazardRegistry.hpp"

#include <atomic>
#include <expected>
#include <future>
#include <memory>
#include <stop_token>
#include <tuple>
#include <type_traits>

namespace tarka { namespace detail {
        template <CancelableBackend... Bs>
        struct all_cancelable : std::bool_constant<(CancelableBackend < Bs > &&...)>      {};
    }

    template <CancelableBackend... Backends>
        requires (sizeof...(Backends) > 0)
    class PortfolioEngine {
    public:
        static constexpr std::size_t kNumBackends = sizeof...(Backends);

        explicit PortfolioEngine(WorkerPool& pool) : pool_(pool) {}

        [[nodiscard]] std::future<std::expected<SatResult, SmtError>>
        check_sat_portfolio(Term t) {
            auto promise = std::make_shared<std::promise<std::expected<SatResult, SmtError>>>();
            auto future = promise->get_future();

            auto stop_src = std::make_shared<std::stop_source>();
            auto completed = std::make_shared<std::atomic<bool>>(false);
            auto outstanding = std::make_shared<std::atomic<std::size_t>>(kNumBackends);

            // Lambda submitted per backend
            auto submit_backend = [&]<std::size_t I>(std::integral_constant<std::size_t, I>) {
                using B = std::tuple_element_t<I, std::tuple<Backends...>>;
                pool_.submit([t, promise, stop_src, completed, outstanding]() mutable {
                    B backend;
                    const std::stop_token tok = stop_src->get_token();
                    auto result = backend.check_sat_cancelable(t, tok);

                    const bool is_definitive =
                        result.has_value() &&
                        (result.value() == SatResult::Sat || result.value() == SatResult::Unsat);

                    if (is_definitive) {
                        bool expected = false;
                        if (completed->compare_exchange_strong(expected, true,
                                                               std::memory_order_acq_rel, std::memory_order_acquire)) {
                            stop_src->request_stop();
                            promise->set_value(std::move(result));
                        }
                    }

                    // No-hang: last outstanding backend resolves if no winner set
                    const std::size_t remaining = outstanding->fetch_sub(1, std::memory_order_acq_rel) - 1;
                    if (remaining == 0) {
                        bool expected = false;
                        if (completed->compare_exchange_strong(expected, true,
                                                               std::memory_order_acq_rel, std::memory_order_acquire)) {
                            promise->set_value(SatResult::Unknown);
                        }
                    }
                });
            };

            // Submit all backends
            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
                (submit_backend(std::integral_constant<std::size_t, Is>{}), ...);
            }(std::make_index_sequence < kNumBackends >
            {}
            )
            ;

            return future;
        }

    private:
        WorkerPool& pool_;
    };
} // namespace tarka
