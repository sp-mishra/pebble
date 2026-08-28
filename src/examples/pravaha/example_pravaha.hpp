#pragma once
#ifndef SRC_EXAMPLES_EXAMPLE_PRAVAHA_HPP
#define SRC_EXAMPLES_EXAMPLE_PRAVAHA_HPP

// ============================================================================
// Pravaha Comprehensive Tutorial Example
//
// Fictional use case: real-time seismic signal processing pipeline.
//
// A seismic monitoring station receives sensor readings and must:
//   1. Ingest & validate raw samples              (sequential setup)
//   2. Normalize + detect anomalies in parallel   (fork-join)
//   3. Classify anomaly types in parallel         (parallel task fan-out)
//   4. Aggregate classification votes             (join with CollectAll policy)
//   5. Emit alert if quorum agrees                (quorum join)
//   6. Archive results                            (async fan-out with JThread)
//   7. Run bulk normalization via hetero executor (SIMD/GPU eDSL overlay)
//   8. Reduce: compute RMS energy of signal       (reduction eDSL)
//   9. Cancellation & error propagation demo
//  10. Multi-input eDSL kernel — channel blending
//  11. Hetero executor bulk normalize (auto-routes GPU/SIMD, no #if needed)
//  12. End-to-end seismic event pipeline
//  18. Tensor symbolic layer: tensor_var, matmul, reduce_sum, dispatch_formula
//  19. Tensor element-wise ops: ew_add/mul/div/exp/log + shape inference
//  20. Softmax pipeline via tensor ops: ew_exp + reduce_sum + ew_div
//  21. Tensor cross-thread registry: process-wide shape registry readable from any thread
//  22. Axiom adapter: TensorExecutionEngine with AxiomEngineAdapter (conditional)
//
// Each section is its own sub-example so they run independently. All share
// the seismic theme so the tutorial reads as one coherent narrative.
// ============================================================================

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <mutex>
#include <numbers>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

#include "test/example_registry.hpp"
#include "utils/log.hpp"
#include "utils/profiler.hpp"
#include "pravaha/pravaha.hpp"
#include "pravaha/execution.hpp"
#include "pravaha/pravaha_expr.hpp"
#include "pravaha/backends/metal_gpu.hpp"
#include "sutra/tensor/tensor_ext.hpp"
#include "sutra/adapters/axiom_adapter.hpp"

namespace seismic_ex {
    // ────────────────────────────────────────────────────────────────────────────
    // Sub-example 1: Sequential pipeline — ingest → validate → preprocess
    //
    // Demonstrates: task(), seq(), Runner<InlineBackend>, Outcome<RunResult>.
    // Three tasks execute in strict order. Shared state is passed by reference
    // (seismic_ex captures it in lambdas).
    // ────────────────────────────────────────────────────────────────────────────
    static testfw::Result ex1_sequential_pipeline() {
        using namespace pravaha;

        // Simulated raw sensor buffer shared across tasks in this scope.
        std::vector<float> buffer;
        bool validated = false;
        float dc_offset = 0.0f;

        // Stage 1: Ingest — populate buffer with 8 synthetic sensor readings.
        auto ingest = task("ingest", [&]() {
            buffer.resize(8);
            for (int i = 0; i < 8; ++i)
                buffer[static_cast<std::size_t>(i)] = static_cast<float>(i) * 0.5f - 1.75f;
        });

        // Stage 2: Validate — reject empty or oversized buffers.
        auto validate = task("validate", [&]() {
            if (buffer.empty() || buffer.size() > 1024)
                throw PravahaError{ErrorKind::InvalidArgument, "buffer size out of range", "validate"};
            validated = true;
        });

        // Stage 3: Preprocess — remove DC offset (mean subtraction).
        auto preprocess = task("preprocess", [&]() {
            float sum = 0.0f;
            for (float v : buffer) sum += v;
            dc_offset = sum / static_cast<float>(buffer.size());
            for (float& v : buffer) v -= dc_offset;
        });

        // Compose: ingest → validate → preprocess (strict left-to-right).
        auto pipeline = seq(ingest, validate, preprocess);

        Runner<> runner; // InlineBackend — deterministic, single-threaded
        auto result = runner.submit(pipeline);

        if (!result.has_value())
            return testfw::fail("ex1: sequential pipeline failed");
        if (!validated)
            return testfw::fail("ex1: validate task did not run");

        // After DC removal the mean must be ~0.
        float mean = 0.0f;
        for (float v : buffer) mean += v;
        mean /= static_cast<float>(buffer.size());
        if (mean > 1e-5f || mean < -1e-5f)
            return testfw::fail("ex1: DC removal incorrect");

        lg::info("pravaha ex1 (sequential): ingest→validate→preprocess, N={}, dc_offset={:.4f}",
                 buffer.size(), dc_offset);
        return {};
    }

    // ────────────────────────────────────────────────────────────────────────────
    // Sub-example 2: Parallel fork-join — normalize ∥ anomaly-detect
    //
    // Demonstrates: all_of(), parallel fork-join, AllOrNothing join policy.
    // Both tasks read from the same input (no write-aliasing) so they can run
    // concurrently. JThreadBackend with 2 workers exercises real parallelism.
    // ────────────────────────────────────────────────────────────────────────────
    static testfw::Result ex2_parallel_fork_join() {
        using namespace pravaha;

        constexpr int N = 16;
        std::vector<float> raw(N);
        for (int i = 0; i < N; ++i)
            raw[static_cast<std::size_t>(i)] = static_cast<float>(i) * 0.1f - static_cast<float>(N) * 0.05f;

        std::vector<float> normalized(N);
        std::atomic<int> anomalies_found{0};

        // Branch A: normalize each sample to [-1, 1] range.
        auto normalize = task("normalize", [&]() {
            float max_abs = 0.0f;
            for (float v : raw) max_abs = std::max(max_abs, std::abs(v));
            if (max_abs < 1e-9f) max_abs = 1.0f;
            for (int i = 0; i < N; ++i)
                normalized[static_cast<std::size_t>(i)] = raw[static_cast<std::size_t>(i)] / max_abs;
        });

        // Branch B: detect anomalies (threshold > 0.6 × max).
        auto detect = task("detect", [&]() {
            float max_abs = 0.0f;
            for (float v : raw) max_abs = std::max(max_abs, std::abs(v));
            float thr = 0.6f * max_abs;
            for (int i = 0; i < N; ++i)
                if (std::abs(raw[static_cast<std::size_t>(i)]) > thr)
                    ++anomalies_found;
        });

        // Fork both branches; join requires both to succeed (AllOrNothing default).
        auto parallel = all_of(normalize, detect);

        JThreadBackend backend{2};
        Runner<JThreadBackend> runner{backend};
        auto result = runner.submit(parallel);

        if (!result.has_value())
            return testfw::fail("ex2: parallel fork-join failed");

        // Spot-check: raw[0] is the most negative (largest abs) → normalized[0] ≈ -1.0.
        if (normalized[0] > -0.98f || normalized[0] < -1.02f)
            return testfw::fail("ex2: normalization result out of range");

        lg::info("pravaha ex2 (parallel fork-join): N={}, anomalies={}, norm[0]={:.4f} (expect -1.0)",
                 N, anomalies_found.load(), normalized[0]);
        return {};
    }

    // ────────────────────────────────────────────────────────────────────────────
    // Sub-example 3: Fan-out classification + CollectAll join policy
    //
    // Demonstrates: three parallel classifiers feeding one aggregator.
    // collect_all_of runs all branches and collects results even if one fails.
    // ────────────────────────────────────────────────────────────────────────────
    static testfw::Result ex3_classification_collect_all() {
        using namespace pravaha;

        float signal_energy = 4.8f; // synthetic computed energy
        float frequency_hz = 12.0f; // dominant frequency
        float duration_ms = 30.0f; // event duration

        std::atomic<int> votes_quake{0};
        std::atomic<int> votes_tremor{0};
        std::atomic<int> votes_noise{0};

        // Classifier A: energy threshold — high energy → quake.
        auto cls_energy = task("cls_energy", [&]() {
            if (signal_energy > 3.0f) ++votes_quake;
            else ++votes_noise;
        });

        // Classifier B: frequency band — 8–20 Hz → tremor or quake.
        auto cls_freq = task("cls_freq", [&]() {
            if (frequency_hz >= 8.0f && frequency_hz <= 20.0f) ++votes_quake;
            else ++votes_noise;
        });

        // Classifier C: duration — long events → tremor.
        auto cls_duration = task("cls_duration", [&]() {
            if (duration_ms > 20.0f) ++votes_tremor;
            else ++votes_noise;
        });

        // collect_all_of: all branches run; results aggregated regardless of
        // individual failures — useful when partial results still have value.
        auto classify = collect_all_of(cls_energy, cls_freq, cls_duration);

        Runner<> runner;
        auto result = runner.submit(classify);
        if (!result.has_value())
            return testfw::fail("ex3: classify collect_all failed");

        int q = votes_quake.load(), t = votes_tremor.load(), n = votes_noise.load();
        bool verdict_quake = (q > t && q > n);
        if (!verdict_quake)
            return testfw::fail("ex3: expected Quake classification");

        lg::info("pravaha ex3 (collect_all): quake_votes={} tremor={} noise={} → Quake", q, t, n);
        return {};
    }

