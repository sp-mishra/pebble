#pragma once
// akruti/scene/parallel.hpp — parallelism via Pravaha (guarded AKRUTI_HAS_PRAVAHA). The scene layer
// writes NO threading code: uniform per-element sweeps (AABB refit, SDF field sampling, per-query
// point/ray tests, and the OUTER loop over independent narrowphase pairs) are expressed as a Pravaha
// lazy_parallel_for lowered into its task graph. When Pravaha is absent the same call runs serially —
// identical results, zero dependency. The akruti core (math/primitives/gjk/query) never sees this.
#include "../math.hpp"

#include <cstddef>
#include <thread>

#if defined(AKRUTI_ENABLE_PRAVAHA) && __has_include("pravaha/pravaha.hpp")
#define AKRUTI_HAS_PRAVAHA 1
#include "pravaha/pravaha.hpp"
#endif

namespace akruti::scene {
#if defined(AKRUTI_HAS_PRAVAHA)

    // Shared worker pool for a Scene. Bulk ops submit uniform per-element work into it.
    // Threads = hardware concurrency by default.
    class ParallelExecutor {
    public:
        explicit ParallelExecutor(unsigned threads = 0)
            : backend_(threads ? threads : std::thread::hardware_concurrency()),
              runner_(backend_) {}

        // Apply body(i) for i in [0, count) across chunks, in parallel. Blocks until done.
        template <class BodyFn>
        void for_range(std::size_t count, BodyFn&& body, std::size_t chunk = 256) {
            if (count == 0) return;
            const std::size_t num_chunks = (count + chunk - 1) / chunk;
            std::vector<std::size_t> chunk_indices(num_chunks);
            for (std::size_t c = 0; c < num_chunks; ++c) chunk_indices[c] = c;

            auto expr = pravaha::lazy_parallel_for(
                std::span<const std::size_t>(chunk_indices),
                [body = std::forward<BodyFn>(body), count, chunk](std::size_t c) {
                    const std::size_t start = c * chunk;
                    const std::size_t end = std::min(start + chunk, count);
                    for (std::size_t i = start; i < end; ++i) {
                        body(i);
                    }
                }, 1);
            (void)runner_.submit(std::move(expr));
        }

    private:
        pravaha::JThreadBackend backend_;
        pravaha::Runner<pravaha::JThreadBackend> runner_;
    };

#else  // serial fallback — no Pravaha

    class ParallelExecutor {
    public:
        explicit ParallelExecutor(unsigned = 0) {}

        template <class BodyFn>
        void for_range(std::size_t count, BodyFn&& body, std::size_t = 0) {
            for (std::size_t i = 0; i < count; ++i) body(i);
        }
    };

#endif
} // namespace akruti::scene
