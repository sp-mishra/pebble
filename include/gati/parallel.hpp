#pragma once
// ============================================================================
// gati/parallel.hpp — Pravaha Multi-Threaded Execution Adapter
// ============================================================================
// Uses pravaha::JThreadBackend and task graphs when Pravaha is enabled,
// or falls back to serial execution when Pravaha is not requested.
// ============================================================================

#include "math.hpp"
#include <cstddef>
#include <thread>
#include <utility>

#if defined(GATI_ENABLE_PRAVAHA) && __has_include("pravaha/pravaha.hpp")
#define GATI_HAS_PRAVAHA 1
#include "pravaha/pravaha.hpp"
#endif

namespace gati {

#if defined(GATI_HAS_PRAVAHA)

class ParallelExecutor {
public:
    explicit ParallelExecutor(unsigned threads = 0)
        : backend_(threads ? threads : std::thread::hardware_concurrency()),
          runner_(backend_) {}

    template <typename BodyFn>
    void for_range(std::size_t count, BodyFn&& body, std::size_t chunk = 256) {
        if (count == 0) return;
        index_range r{count};
        auto expr = pravaha::lazy_parallel_for(
            r, [body = std::forward<BodyFn>(body)](std::size_t i) { body(i); }, chunk);
        (void)runner_.submit(std::move(expr));
    }

private:
    struct index_range {
        std::size_t n;
        [[nodiscard]] std::size_t size() const noexcept { return n; }
        [[nodiscard]] std::size_t operator[](std::size_t i) const noexcept { return i; }
    };

    pravaha::JThreadBackend backend_;
    pravaha::Runner<pravaha::JThreadBackend> runner_;
};

#else // Serial fallback

class ParallelExecutor {
public:
    explicit ParallelExecutor(unsigned = 0) {}

    template <typename BodyFn>
    void for_range(std::size_t count, BodyFn&& body, std::size_t = 0) {
        for (std::size_t i = 0; i < count; ++i) {
            body(i);
        }
    }
};

#endif

} // namespace gati