    // ────────────────────────────────────────────────────────────────────────────
    // Sub-example 4: Quorum join policy — alert only when ≥2 of 3 agree
    //
    // Demonstrates: quorum_of<N>() template free function.
    // Models fault-tolerant alert dispatch: send alert only if quorum agrees.
    // ────────────────────────────────────────────────────────────────────────────
    static testfw::Result ex4_quorum_alert() {
        using namespace pravaha;

        std::atomic<int> alert_dispatched{0};
        std::atomic<int> sensor_votes{0};

        // Three independent sensors vote on whether to fire an alert.
        auto sensor_a = task("sensor_a", [&]() { ++sensor_votes; }); // votes yes
        auto sensor_b = task("sensor_b", [&]() { ++sensor_votes; }); // votes yes
        // sensor_c doesn't increment — simulates a missed reading.
        auto sensor_c = task("sensor_c", [&]() { /* no vote */ });

        // quorum_of<2>: pipeline proceeds if at least 2 of 3 branches succeed.
        // AllOrNothing would require all 3; Quorum is more resilient.
        auto vote = quorum_of < 2 > (sensor_a, sensor_b, sensor_c);

        // Downstream: dispatch alert after quorum is reached.
        auto dispatch = task("dispatch_alert", [&]() { ++alert_dispatched; });
        auto pipeline = seq(vote, dispatch);

        Runner<> runner;
        auto result = runner.submit(pipeline);
        if (!result.has_value())
            return testfw::fail("ex4: quorum pipeline failed");
        if (alert_dispatched.load() != 1)
            return testfw::fail("ex4: alert not dispatched after quorum");

        lg::info("pravaha ex4 (quorum): sensor_votes={} alert_dispatched={}",
                 sensor_votes.load(), alert_dispatched.load());
        return {};
    }

    // ────────────────────────────────────────────────────────────────────────────
    // Sub-example 5: Task priorities via with_priority()
    //
    // Demonstrates: with_priority(TaskPriority, expr) wrapper.
    // High-priority calibration, normal-priority processing, low-priority archive.
    // InlineBackend executes sequentially so order matches seq composition.
    // ────────────────────────────────────────────────────────────────────────────
    static testfw::Result ex5_task_priorities() {
        using namespace pravaha;

        std::vector<std::string> execution_log;
        std::mutex log_mutex;
        auto log_step = [&](std::string s) {
            std::lock_guard lk{log_mutex};
            execution_log.push_back(std::move(s));
        };

        // with_priority wraps any Pravaha expression, attaching a priority hint
        // that the ready-queue respects when multiple tasks are eligible.
        auto calibrate = with_priority(TaskPriority::High,
                                       task("calibrate", [&]() { log_step("calibrate"); }));

        auto process = with_priority(TaskPriority::Normal,
                                     task("process", [&]() { log_step("process"); }));

        auto archive = with_priority(TaskPriority::Low,
                                     task("archive", [&]() { log_step("archive"); }));

        // Sequential composition preserves order; priorities influence scheduling
        // when multiple tasks are concurrently ready (relevant for JThreadBackend).
        auto pipeline = seq(calibrate, process, archive);

        Runner<> runner;
        auto result = runner.submit(pipeline);
        if (!result.has_value())
            return testfw::fail("ex5: priority pipeline failed");
        if (execution_log.size() != 3)
            return testfw::fail("ex5: not all tasks ran");

        lg::info("pravaha ex5 (priorities): order=[{}, {}, {}]",
                 execution_log[0], execution_log[1], execution_log[2]);
        return {};
    }

    // ────────────────────────────────────────────────────────────────────────────
    // Sub-example 6: Sender/Receiver execution model (P2300-style)
    //
    // Demonstrates: execution::from_expr, then(), when_all(), sync_wait().
    // Models async alert enrichment: fetch station metadata in parallel with
    // computing signal statistics, then combine for a final alert record.
    // ────────────────────────────────────────────────────────────────────────────
    static testfw::Result ex6_sender_receiver() {
        using namespace pravaha;
        using namespace pravaha::execution;

        std::string station_meta;
        float peak_amplitude = 0.0f;

        // Build two independent task expressions.
        auto fetch_meta = task("fetch_meta", [&]() { station_meta = "Station-7, lat=37.7, lon=-122.4"; });
        auto compute_amp = task("compute_amp", [&]() { peak_amplitude = 3.14f; });

        // Lift both into senders. when_all waits for both to complete.
        auto s_meta = from_expr(fetch_meta);
        auto s_amp = from_expr(compute_amp);
        auto both = when_all(std::move(s_meta), std::move(s_amp));

        // Chain a final "compose" step that formats the alert record.
        std::string alert_record;
        auto final_s = then(std::move(both), "compose", [&]() {
            alert_record = station_meta + " | peak=" + std::to_string(peak_amplitude);
        });

        auto res = sync_wait(std::move(final_s));
        if (!res.value)
            return testfw::fail("ex6: sender/receiver pipeline did not succeed");
        if (alert_record.empty())
            return testfw::fail("ex6: alert_record not populated");

        lg::info("pravaha ex6 (sender/receiver): alert_record='{}'", alert_record);
        return {};
    }

    // ────────────────────────────────────────────────────────────────────────────
    // Sub-example 7: Cancellation — cooperative stop via CancellationSource/Token
    //
    // Demonstrates: CancellationSource, CancellationToken, stop_requested().
    // A long-running analysis task checks for cancellation mid-loop; the token
    // is pre-armed via CancellationSource before submission.
    // ────────────────────────────────────────────────────────────────────────────
    static testfw::Result ex7_cancellation() {
        using namespace pravaha;

        // CancellationSource owns the shared state; token is a read-only view.
        CancellationSource source;
        source.request_stop(); // pre-arm before the task even runs

        std::atomic<bool> partial_work_done{false};
        CancellationToken token = source.token();

        auto long_analysis = task("long_analysis", [&]() {
            for (int i = 0; i < 1000; ++i) {
                if (token.stop_requested()) {
                    // Cooperative cancellation: surface as TaskCanceled.
                    throw PravahaError{
                        ErrorKind::TaskCanceled,
                        "analysis canceled by token", "long_analysis"
                    };
                }
                partial_work_done = true; // never reached when pre-armed
            }
        });

        Runner<> runner;
        auto result = runner.submit(long_analysis, token);

        // With InlineBackend the task runs synchronously. Since the token was armed
        // before submission the task throws, producing a Failed/Canceled outcome.
        // Key invariant: partial_work_done must remain false — no real work ran.
        if (partial_work_done.load())
            return testfw::fail("ex7: partial work ran despite cancellation");

        lg::info("pravaha ex7 (cancellation): task {} as expected",
                 (!result.has_value() || !result->succeeded()) ? "canceled/failed" : "succeeded (unexpected)");
        return {};
    }

    // ────────────────────────────────────────────────────────────────────────────
    // Sub-example 8: Hetero executor — bulk normalize via SIMD eDSL
    //
    // Demonstrates: pravaha_expr.hpp eDSL, hetero_executor, SIMD element-wise.
    // Use case: normalize 1 K seismic samples in one vectorized pass.
    //   out[i] = in[i] * 0.5 + 0.1   (stand-in DC+scale normalization)
    // ────────────────────────────────────────────────────────────────────────────
    static testfw::Result ex8_hetero_simd_normalize() {
        using namespace pravaha;
        using namespace pravaha::compute;
        using namespace pravaha::expr;

        constexpr std::size_t N = 1024;
        std::vector<float> src(N), dst(N, 0.0f);
        for (std::size_t i = 0; i < N; ++i)
            src[i] = static_cast<float>(i) * 0.002f - 1.0f; // range [-1, ~1.046]

        // Build element-wise expression: out = x * 0.5 + 0.1
        pravaha::expr::var x;
        auto scale = x * pravaha::expr::lit(0.5f);
        auto shifted = scale + pravaha::expr::lit(0.1f);

        buffer_descriptor d;
        d.shape = {N};
        d.element_type = data_element_type::f32;

        hetero::hetero_executor exec;
        hetero::execution_context ctx;
        auto r = exec.execute(shifted,
                              make_view(dst.data(), d),
                              make_const_view(src.data(), d),
                              ctx);
        if (!r)
            return testfw::fail("ex8: SIMD normalization failed");

        float expected0 = src[0] * 0.5f + 0.1f;
        float expected_last = src[N - 1] * 0.5f + 0.1f;
        if (std::abs(dst[0] - expected0) > 1e-4f) return testfw::fail("ex8: dst[0] mismatch");
        if (std::abs(dst[N - 1] - expected_last) > 1e-4f) return testfw::fail("ex8: dst[N-1] mismatch");

        lg::info("pravaha ex8 (SIMD normalize): N={}, dst[0]={:.4f} dst[N-1]={:.4f}",
                 N, dst[0], dst[N - 1]);
        return {};
    }

