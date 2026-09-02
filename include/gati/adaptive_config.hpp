#pragma once
// gati/adaptive_config.hpp — Runtime Scene Heuristics and Adaptive Parameter Auto-Tuning.
#include <cstddef>
#include <algorithm>
#include <thread>

namespace gati {namespace detail {
#if defined(__APPLE__) && defined(__aarch64__)
        inline constexpr bool kIsAppleSilicon = true;
        inline constexpr int kNeonLanes = 4;
#else
        inline constexpr bool kIsAppleSilicon = false;
        inline constexpr int kNeonLanes = 0;
#endif

#if defined(__AVX2__)
        inline constexpr int kSimdWidth = 8;
#elif defined(__SSE4_1__) || defined(__ARM_NEON)
        inline constexpr int kSimdWidth = 4;
#else
        inline constexpr int kSimdWidth = 1;
#endif

        inline constexpr int kBatchSize = kSimdWidth;

        inline unsigned default_thread_count() noexcept {
            const unsigned hw = std::thread::hardware_concurrency();
            return hw > 1 ? hw - 1 : 1;
        }
    } // namespace detail

    struct SceneStats {
        std::size_t rigid_count = 0;
        std::size_t particle_count = 0;
        std::size_t sleeping_count = 0;
        std::size_t contact_count = 0;
        std::size_t cache_hit_count = 0;
        float median_radius = 1.0f;
        float scene_extent = 100.0f;
        float avg_closing_speed = 0.0f;
        bool has_particles = false;
        bool has_joints = false;
        bool has_fracturables = false;
    };

    struct AdaptiveConfig {
        void update(const SceneStats& stats) noexcept {
            use_spatial_hash = (stats.rigid_count + stats.particle_count) >= 200;

            velocity_iters = std::clamp(8 + int(stats.contact_count / 50), 4, 20);
            position_iters = std::clamp(2 + int(stats.contact_count / 200), 1, 6);

            enable_ccd = stats.avg_closing_speed > 5.0f;
            enable_coupling = stats.has_particles && stats.rigid_count > 0;

            sleep_threshold = stats.contact_count > 500 ? 0.02f : 0.05f;

            parallel_fracture = stats.has_fracturables &&
                detail::default_thread_count() >= 2;

            cell_size = std::max(1.0f, stats.median_radius * 2.0f);
        }

        bool use_spatial_hash = false;
        int velocity_iters = 8;
        int position_iters = 3;
        bool enable_ccd = false;
        bool enable_coupling = false;
        float sleep_threshold = 0.05f;
        bool parallel_fracture = false;
        float cell_size = 18.0f;
    };
} // namespace gati
