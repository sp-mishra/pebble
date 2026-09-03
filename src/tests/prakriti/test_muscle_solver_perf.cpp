#include "catch_amalgamated.hpp"
#include "prakriti/prakriti.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>

namespace {
    struct MuscleBenchFixture {
        prakriti::ParticleStore particles;
        prakriti::MuscleStore<> store;

        explicit MuscleBenchFixture(std::uint32_t count) {
            particles.reserve(count + 1);
            store.reserve(count);

            // Chain layout keeps constraints local in memory and stable for repeated sweeps.
            for (std::uint32_t i = 0; i <= count; ++i) {
                particles.add({
                    .position = {0.2f * static_cast<float>(i), 0.0f},
                    .velocity = {0.0f, 0.0f},
                    .mass = 1.0f
                });
            }

            for (std::uint32_t i = 0; i < count; ++i) {
                (void)store.add({
                    .origin = i,
                    .insertion = i + 1,
                    .rest_length = 0.16f,
                    .tendon_slack_length = 0.05f,
                    .max_isometric_force = 750.0f,
                    .activation = 0.85f,
                    .pennation_angle = 0.1f,
                    .optimal_fiber_length = 0.10f,
                });
            }
        }
    };

    [[nodiscard]] std::uint64_t run_steps(prakriti::MuscleSolver<>& solver,
                                          MuscleBenchFixture& f,
                                          const int steps,
                                          const float dt) {
        const auto t0 = std::chrono::steady_clock::now();
        const float inv_dt2 = 1.0f / (dt * dt);
        for (int i = 0; i < steps; ++i) {
            solver.solve_substep(f.store, f.particles, dt, inv_dt2);
        }
        const auto t1 = std::chrono::steady_clock::now();
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
    }

    [[nodiscard]] bool finite_lambda(const prakriti::MuscleStore<>& store) {
        for (const float v : store.lambda_accum) {
            if (!std::isfinite(v)) return false;
        }
        return true;
    }
} // namespace

TEST_CASE (
"Prakriti: Muscle solver throughput smoke benchmark"
,
"[prakriti][muscle][perf][.]"
)
 {
    constexpr std::uint32_t kMuscles = 4096;
    constexpr int kSteps = 30;
    constexpr float kDt = 1.0f / 120.0f;

    MuscleBenchFixture serial_fixture{kMuscles};
    MuscleBenchFixture batch_fixture{kMuscles};

    // Huge chunk disables Pravaha path and approximates scalar serial mode.
    prakriti::MuscleSolver<> serial_solver{/*threads*/ 0, /*chunk_size*/ std::numeric_limits<std::size_t>::max()};
    prakriti::MuscleSolver<> batch_solver{/*threads*/ 0, /*chunk_size*/ 1024};

    const auto serial_us = run_steps(serial_solver, serial_fixture, kSteps, kDt);
    const auto batch_us = run_steps(batch_solver, batch_fixture, kSteps, kDt);

    INFO("serial_us=" << serial_us << " batch_us=" << batch_us);
    REQUIRE(finite_lambda(serial_fixture.store));
    REQUIRE(finite_lambda(batch_fixture.store));

#if defined(PRAKRITI_HAS_MUSCLE_PRAVAHA)
    MuscleBenchFixture pravaha_fixture{kMuscles};
    prakriti::MuscleSolver<> pravaha_solver{/*threads*/ 0, /*chunk_size*/ 256};
    const auto pravaha_us = run_steps(pravaha_solver, pravaha_fixture, kSteps, kDt);
    INFO ("pravaha_us=" << pravaha_us);
    REQUIRE (finite_lambda(pravaha_fixture.store));
#endif
}