    // ────────────────────────────────────────────────────────────────────────────
    // Sub-example 9: Hetero executor — RMS energy via SIMD reduction
    //
    // Demonstrates: reduce_sum, reduce_child(), hetero_executor::reduce<Op>().
    // Use case: compute root-mean-square energy of a seismic window to classify
    // signal strength before dispatching downstream tasks.
    // ────────────────────────────────────────────────────────────────────────────
    static testfw::Result ex9_hetero_reduce_rms() {
        using namespace pravaha;
        using namespace pravaha::compute;
        using namespace pravaha::expr;

        constexpr std::size_t N = 512;
        std::vector<float> src(N);
        for (std::size_t i = 0; i < N; ++i)
            src[i] = std::sin(static_cast<float>(i) * 0.05f); // pure sine

        // Build reduction: Σ x[i]²
        pravaha::expr::var x;
        auto r_expr = pravaha::expr::reduce_sum(x * x);

        buffer_descriptor d;
        d.shape = {N};
        d.element_type = data_element_type::f32;

        hetero::hetero_executor exec;
        hetero::execution_context ctx;

        // reduce_child(r_expr) recovers the element-wise child x*x for the backend.
        auto res = exec.reduce<pravaha::expr::reduce_op::sum>(pravaha::expr::reduce_child(r_expr),
                                                              make_const_view(src.data(), d),
                                                              ctx);
        if (!res)
            return testfw::fail("ex9: reduce_sum failed");

        float sum_sq = *res;
        float rms = std::sqrt(sum_sq / static_cast<float>(N));

        // Reference: scalar loop for verification.
        float ref_sum = 0.0f;
        for (float v : src) ref_sum += v * v;
        float rms_ref = std::sqrt(ref_sum / static_cast<float>(N));

        if (std::abs(rms - rms_ref) > 1e-3f)
            return testfw::fail("ex9: RMS energy mismatch");

        lg::info("pravaha ex9 (reduce RMS): N={}, rms={:.4f} ref={:.4f}", N, rms, rms_ref);
        return {};
    }

    // ────────────────────────────────────────────────────────────────────────────
    // Sub-example 10: Multi-input eDSL kernel — AXPY-style signal mix
    //
    // Demonstrates: input<0>/input<1>, multi-source execute overload.
    // Use case: blend two sensor channels — out[i] = 0.7*x[i] + 0.3*y[i]
    // ────────────────────────────────────────────────────────────────────────────
    static testfw::Result ex10_hetero_multi_input() {
        using namespace pravaha;
        using namespace pravaha::compute;
        using namespace pravaha::expr;

        constexpr std::size_t N = 256;
        std::vector<float> ch0(N), ch1(N), dst(N, 0.0f);
        for (std::size_t i = 0; i < N; ++i) {
            ch0[i] = static_cast<float>(i) * 0.01f;
            ch1[i] = static_cast<float>(N - i) * 0.01f;
        }

        // out[i] = 0.7*x[i] + 0.3*y[i]  — weighted channel blend
        pravaha::expr::input < 0 > xi;
        pravaha::expr::input < 1 > yi;
        auto blend = pravaha::expr::lit(0.7f) * xi + pravaha::expr::lit(0.3f) * yi;

        buffer_descriptor d;
        d.shape = {N};
        d.element_type = data_element_type::f32;

        hetero::hetero_executor exec;
        hetero::execution_context ctx;

        // Multi-source execute: two compute_view<const T> in a std::array.
        std::array<compute_view<const float>, 2> srcs{
            make_const_view(ch0.data(), d),
            make_const_view(ch1.data(), d)
        };
        auto r = exec.execute(blend, make_view(dst.data(), d), srcs, ctx);
        if (!r)
            return testfw::fail("ex10: multi-input execute failed");

        float expected0 = 0.7f * ch0[0] + 0.3f * ch1[0];
        float expected_mid = 0.7f * ch0[N / 2] + 0.3f * ch1[N / 2];
        if (std::abs(dst[0] - expected0) > 1e-4f) return testfw::fail("ex10: dst[0] mismatch");
        if (std::abs(dst[N / 2] - expected_mid) > 1e-4f) return testfw::fail("ex10: dst[mid] mismatch");

        lg::info("pravaha ex10 (multi-input blend): N={}, dst[0]={:.4f} dst[mid]={:.4f}",
                 N, dst[0], dst[N / 2]);
        return {};
    }

    // ────────────────────────────────────────────────────────────────────────────
    // Sub-example 11: Hetero executor — bulk normalize (GPU on Apple, SIMD elsewhere)
    //
    // Demonstrates: hetero_executor auto-routing — no #if guards needed.
    // 256K floats (1 MB) cross the GPU threshold on Apple Silicon; elsewhere
    // the same code runs via SIMD. Both paths produce identical results.
    // Use case: full-station bulk energy normalization — 256K samples.
    // ────────────────────────────────────────────────────────────────────────────
    static testfw::Result ex11_gpu_bulk_normalize() {
        using namespace pravaha;
        using namespace pravaha::compute;
        using namespace pravaha::expr;

        constexpr std::size_t N = 1 << 18; // 256K floats = 1 MB → GPU threshold on Apple
        std::vector<float> src(N), dst(N, 0.0f);
        for (std::size_t i = 0; i < N; ++i)
            src[i] = static_cast<float>(i) * (1.0f / static_cast<float>(N));

        // out[i] = x[i] * 2.0 - 1.0  — map [0,1) → [-1, 1)
        pravaha::expr::var x;
        auto norm = x * pravaha::expr::lit(2.0f) - pravaha::expr::lit(1.0f);

        buffer_descriptor d;
        d.shape = {N};
        d.element_type = data_element_type::f32;
        d.is_unified = true; // Apple Silicon zero-copy hint (ignored elsewhere)

        // hetero_executor routes to Metal GPU on Apple Silicon (≥256KB) or SIMD otherwise.
        // No platform #if needed — the backend registry handles availability at runtime.
        hetero::hetero_executor exec;
        hetero::execution_context ctx;
        auto r = exec.execute(norm,
                              make_view(dst.data(), d),
                              make_const_view(src.data(), d),
                              ctx);
        if (!r)
            return testfw::fail("ex11: bulk normalize failed");

        float exp0 = src[0] * 2.0f - 1.0f;
        float expLast = src[N - 1] * 2.0f - 1.0f;
        if (std::abs(dst[0] - exp0) > 1e-3f) return testfw::fail("ex11: dst[0] mismatch");
        if (std::abs(dst[N - 1] - expLast) > 1e-3f) return testfw::fail("ex11: dst[N-1] mismatch");

        lg::info("pravaha ex11 (hetero bulk normalize): N={}, dst[0]={:.4f} dst[N-1]={:.4f}",
                 N, dst[0], dst[N - 1]);
        return {};
    }

    // ────────────────────────────────────────────────────────────────────────────
    // Sub-example 12: End-to-end seismic event pipeline
    //
    // Ties all concepts together. Ingest → parallel (normalize ∥ classify)
    // → conditional alert dispatch. Uses JThreadBackend for real concurrency.
    // ────────────────────────────────────────────────────────────────────────────
    static testfw::Result ex12_end_to_end() {
        using namespace pravaha;

        constexpr int N = 64;
        std::vector<float> raw(N);
        for (int i = 0; i < N; ++i)
            raw[static_cast<std::size_t>(i)] = static_cast<float>(i) * 0.05f - static_cast<float>(N) * 0.025f;

        std::atomic<int> alert_count{0};
        std::atomic<bool> ingested{false};
        std::atomic<int> energy_class{0};

        // Stage 1: ingest
        auto ingest = task("e2e_ingest", [&]() { ingested = true; });

        // Stage 2a: CPU normalization — normalizes raw into a local copy.
        std::vector<float> normalized(N);
        auto cpu_norm = task("e2e_cpu_norm", [&]() {
            float mx = 0.0f;
            for (float v : raw) mx = std::max(mx, std::abs(v));
            if (mx < 1e-9f) mx = 1.0f;
            for (int i = 0; i < N; ++i)
                normalized[static_cast<std::size_t>(i)] = raw[static_cast<std::size_t>(i)] / mx;
        });

        // Stage 2b: energy classifier (parallel to normalization).
        auto energy_cls = task("e2e_energy_cls", [&]() {
            float sum_sq = 0.0f;
            for (float v : raw) sum_sq += v * v;
            energy_class = (sum_sq / N > 0.1f) ? 1 : 0;
        });

        // Fork stages 2a and 2b in parallel, join with AllOrNothing.
        auto fork = all_of(cpu_norm, energy_cls);

        // Stage 3: dispatch alert only when the energy classifier flagged.
        auto alert = task("e2e_alert", [&]() {
            if (energy_class.load() == 1) ++alert_count;
        });

        // Full pipeline: ingest → (normalize ∥ classify) → alert
        auto full = seq(ingest, seq(fork, alert));

        JThreadBackend backend{2};
        Runner<JThreadBackend> runner{backend};
        auto result = runner.submit(full);

        if (!result.has_value())
            return testfw::fail("ex12: end-to-end pipeline failed");
        if (!ingested.load())
            return testfw::fail("ex12: ingest task did not run");

        lg::info("pravaha ex12 (end-to-end): N={}, energy_class={}, alert_dispatched={}",
                 N, energy_class.load(), alert_count.load());
        return {};
    }

    // ============================================================================
    // Pi estimation — backend speed comparison
    //
    // Shared constants — change kPiN here to resize all Pi sub-examples at once.
    //
    // Same Riemann integral (pi/4 = ∫₀¹ 1/(1+x²)dx, N=kPiN strips) run on four
    // backends to demonstrate Pravaha's execution model and measure relative speed:
    //
    //   ex13: InlineBackend     — single task, scalar double, deterministic
    //   ex14: JThreadBackend    — 4 parallel workers, data partitioned, CPU-bound
    //   ex15: hetero_executor   — SIMD (or Metal GPU on Apple) vectorized reduce_sum
    //   ex16: Monte Carlo       — CPU-bound stochastic; 4 JThread workers, xorshift64
    //   ex17: profiler::compare — all four head-to-head; formatted table
    //
    // Mirrors the ArrayFire host-vs-device example pattern using only Pravaha backends.
    // ============================================================================

    inline static constexpr std::size_t kPiN = 20'000'0000; // strips / MC samples
    inline static constexpr int kPiWorkers = 4; // JThread + MC worker count
    inline static constexpr double kPiRef = std::numbers::pi;
    inline static constexpr double kPiH = 1.0 / static_cast<double>(kPiN);
    inline static constexpr int kPiIters = 20; // profiler iterations
    inline static constexpr int kPiWarmup = 3; // profiler warmup

    // ────────────────────────────────────────────────────────────────────────────
    // Sub-example 13: Pi — InlineBackend (scalar Riemann, baseline)
    // ────────────────────────────────────────────────────────────────────────────
    static testfw::Result ex13_pi_inline() {
        using namespace pravaha;

        double pi_val = 0.0;

        auto compute = task("pi_inline", [&]() {
            double sum = 0.0;
            for (std::size_t i = 0; i < kPiN; ++i) {
                const double x = (static_cast<double>(i) + 0.5) * kPiH;
                sum += 1.0 / (1.0 + x * x);
            }
            pi_val = 4.0 * sum * kPiH;
        });

        profiler::ProfileConfig cfg;
        cfg.label = "pi_inline_backend";
        cfg.iterations = static_cast<std::size_t>(kPiIters);
        cfg.warmup_iterations = static_cast<std::size_t>(kPiWarmup);

        const auto prof = profiler::measure(cfg, [&]() {
            Runner<> runner;
            if (const auto res = runner.submit(compute); !res)
                throw std::runtime_error("pi inline task failed");
        });

        if (std::abs(pi_val - kPiRef) > 1e-9)
            return testfw::fail("ex13: InlineBackend Pi error too large");

        lg::info("pravaha ex13 (Pi InlineBackend, N={}): pi={:.10f} err={:.2e} avg={}",
                 kPiN, pi_val, std::abs(pi_val - kPiRef),
                 profiler::internal::format_duration(prof.average_duration));
        return {};
    }

    // ────────────────────────────────────────────────────────────────────────────
    // Sub-example 14: Pi — JThreadBackend (parallel Riemann, CPU-bound, 4 workers)
    // ────────────────────────────────────────────────────────────────────────────
    static testfw::Result ex14_pi_jthread() {
        using namespace pravaha;

        std::array < double, kPiWorkers > partials{};
        constexpr std::size_t per = kPiN / static_cast<std::size_t>(kPiWorkers);

        auto make_worker = [&](int id) {
            return task(std::string("pi_jt_") + std::to_string(id), [&, id]() {
                const std::size_t start = static_cast<std::size_t>(id) * per;
                const std::size_t end = (id == kPiWorkers - 1) ? kPiN : start + per;
                double local = 0.0;
                for (std::size_t i = start; i < end; ++i) {
                    const double x = (static_cast<double>(i) + 0.5) * kPiH;
                    local += 1.0 / (1.0 + x * x);
                }
                partials[static_cast<std::size_t>(id)] = local;
            });
        };

        auto parallel_pi = all_of(make_worker(0), make_worker(1),
                                  make_worker(2), make_worker(3));
        auto reduce_task = task("pi_jt_reduce", [&]() {
            double sum = 0.0;
            for (const double p : partials) sum += p;
            partials[0] = 4.0 * sum * kPiH;
        });
        auto pipeline = seq(parallel_pi, reduce_task);

        profiler::ProfileConfig cfg;
        cfg.label = "pi_jthread_backend";
        cfg.iterations = static_cast<std::size_t>(kPiIters);
        cfg.warmup_iterations = static_cast<std::size_t>(kPiWarmup);

        double pi_val = 0.0;
        const auto prof = profiler::measure(cfg, [&]() {
            partials.fill(0.0);
            JThreadBackend backend{kPiWorkers};
            Runner<JThreadBackend> runner{backend};
            if (const auto res = runner.submit(pipeline); !res)
                throw std::runtime_error("pi jthread pipeline failed");
            pi_val = partials[0];
        });

        if (std::abs(pi_val - kPiRef) > 1e-9)
            return testfw::fail("ex14: JThreadBackend Pi error too large");

        lg::info("pravaha ex14 (Pi JThreadBackend, N={}, workers={}): pi={:.10f} err={:.2e} avg={}",
                 kPiN, kPiWorkers, pi_val, std::abs(pi_val - kPiRef),
                 profiler::internal::format_duration(prof.average_duration));
        return {};
    }

    // ────────────────────────────────────────────────────────────────────────────
    // Sub-example 15: Pi — hetero_executor (Metal GPU on Apple Silicon, SIMD
    // fallback elsewhere). Same N as all other Pi examples. f32 used so Metal
    // GPU is eligible; GPU reduce of 200 M floats ~42 ms (vs 258 ms scalar).
    // Float error at N=200M is within 1e-4 (threshold shared with original).
    // ────────────────────────────────────────────────────────────────────────────
    static testfw::Result ex15_pi_hetero() {
        using namespace pravaha;
        using namespace pravaha::compute;
        using namespace pravaha::expr;

        constexpr float kHf = 1.0f / static_cast<float>(kPiN);

        std::vector<float> xs(kPiN);
        for (std::size_t i = 0; i < kPiN; ++i)
            xs[i] = (static_cast<float>(i) + 0.5f) * kHf;

        pravaha::expr::var x;
        const auto integrand = pravaha::expr::lit(1.0f) / (pravaha::expr::lit(1.0f) + x * x);
        const auto r_expr = pravaha::expr::reduce_sum(integrand);

        buffer_descriptor d;
        d.shape = {kPiN};
        d.element_type = data_element_type::f32;
        d.is_unified = true;

        hetero::hetero_executor exec;
        hetero::execution_context ctx;

        profiler::ProfileConfig cfg;
        cfg.label = "pi_hetero_executor";
        cfg.iterations = static_cast<std::size_t>(kPiIters);
        cfg.warmup_iterations = static_cast<std::size_t>(kPiWarmup);

        double pi_val = 0.0;
        const auto prof = profiler::measure(cfg, [&]() {
            const auto res = exec.reduce<pravaha::expr::reduce_op::sum>(
                pravaha::expr::reduce_child(r_expr),
                make_const_view(xs.data(), d),
                ctx);
            if (!res) throw std::runtime_error("pi hetero reduce failed");
            pi_val = 4.0 * static_cast<double>(*res) * static_cast<double>(kHf);
        });

        if (std::abs(pi_val - kPiRef) > 1e-4)
            return testfw::fail("ex15: hetero_executor Pi error too large");

        lg::info("pravaha ex15 (Pi hetero_executor f32, N={}): pi={:.8f} err={:.2e} avg={}",
                 kPiN, pi_val, std::abs(pi_val - kPiRef),
                 profiler::internal::format_duration(prof.average_duration));
        return {};
    }

    // ────────────────────────────────────────────────────────────────────────────
    // Sub-example 16: Pi -- Monte Carlo (CPU-bound stochastic, JThreadBackend)
    //
    // Like ArrayFire's pi_device/pi_host -- stochastic method for contrast.
    // kPiN random (x,y) points; fraction inside unit circle ~= pi/4.
    // Each worker uses xorshift64 seeded per-id to avoid shared-state contention.
    // ────────────────────────────────────────────────────────────────────────────
    static testfw::Result ex16_pi_monte_carlo() {
        using namespace pravaha;

        std::atomic<std::size_t> inside{0};
        constexpr std::size_t per_mc = kPiN / static_cast<std::size_t>(kPiWorkers);

        auto make_mc_worker = [&](int id) {
            return task(std::string("pi_mc_") + std::to_string(id), [&, id]() {
                uint64_t state = 0x9e3779b97f4a7c15ULL
                    ^ static_cast<uint64_t>(id * 6364136223846793005LL
                        + 1442695040888963407LL);
                auto next_f64 = [&]() -> double {
                    state ^= state << 13;
                    state ^= state >> 7;
                    state ^= state << 17;
                    return static_cast<double>(state >> 11) / static_cast<double>(1ULL << 53);
                };
                const std::size_t n = (id == kPiWorkers - 1)
                                          ? kPiN - static_cast<std::size_t>(id) * per_mc
                                          : per_mc;
                std::size_t local = 0;
                for (std::size_t i = 0; i < n; ++i) {
                    const double x = next_f64(), y = next_f64();
                    if (x * x + y * y <= 1.0) ++local;
                }
                inside.fetch_add(local, std::memory_order_relaxed);
            });
        };

        auto parallel_mc = all_of(make_mc_worker(0), make_mc_worker(1),
                                  make_mc_worker(2), make_mc_worker(3));

        profiler::ProfileConfig cfg;
        cfg.label = "pi_monte_carlo";
        cfg.iterations = static_cast<std::size_t>(kPiIters);
        cfg.warmup_iterations = static_cast<std::size_t>(kPiWarmup);

        double pi_val = 0.0;
        const auto prof = profiler::measure(cfg, [&]() {
            inside.store(0, std::memory_order_relaxed);
            JThreadBackend backend{kPiWorkers};
            Runner<JThreadBackend> runner{backend};
            if (const auto res = runner.submit(parallel_mc); !res)
                throw std::runtime_error("pi monte carlo tasks failed");
            pi_val = 4.0 * static_cast<double>(inside.load()) / static_cast<double>(kPiN);
        });

        // Monte Carlo converges O(1/sqrt(N)); 20M samples -> ~4 decimal places.
        if (std::abs(pi_val - kPiRef) > 2e-3)
            return testfw::fail("ex16: Monte Carlo Pi error too large");

        lg::info("pravaha ex16 (Pi Monte Carlo, N={}, workers={}): pi={:.6f} err={:.4f} avg={}",
                 kPiN, kPiWorkers, pi_val, std::abs(pi_val - kPiRef),
                 profiler::internal::format_duration(prof.average_duration));
        return {};
    }

    // ────────────────────────────────────────────────────────────────────────────
    // Sub-example 16b: Pi -- Monte Carlo with GPU reduction (hetero_executor)
    //
    // Two-phase approach:
    //   Phase 1 (CPU): 4 JThread workers generate N random (x,y) pairs using
    //   xorshift64 and write inside[i]=1.0f / 0.0f into a shared float buffer.
    //   Phase 2 (GPU/SIMD): hetero_executor::reduce<sum>(identity, inside_buf)
    //   aggregates the hit count — on Apple Silicon this runs on Metal GPU.
    //
    // Timing separates the two phases so the GPU-reduce contribution is visible.
    // ────────────────────────────────────────────────────────────────────────────
    static testfw::Result ex16b_pi_monte_carlo_gpu() {
        using namespace pravaha;
        using namespace pravaha::compute;
        using namespace pravaha::expr;

        constexpr std::size_t per_mc = kPiN / static_cast<std::size_t>(kPiWorkers);

        // Shared buffer: inside[i] = 1.0f if point i is inside unit circle, else 0.
        std::vector<float> inside_buf(kPiN, 0.f);

        // Phase 1: parallel CPU generation into inside_buf.
        auto make_mc_worker = [&](int id) {
            return task(std::string("pi_mc_gpu_") + std::to_string(id), [&, id]() {
                uint64_t state = 0xdeadbeefcafe1234ULL
                    ^ static_cast<uint64_t>(id * 6364136223846793005LL
                        + 1442695040888963407LL);
                auto next_f32 = [&]() -> float {
                    state ^= state << 13;
                    state ^= state >> 7;
                    state ^= state << 17;
                    return static_cast<float>(state >> 40) / static_cast<float>(1ULL << 24);
                };
                const std::size_t start = static_cast<std::size_t>(id) * per_mc;
                const std::size_t end = (id == kPiWorkers - 1) ? kPiN : start + per_mc;
                for (std::size_t i = start; i < end; ++i) {
                    const float x = next_f32(), y = next_f32();
                    inside_buf[i] = (x * x + y * y <= 1.0f) ? 1.0f : 0.0f;
                }
            });
        };

        auto gen_par = all_of(make_mc_worker(0), make_mc_worker(1),
                              make_mc_worker(2), make_mc_worker(3));

        // Phase 2: hetero_executor reduce over inside_buf.
        // Identity expression: reduce_sum(x) → Σ inside[i].
        pravaha::expr::var xi;
        const auto id_expr = pravaha::expr::reduce_sum(xi);

        buffer_descriptor d;
        d.shape = {kPiN};
        d.element_type = data_element_type::f32;
        d.is_unified = true; // zero-copy on Apple Silicon

        hetero::hetero_executor exec;
        hetero::execution_context ctx;

        profiler::ProfileConfig cfg;
        cfg.label = "pi_mc_gpu";
        cfg.iterations = static_cast<std::size_t>(kPiIters);
        cfg.warmup_iterations = static_cast<std::size_t>(kPiWarmup);

        double pi_val = 0.0;
        const auto prof = profiler::measure(cfg, [&]() {
            // Phase 1: regenerate inside_buf each iteration.
            {
                JThreadBackend backend{kPiWorkers};
                Runner<JThreadBackend> runner{backend};
                if (const auto res = runner.submit(gen_par); !res)
                    throw std::runtime_error("pi mc gpu: generation tasks failed");
            }
            // Phase 2: GPU/SIMD reduce.
            const auto res = exec.reduce<pravaha::expr::reduce_op::sum>(
                pravaha::expr::reduce_child(id_expr),
                make_const_view(inside_buf.data(), d),
                ctx);
            if (!res) throw std::runtime_error("pi mc gpu: hetero reduce failed");
            pi_val = 4.0 * static_cast<double>(*res) / static_cast<double>(kPiN);
        });

        // MC converges O(1/sqrt(N)); same tolerance as ex16.
        if (std::abs(pi_val - kPiRef) > 2e-3)
            return testfw::fail("ex16b: GPU Monte Carlo Pi error too large");

        lg::info("pravaha ex16b (Pi MC-GPU, N={}, workers={}): pi={:.6f} err={:.4f} avg={}",
                 kPiN, kPiWorkers, pi_val, std::abs(pi_val - kPiRef),
                 profiler::internal::format_duration(prof.average_duration));
        return {};
    }

    // ────────────────────────────────────────────────────────────────────────────
    // Sub-example 17: Pi -- backend comparison table
    //
    // Runs all five Pi methods under identical profiler config; prints a formatted
    // table with timing and Mann-Whitney speedup verdict vs InlineBackend baseline.
    //
    //  Backend          | avg time | vs InlineBackend
    //  ────────────────────────────────────────────────
    //  InlineBackend    |  xxx ms  | baseline
    //  JThreadBackend   |  xxx ms  | Faster/Slower by X%
    //  hetero_executor  |  xxx ms  | Faster/Slower by X%
    //  MonteCarlo       |  xxx ms  | Faster/Slower by X%
    //  MC-GPU           |  xxx ms  | Faster/Slower by X%
    // ────────────────────────────────────────────────────────────────────────────
    static testfw::Result ex17_pi_backend_compare() {
        using namespace pravaha;
        using namespace pravaha::compute;
        using namespace pravaha::expr;

        profiler::ProfileConfig cfg;
        cfg.iterations = static_cast<std::size_t>(kPiIters);
        cfg.warmup_iterations = static_cast<std::size_t>(kPiWarmup);

        // ── 1. InlineBackend -- scalar Riemann ───────────────────────────────────
        cfg.label = "InlineBackend";
        const auto prof_inline = profiler::measure(cfg, [&]() {
            double sum = 0.0;
            for (std::size_t i = 0; i < kPiN; ++i) {
                const double x = (static_cast<double>(i) + 0.5) * kPiH;
                sum += 1.0 / (1.0 + x * x);
            }
            volatile double sink = 4.0 * sum * kPiH;
            (void)sink;
        });

        // ── 2. JThreadBackend -- parallel Riemann ────────────────────────────────
        std::array < double, kPiWorkers > partials{};
        constexpr std::size_t per = kPiN / static_cast<std::size_t>(kPiWorkers);
        auto make_worker = [&](int id) {
            return task(std::string("cmp_jt_") + std::to_string(id), [&, id]() {
                const std::size_t start = static_cast<std::size_t>(id) * per;
                const std::size_t end = (id == kPiWorkers - 1) ? kPiN : start + per;
                double local = 0.0;
                for (std::size_t i = start; i < end; ++i) {
                    const double x = (static_cast<double>(i) + 0.5) * kPiH;
                    local += 1.0 / (1.0 + x * x);
                }
                partials[static_cast<std::size_t>(id)] = local;
            });
        };
        auto jt_par = all_of(make_worker(0), make_worker(1), make_worker(2), make_worker(3));
        auto jt_red = task("cmp_jt_reduce", [&]() {
            double s = 0.0;
            for (const double p : partials) s += p;
            partials[0] = 4.0 * s * kPiH;
        });
        auto jt_pipeline = seq(jt_par, jt_red);

        cfg.label = "JThreadBackend";
        const auto prof_jthread = profiler::measure(cfg, [&]() {
            partials.fill(0.0);
            JThreadBackend backend{kPiWorkers};
            Runner<JThreadBackend> runner{backend};
            if (const auto res = runner.submit(jt_pipeline); !res)
                throw std::runtime_error("cmp jthread failed");
        });

        if (std::abs(partials[0] - kPiRef) > 1e-9)
            return testfw::fail("ex17: JThread Pi comparison value wrong");

        // ── 3. hetero_executor -- SIMD / Metal GPU ───────────────────────────────
        std::vector<float> xs(kPiN);
        constexpr float kHf = 1.0f / static_cast<float>(kPiN);
        for (std::size_t i = 0; i < kPiN; ++i)
            xs[i] = (static_cast<float>(i) + 0.5f) * kHf;

        pravaha::expr::var xv;
        const auto integrand = pravaha::expr::lit(1.0f) / (pravaha::expr::lit(1.0f) + xv * xv);
        const auto r_expr = pravaha::expr::reduce_sum(integrand);

        buffer_descriptor d;
        d.shape = {kPiN};
        d.element_type = data_element_type::f32;
        d.is_unified = true;

        hetero::hetero_executor exec;
        hetero::execution_context ctx;

        cfg.label = "hetero_executor";
        const auto prof_hetero = profiler::measure(cfg, [&]() {
            const auto res = exec.reduce<pravaha::expr::reduce_op::sum>(
                pravaha::expr::reduce_child(r_expr),
                make_const_view(xs.data(), d),
                ctx);
            if (!res) throw std::runtime_error("cmp hetero failed");
            volatile float sink = *res;
            (void)sink;
        });

        // ── 4. Monte Carlo -- CPU-bound stochastic ───────────────────────────────
        std::atomic<std::size_t> inside{0};
        constexpr std::size_t per_mc = kPiN / static_cast<std::size_t>(kPiWorkers);
        auto make_mc = [&](int id) {
            return task(std::string("cmp_mc_") + std::to_string(id), [&, id]() {
                uint64_t state = 0x9e3779b97f4a7c15ULL
                    ^ static_cast<uint64_t>(id * 6364136223846793005LL
                        + 1442695040888963407LL);
                auto next_f64 = [&]() -> double {
                    state ^= state << 13;
                    state ^= state >> 7;
                    state ^= state << 17;
                    return static_cast<double>(state >> 11) / static_cast<double>(1ULL << 53);
                };
                const std::size_t n = (id == kPiWorkers - 1)
                                          ? kPiN - static_cast<std::size_t>(id) * per_mc
                                          : per_mc;
                std::size_t local = 0;
                for (std::size_t i = 0; i < n; ++i) {
                    const double x = next_f64(), y = next_f64();
                    if (x * x + y * y <= 1.0) ++local;
                }
                inside.fetch_add(local, std::memory_order_relaxed);
            });
        };
        auto mc_par = all_of(make_mc(0), make_mc(1), make_mc(2), make_mc(3));

        cfg.label = "MonteCarlo";
        const auto prof_mc = profiler::measure(cfg, [&]() {
            inside.store(0, std::memory_order_relaxed);
            JThreadBackend backend{kPiWorkers};
            Runner<JThreadBackend> runner{backend};
            if (const auto res = runner.submit(mc_par); !res)
                throw std::runtime_error("cmp monte carlo failed");
        });

        // ── 5. MC-GPU -- CPU generation + GPU/SIMD reduce ────────────────────────
        std::vector<float> inside_buf_cmp(kPiN, 0.f);
        constexpr std::size_t per_mc_gpu = kPiN / static_cast<std::size_t>(kPiWorkers);
        auto make_mc_gpu = [&](int id) {
            return task(std::string("cmp_mc_gpu_") + std::to_string(id), [&, id]() {
                uint64_t state = 0xdeadbeefcafe1234ULL
                    ^ static_cast<uint64_t>(id * 6364136223846793005LL
                        + 1442695040888963407LL);
                auto next_f32 = [&]() -> float {
                    state ^= state << 13;
                    state ^= state >> 7;
                    state ^= state << 17;
                    return static_cast<float>(state >> 40) / static_cast<float>(1ULL << 24);
                };
                const std::size_t start = static_cast<std::size_t>(id) * per_mc_gpu;
                const std::size_t end = (id == kPiWorkers - 1) ? kPiN : start + per_mc_gpu;
                for (std::size_t i = start; i < end; ++i) {
                    const float xi2 = next_f32(), yi2 = next_f32();
                    inside_buf_cmp[i] = (xi2 * xi2 + yi2 * yi2 <= 1.0f) ? 1.0f : 0.0f;
                }
            });
        };
        auto mc_gpu_par = all_of(make_mc_gpu(0), make_mc_gpu(1),
                                 make_mc_gpu(2), make_mc_gpu(3));

        pravaha::expr::var xmc;
        const auto mc_id_expr = pravaha::expr::reduce_sum(xmc);
        buffer_descriptor dmc;
        dmc.shape = {kPiN};
        dmc.element_type = data_element_type::f32;
        dmc.is_unified = true;

        cfg.label = "MC-GPU";
        const auto prof_mc_gpu = profiler::measure(cfg, [&]() {
            {
                JThreadBackend backend{kPiWorkers};
                Runner<JThreadBackend> runner{backend};
                if (const auto res = runner.submit(mc_gpu_par); !res)
                    throw std::runtime_error("cmp mc_gpu gen failed");
            }
            const auto res = exec.reduce<pravaha::expr::reduce_op::sum>(
                pravaha::expr::reduce_child(mc_id_expr),
                make_const_view(inside_buf_cmp.data(), dmc),
                ctx);
            if (!res) throw std::runtime_error("cmp mc_gpu reduce failed");
            volatile float sink = *res;
            (void)sink;
        });

        // ── Table ────────────────────────────────────────────────────────────────
        const auto cmp_jt = profiler::compare(prof_inline, prof_jthread);
        const auto cmp_hetero = profiler::compare(prof_inline, prof_hetero);
        const auto cmp_mc = profiler::compare(prof_inline, prof_mc);
        const auto cmp_mc_gpu = profiler::compare(prof_inline, prof_mc_gpu);

        constexpr int kW = 16;
        lg::info("pravaha ex17 (Pi backend compare, N={}, iters={}):", kPiN, kPiIters);
        lg::info("  {:<{}}  {:>10}  {}", "Backend", kW, "avg time", "vs InlineBackend");
        lg::info("  {:-<{}}  {:-<10}  {:-<30}", "", kW, "", "");
        lg::info("  {:<{}}  {:>10}  baseline",
                 "InlineBackend", kW,
                 profiler::internal::format_duration(prof_inline.average_duration));
        lg::info("  {:<{}}  {:>10}  {}",
                 "JThreadBackend", kW,
                 profiler::internal::format_duration(prof_jthread.average_duration),
                 cmp_jt.verdict);
        lg::info("  {:<{}}  {:>10}  {}",
                 "hetero_executor", kW,
                 profiler::internal::format_duration(prof_hetero.average_duration),
                 cmp_hetero.verdict);
        lg::info("  {:<{}}  {:>10}  {}",
                 "MonteCarlo", kW,
                 profiler::internal::format_duration(prof_mc.average_duration),
                 cmp_mc.verdict);
        lg::info("  {:<{}}  {:>10}  {}",
                 "MC-GPU", kW,
                 profiler::internal::format_duration(prof_mc_gpu.average_duration),
                 cmp_mc_gpu.verdict);
        return {};
    }

    // ────────────────────────────────────────────────────────────────────────────
    // Sub-example 18: Tensor symbolic layer — tensor_var, matmul, dispatch_formula
    //
    // Demonstrates: sutra::tensor::extension, tensor_var<>, matmul(), reduce_sum(),
    // dispatch_formula(). Models a seismic feature extractor: weight matrix W (4×8)
    // applied to a raw feature vector x (8), output feature vector y (4), then
    // reduce to scalar energy.
    // ────────────────────────────────────────────────────────────────────────────
    static testfw::Result ex18_tensor_matmul_dispatch() {
        using namespace sutra;
        using namespace sutra::tensor;

        sutra::context ctx;
        ctx.use(sutra::math::extension{});
        ctx.use(sutra::tensor::extension{});

        // Declare symbolic tensors with eager shape registration.
        tensor_var < 2 > W("W_feat", {4, 8}); // weight matrix [4,8]
        tensor_var < 1 > x("x_feat", {8}); // input vector [8]

        formula_ref y = matmul(W, x); // shape → [4]
        formula_ref loss = reduce_sum(y); // shape → scalar

        // Verify shape inference.
        auto y_shape = infer_output_shape(y);
        if (y_shape.shape.size() != 1 || y_shape.shape[0] != 4)
            return testfw::fail("ex18: matmul output shape wrong");

        auto loss_shape = infer_output_shape(loss);
        if (!loss_shape.shape.empty()) {
            // reduce_sum with axis=-1 → scalar (empty shape)
            return testfw::fail("ex18: reduce_sum output shape should be scalar");
        }

        // Concrete execution via dispatch_formula.
        constexpr std::size_t M = 4, K = 8;
        std::vector<float> W_data(M * K), x_data(K), y_data(M, 0.f), scalar_out(1, 0.f);

        // Identity-like weight: row i has 1.0 at col i*K/M, 0 elsewhere.
        for (std::size_t i = 0; i < M; ++i) W_data[i * K + i * 2] = 1.0f;
        for (std::size_t j = 0; j < K; ++j) x_data[j] = static_cast<float>(j + 1);

        pc::buffer_descriptor bd_W;
        bd_W.shape = {M, K};
        bd_W.element_type = pc::data_element_type::f32;

        pc::buffer_descriptor bd_x;
        bd_x.shape = {K};
        bd_x.element_type = pc::data_element_type::f32;

        pc::buffer_descriptor bd_y;
        bd_y.shape = {M};
        bd_y.element_type = pc::data_element_type::f32;

        std::array<engine_binding, 2> srcs{
            engine_binding{bd_W, W_data.data()},
            engine_binding{bd_x, x_data.data()}
        };
        engine_binding dst_y{bd_y, y_data.data()};

        bool ok = dispatch_formula(y, srcs, dst_y, ctx);
        if (!ok)
            return testfw::fail("ex18: dispatch_formula(matmul) failed");

        // Dispatch reduce_sum on y.
        pc::buffer_descriptor bd_sc;
        bd_sc.shape = {};
        bd_sc.element_type = pc::data_element_type::f32;

        std::array<engine_binding, 1> src_y{engine_binding{bd_y, y_data.data()}};
        engine_binding dst_sc{bd_sc, scalar_out.data()};

        bool ok2 = dispatch_formula(loss, src_y, dst_sc, ctx);
        if (!ok2)
            return testfw::fail("ex18: dispatch_formula(reduce_sum) failed");

        // y[i] = W[i,:] · x. W row i has 1 at col i*2, rest 0 → y[i] = x[i*2] = 2i+1.
        float expected_sum = 0.f;
        for (std::size_t i = 0; i < M; ++i) expected_sum += static_cast<float>(i * 2 + 1);
        if (std::abs(scalar_out[0] - expected_sum) > 1e-3f)
            return testfw::fail("ex18: reduce_sum result incorrect");

        lg::info("pravaha ex18 (tensor matmul+reduce): shape W=[4,8] x=[8] y=[4] sum={:.2f}", scalar_out[0]);
        return {};
    }

    // ────────────────────────────────────────────────────────────────────────────
    // Sub-example 19: Tensor element-wise ops — ew_add, ew_mul, ew_exp, ew_log
    //
    // Demonstrates: Path B dispatch (element-wise), shape inference for broadcast,
    // scalar-broadcast for ew_div. Use case: per-element feature normalization.
    // ────────────────────────────────────────────────────────────────────────────
    static testfw::Result ex19_tensor_elementwise() {
        using namespace sutra;
        using namespace sutra::tensor;

        sutra::context ctx;
        ctx.use(sutra::math::extension{});
        ctx.use(sutra::tensor::extension{});

        constexpr std::size_t N = 8;

        tensor_var < 1 > a("a_feat", {N});
        tensor_var < 1 > b("b_feat", {N});

        // Build: c = ew_exp(ew_add(a, b))  then  d = ew_log(c)  → d == a+b
        formula_ref ab = ew_add(a, b);
        formula_ref c = ew_exp(ab);
        formula_ref d = ew_log(c);

        auto ab_shape = infer_output_shape(ab);
        if (ab_shape.shape.size() != 1 || ab_shape.shape[0] != N)
            return testfw::fail("ex19: ew_add shape wrong");

        // Execute: a[i]=i, b[i]=0.5 → ab[i]=i+0.5 → exp(i+0.5) → log → i+0.5
        std::vector<float> a_d(N), b_d(N), ab_d(N, 0.f), c_d(N, 0.f), d_d(N, 0.f);
        for (std::size_t i = 0; i < N; ++i) {
            a_d[i] = static_cast<float>(i);
            b_d[i] = 0.5f;
        }

        pc::buffer_descriptor bd;
        bd.shape = {N};
        bd.element_type = pc::data_element_type::f32;

        std::array<engine_binding, 2> srcs_ab{
            engine_binding{bd, a_d.data()},
            engine_binding{bd, b_d.data()}
        };
        engine_binding dst_ab{bd, ab_d.data()};
        if (!dispatch_formula(ab, srcs_ab, dst_ab, ctx))
            return testfw::fail("ex19: ew_add dispatch failed");

        std::array<engine_binding, 1> srcs_c{engine_binding{bd, ab_d.data()}};
        engine_binding dst_c{bd, c_d.data()};
        if (!dispatch_formula(c, srcs_c, dst_c, ctx))
            return testfw::fail("ex19: ew_exp dispatch failed");

        std::array<engine_binding, 1> srcs_d{engine_binding{bd, c_d.data()}};
        engine_binding dst_d{bd, d_d.data()};
        if (!dispatch_formula(d, srcs_d, dst_d, ctx))
            return testfw::fail("ex19: ew_log dispatch failed");

        // d[i] should recover a[i]+b[i] = i+0.5
        for (std::size_t i = 0; i < N; ++i) {
            float expected = static_cast<float>(i) + 0.5f;
            if (std::abs(d_d[i] - expected) > 1e-4f)
                return testfw::fail("ex19: ew_log(ew_exp(ew_add(a,b))) != a+b");
        }

        lg::info("pravaha ex19 (tensor ew_ops): N={} log(exp(a+b))==a+b verified", N);
        return {};
    }

    // ────────────────────────────────────────────────────────────────────────────
    // Sub-example 20: Softmax pipeline — ew_exp + reduce_sum + ew_div
    //
    // Demonstrates: composing tensor ops into a complete numerically-stable-ish
    // softmax: out[i] = exp(x[i]) / sum(exp(x[j])). Uses dispatch_formula for
    // each stage. Checks output sums to 1.
    // ────────────────────────────────────────────────────────────────────────────
    static testfw::Result ex20_tensor_softmax() {
        using namespace sutra;
        using namespace sutra::tensor;

        sutra::context ctx;
        ctx.use(sutra::math::extension{});
        ctx.use(sutra::tensor::extension{});

        constexpr std::size_t N = 5;

        tensor_var < 1 > x("x_sm", {N});
        formula_ref ex = ew_exp(x);
        formula_ref denom = reduce_sum(ex);
        formula_ref sm = ew_div(ex, denom);

        // Input: simple values.
        std::vector<double> x_d{1.0, 2.0, 3.0, 4.0, 5.0};
        std::vector<double> ex_d(N, 0.0), sm_d(N, 0.0);
        double denom_d{0.0};

        pc::buffer_descriptor bd_N;
        bd_N.shape = {N};
        bd_N.element_type = pc::data_element_type::f64;

        pc::buffer_descriptor bd_sc;
        bd_sc.shape = {};
        bd_sc.element_type = pc::data_element_type::f64;

        // Stage 1: exp(x)
        std::array<engine_binding, 1> s1{engine_binding{bd_N, x_d.data()}};
        if (!dispatch_formula(ex, s1, {bd_N, ex_d.data()}, ctx))
            return testfw::fail("ex20: ew_exp failed");

        // Stage 2: sum(exp(x))
        std::array<engine_binding, 1> s2{engine_binding{bd_N, ex_d.data()}};
        if (!dispatch_formula(denom, s2, {bd_sc, &denom_d}, ctx))
            return testfw::fail("ex20: reduce_sum failed");

        // Stage 3: exp(x) / sum — scalar-broadcast divide
        // ew_div broadcasts scalar denominator across vector numerator.
        pc::buffer_descriptor bd_sc_in;
        bd_sc_in.shape = {1};
        bd_sc_in.element_type = pc::data_element_type::f64;
        double denom_val = denom_d;

        std::array<engine_binding, 2> s3{
            engine_binding{bd_N, ex_d.data()},
            engine_binding{bd_sc_in, &denom_val}
        };
        if (!dispatch_formula(sm, s3, {bd_N, sm_d.data()}, ctx))
            return testfw::fail("ex20: ew_div (softmax) failed");

        // Verify: outputs sum to 1.
        double total = 0.0;
        for (double v : sm_d) total += v;
        if (std::abs(total - 1.0) > 1e-6)
            return testfw::fail("ex20: softmax does not sum to 1");

        // Verify monotone: softmax preserves order.
        for (std::size_t i = 0; i + 1 < N; ++i)
            if (sm_d[i] >= sm_d[i + 1])
                return testfw::fail("ex20: softmax not strictly increasing");

        lg::info("pravaha ex20 (tensor softmax): N={} sum={:.8f} sm[0]={:.6f} sm[4]={:.6f}",
                 N, total, sm_d[0], sm_d[N - 1]);
        return {};
    }

    // ────────────────────────────────────────────────────────────────────────────
    // Sub-example 21: Tensor process-wide shape registry — cross-thread read
    //
    // Demonstrates: the tensor_shape_registry (process-wide, shared_mutex) allows
    // a shape set on thread A to be read concurrently on thread B via the same
    // node_index. This is the actual cross-thread invariant:
    //   - registry is NOT thread-local; get_tensor_shape is safe from any thread
    //   - the node_index is the key; both threads access the same integer key
    //
    // Note: migrate() assigns new node indices so migrated refs require separate
    // shape re-registration (not shown here — that is a known limitation).
    // ────────────────────────────────────────────────────────────────────────────
    static testfw::Result ex21_tensor_cross_thread_registry() {
        using namespace sutra;
        using namespace sutra::tensor;

        // Build a tensor var on this thread (main = "thread A").
        // tensor_var<> registers the shape in the process-wide registry at construction.
        tensor_var < 2 > W("W_reg_test", {5, 7});
        const sutra::formula_ref W_ref = W.ref();
        const sutra::node_index W_idx = W_ref.root;

        bool shape_readable_on_b = false;
        std::string error_msg;

        // Thread B: read the same shape from the process-wide registry.
        // No migration needed — registry is keyed by node_index (a plain integer).
        std::thread t([&]() {
            // Reconstruct a formula_ref pointing at the same node_index in the
            // ephemeral store of thread A (still alive; main thread is blocked on join).
            sutra::formula_ref ref_b{W_idx, W_ref.store};
            const auto* td = sutra::math::get_tensor_shape(ref_b);
            if (td && td->shape.size() == 2 && td->shape[0] == 5 && td->shape[1] == 7) {
                shape_readable_on_b = true;
            }
            else {
                error_msg = td ? "shape dimensions wrong" : "shape not found in registry";
            }
        });
        t.join();

        if (!shape_readable_on_b)
            return testfw::fail("ex21: " + error_msg);

        lg::info("pravaha ex21 (tensor cross-thread registry): shape [5,7] readable from thread B");
        return {};
    }

    // ────────────────────────────────────────────────────────────────────────────
    // Sub-example 22: Axiom adapter — TensorExecutionEngine with AxiomEngineAdapter
    //
    // Demonstrates: AxiomEngineAdapter satisfying TensorExecutionEngine concept,
    // zero-copy make_engine_view, and evaluate() variadic helper.
    // Guarded by SUTRA_HAS_AXIOM — degrades gracefully when Axiom is absent.
    // ────────────────────────────────────────────────────────────────────────────
    static testfw::Result ex22_axiom_adapter() {
#if SUTRA_HAS_AXIOM
        using namespace sutra;
        using namespace sutra::tensor;
        using Adapter = adapters::AxiomEngineAdapter;

        // Concept satisfaction is a compile-time invariant; static_assert guards it.
        static_assert(TensorExecutionEngine<Adapter, axiom::Tensor>,
                      "AxiomEngineAdapter must satisfy TensorExecutionEngine");

        sutra::context ctx;
        ctx.use(sutra::math::extension{});
        ctx.use(sutra::tensor::extension{});

        // Simple ew_add test: a + b = c, all [4] float32 tensors.
        constexpr std::size_t N = 4;
        tensor_var < 1 > a("ax_a", {N}, pc::data_element_type::f32);
        tensor_var < 1 > b("ax_b", {N}, pc::data_element_type::f32);
        formula_ref ab = ew_add(a, b);

        Adapter eng;
        auto at = axiom::Tensor::zeros({N}, axiom::DType::Float32);
        auto bt = axiom::Tensor::zeros({N}, axiom::DType::Float32);

        // Fill: a[i]=i, b[i]=10
        for (std::size_t i = 0; i < N; ++i) {
            at.set_item<float>({i}, static_cast<float>(i));
            bt.set_item<float>({i}, 10.f);
        }

        // evaluate() dispatches through AxiomEngineAdapter::dispatch()
        axiom::Tensor ct = evaluate(eng, ab, at, bt);

        for (std::size_t i = 0; i < N; ++i) {
            float expected = static_cast<float>(i) + 10.f;
            float got = ct.item<float>({i});
            if (std::abs(got - expected) > 1e-4f)
                return testfw::fail("ex22: Axiom ew_add result mismatch");
        }

        // make_engine_view: zero-copy const view from axiom::Tensor
        auto av = make_engine_view<float>(eng, at);
        if (av.desc.shape.empty() || av.desc.shape[0] != N)
            return testfw::fail("ex22: make_engine_view shape wrong");

        lg::info("pravaha ex22 (Axiom adapter): N={} ew_add via AxiomEngineAdapter verified", N);
#else
        lg::info("pravaha ex22 (Axiom adapter): SUTRA_HAS_AXIOM=0 — skipped");
#endif
        return {};
    }
} // namespace seismic_ex

// ============================================================================
// Registry entry
// ============================================================================


// ============================================================================
// Registry entry
// ============================================================================

struct PravahaExample {
    static constexpr std::string_view name() { return "pravaha"; }

    static constexpr std::string_view description() {
        return "Pravaha comprehensive seismic pipeline tutorial: "
            "sequential/parallel/quorum tasks, priorities, sender/receiver, "
            "cancellation, SIMD/GPU eDSL, reductions, multi-input, end-to-end; "
            "Pi backend comparison (InlineBackend/JThreadBackend/hetero_executor/MonteCarlo/MC-GPU) "
            "with profiler::compare table; "
            "Tensor symbolic layer: tensor_var, matmul, reduce_sum, ew_*, dispatch_formula, "
            "softmax pipeline, cross-thread registry, Axiom adapter";
    }

    static constexpr std::array<std::string_view, 6> tag_data{
        "pravaha", "hetero", "simd", "tutorial", "profiler", "tensor"
    };
    static constexpr std::span<const std::string_view> tags() { return tag_data; }

    static testfw::Result run() {
        // Sub-examples run in tutorial order; stop at first failure.
        if (auto r = seismic_ex::ex1_sequential_pipeline(); !r) return r;
        if (auto r = seismic_ex::ex2_parallel_fork_join(); !r) return r;
        if (auto r = seismic_ex::ex3_classification_collect_all(); !r) return r;
        if (auto r = seismic_ex::ex4_quorum_alert(); !r) return r;
        if (auto r = seismic_ex::ex5_task_priorities(); !r) return r;
        if (auto r = seismic_ex::ex6_sender_receiver(); !r) return r;
        if (auto r = seismic_ex::ex7_cancellation(); !r) return r;
        if (auto r = seismic_ex::ex8_hetero_simd_normalize(); !r) return r;
        if (auto r = seismic_ex::ex9_hetero_reduce_rms(); !r) return r;
        if (auto r = seismic_ex::ex10_hetero_multi_input(); !r) return r;
        if (auto r = seismic_ex::ex11_gpu_bulk_normalize(); !r) return r;
        if (auto r = seismic_ex::ex12_end_to_end(); !r) return r;
        if (auto r = seismic_ex::ex13_pi_inline(); !r) return r;
        if (auto r = seismic_ex::ex14_pi_jthread(); !r) return r;
        if (auto r = seismic_ex::ex15_pi_hetero(); !r) return r;
        if (auto r = seismic_ex::ex16_pi_monte_carlo(); !r) return r;
        if (auto r = seismic_ex::ex16b_pi_monte_carlo_gpu(); !r) return r;
        if (auto r = seismic_ex::ex17_pi_backend_compare(); !r) return r;
        if (auto r = seismic_ex::ex18_tensor_matmul_dispatch(); !r) return r;
        if (auto r = seismic_ex::ex19_tensor_elementwise(); !r) return r;
        if (auto r = seismic_ex::ex20_tensor_softmax(); !r) return r;
        if (auto r = seismic_ex::ex21_tensor_cross_thread_registry(); !r) return r;
        if (auto r = seismic_ex::ex22_axiom_adapter(); !r) return r;
        return {};
    }
};

#endif // SRC_EXAMPLES_EXAMPLE_PRAVAHA_HPP
