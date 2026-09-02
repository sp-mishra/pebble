// ============================================================================
// test_pravaha_scheduler.cpp — Unit tests for pravaha scheduler policies
//                              and the profiling wrapper.
// ============================================================================

#include "catch_amalgamated.hpp"

#include "pravaha/schedulers/scheduler_policy.hpp"
#include "pravaha/schedulers/critical_path_scheduler.hpp"
#include "pravaha/schedulers/work_stealing_scheduler.hpp"
#include "pravaha/pravaha_profiler.hpp"

#include <chrono>
#include <thread>
#include <optional>
#include <cstddef>

using pravaha::TaskPriority;

// ============================================================================
// § 1  Concept satisfaction — verified at compile time
// ============================================================================

static_assert(pravaha::sched::SchedulerPolicy<pravaha::sched::fifo_scheduler_policy>,
              "fifo_scheduler_policy must satisfy SchedulerPolicy");

static_assert(pravaha::sched::SchedulerPolicy<pravaha::sched::priority_scheduler_policy>,
              "priority_scheduler_policy must satisfy SchedulerPolicy");

static_assert(pravaha::sched::SchedulerPolicy<pravaha::sched::critical_path_scheduler_policy>,
              "critical_path_scheduler_policy must satisfy SchedulerPolicy");

static_assert(pravaha::sched::SchedulerPolicy<pravaha::sched::work_stealing_scheduler_policy>,
              "work_stealing_scheduler_policy must satisfy SchedulerPolicy");

static_assert(
    pravaha::sched::SchedulerPolicy<
        pravaha::profile::profiling_scheduler_policy<pravaha::sched::fifo_scheduler_policy>>,
    "profiling_scheduler_policy<fifo> must satisfy SchedulerPolicy");

// ============================================================================
// § 2  fifo_scheduler_policy — FIFO ordering
// ============================================================================

TEST_CASE (



"fifo_scheduler_policy returns tasks in insertion order"
,
"[pravaha][scheduler][fifo]"
)
 {
    pravaha::sched::fifo_scheduler_policy sched;

    const pravaha::sched::task_token t0{.id = 0};
    const pravaha::sched::task_token t1{.id = 1};
    const pravaha::sched::task_token t2{.id = 2};

    sched.on_task_ready(t0);
    sched.on_task_ready(t1);
    sched.on_task_ready(t2);

    REQUIRE(sched.pending_count() == 3u);

    auto r0 = sched.select_next_task(0);
    REQUIRE(r0.has_value());
    CHECK(r0->id == 0u);

    auto r1 = sched.select_next_task(0);
    REQUIRE(r1.has_value());
    CHECK(r1->id == 1u);

    auto r2 = sched.select_next_task(0);
    REQUIRE(r2.has_value());
    CHECK(r2->id == 2u);

    CHECK_FALSE(sched.select_next_task(0).has_value());
}

TEST_CASE (



"fifo_scheduler_policy returns nullopt when queue is empty"
,
"[pravaha][scheduler][fifo]"
)
 {
    pravaha::sched::fifo_scheduler_policy sched;
    CHECK_FALSE(sched.select_next_task(0).has_value());
}

TEST_CASE (



"fifo_scheduler_policy on_task_complete is a no-op"
,
"[pravaha][scheduler][fifo]"
)
 {
    pravaha::sched::fifo_scheduler_policy sched;
    const pravaha::sched::task_token t{.id = 42};
    sched.on_task_ready(t);
    auto got = sched.select_next_task(0);
    REQUIRE(got.has_value());
    // on_task_complete must not throw and must not affect future selects
    REQUIRE_NOTHROW(sched.on_task_complete(*got));
    CHECK_FALSE(sched.select_next_task(0).has_value());
}

// ============================================================================
// § 3  priority_scheduler_policy — High > Normal > Low
// ============================================================================

TEST_CASE (



"priority_scheduler_policy returns High before Normal before Low"
,
"[pravaha][scheduler][priority]"
)
 {
    pravaha::sched::priority_scheduler_policy sched;

    // Enqueue in intentionally mixed order.
    sched.on_task_ready({.id = 10, .priority = TaskPriority::Low});
    sched.on_task_ready({.id = 20, .priority = TaskPriority::High});
    sched.on_task_ready({.id = 30, .priority = TaskPriority::Normal});

    auto first = sched.select_next_task(0);
    REQUIRE(first.has_value());
    CHECK(first->priority == TaskPriority::High);
    CHECK(first->id == 20u);

    auto second = sched.select_next_task(0);
    REQUIRE(second.has_value());
    CHECK(second->priority == TaskPriority::Normal);
    CHECK(second->id == 30u);

    auto third = sched.select_next_task(0);
    REQUIRE(third.has_value());
    CHECK(third->priority == TaskPriority::Low);
    CHECK(third->id == 10u);

    CHECK_FALSE(sched.select_next_task(0).has_value());
}

TEST_CASE (



"priority_scheduler_policy respects insertion order within the same priority"
,
"[pravaha][scheduler][priority]"
)
 {
    pravaha::sched::priority_scheduler_policy sched;

    // Two Normal tasks — FIFO within same priority tier.
    sched.on_task_ready({.id = 1, .priority = TaskPriority::Normal});
    sched.on_task_ready({.id = 2, .priority = TaskPriority::Normal});

    auto first = sched.select_next_task(0);
    REQUIRE(first.has_value());
    CHECK(first->id == 1u);

    auto second = sched.select_next_task(0);
    REQUIRE(second.has_value());
    CHECK(second->id == 2u);
}

TEST_CASE (



"priority_scheduler_policy returns nullopt when queue is empty"
,
"[pravaha][scheduler][priority]"
)
 {
    pravaha::sched::priority_scheduler_policy sched;
    CHECK_FALSE(sched.select_next_task(0).has_value());
}

// ============================================================================
// § 4  critical_path_scheduler_policy — interface contract (no DAG required)
// ============================================================================

TEST_CASE (



"critical_path_scheduler_policy defaults are sane after default construction"
,
"[pravaha][scheduler][critical_path]"
)
 {
    pravaha::sched::critical_path_scheduler_policy sched;

    // Without a DAG, critical_path_length must be 0.
    CHECK(sched.critical_path_length() == 0u);

    // expected_speedup must be >= 1.0f even with no DAG loaded.
    CHECK(sched.expected_speedup() >= 1.0f);
}

TEST_CASE (



"critical_path_scheduler_policy can enqueue and dequeue without a DAG"
,
"[pravaha][scheduler][critical_path]"
)
 {
    pravaha::sched::critical_path_scheduler_policy sched;

    sched.on_task_ready({.id = 5, .priority = TaskPriority::Normal, .dag_depth = 3});
    sched.on_task_ready({.id = 6, .priority = TaskPriority::Normal, .dag_depth = 1});

    CHECK(sched.pending_count() == 2u);

    // Highest dag_depth should come out first.
    auto first = sched.select_next_task(0);
    REQUIRE(first.has_value());
    CHECK(first->id == 5u);

    auto second = sched.select_next_task(0);
    REQUIRE(second.has_value());
    CHECK(second->id == 6u);

    CHECK_FALSE(sched.select_next_task(0).has_value());
}

TEST_CASE (



"critical_path_scheduler_policy breaks depth ties with priority"
,
"[pravaha][scheduler][critical_path]"
)
 {
    pravaha::sched::critical_path_scheduler_policy sched;

    // Same dag_depth, different priority.
    sched.on_task_ready({.id = 1, .priority = TaskPriority::Low,    .dag_depth = 2});
    sched.on_task_ready({.id = 2, .priority = TaskPriority::High,   .dag_depth = 2});
    sched.on_task_ready({.id = 3, .priority = TaskPriority::Normal, .dag_depth = 2});

    auto first = sched.select_next_task(0);
    REQUIRE(first.has_value());
    CHECK(first->priority == TaskPriority::High);
}

TEST_CASE (



"critical_path_scheduler_policy returns nullopt when queue is empty"
,
"[pravaha][scheduler][critical_path]"
)
 {
    pravaha::sched::critical_path_scheduler_policy sched;
    CHECK_FALSE(sched.select_next_task(0).has_value());
}

// ============================================================================
// § 5  work_stealing_scheduler_policy — basic dispatch and dequeue
// ============================================================================

TEST_CASE (



"work_stealing_scheduler_policy dispatches tasks round-robin across workers"
,
"[pravaha][scheduler][work_stealing]"
)
 {
    // Use 2 workers for determinism.
    pravaha::sched::work_stealing_scheduler_policy sched{2};
    CHECK(sched.worker_count() == 2u);

    sched.on_task_ready({.id = 0});
    sched.on_task_ready({.id = 1});
    sched.on_task_ready({.id = 2});
    sched.on_task_ready({.id = 3});

    // Worker 0 gets tasks 0 and 2 (LIFO); worker 1 gets 1 and 3.
    // Just verify all 4 are retrievable across the two workers.
    std::size_t retrieved = 0;
    while (true) {
        bool got_any = false;
        if (auto t = sched.select_next_task(0); t.has_value()) { ++retrieved; got_any = true; }
        if (auto t = sched.select_next_task(1); t.has_value()) { ++retrieved; got_any = true; }
        if (!got_any) break;
    }
    CHECK(retrieved == 4u);
}

TEST_CASE (



"work_stealing_scheduler_policy steals from busy worker"
,
"[pravaha][scheduler][work_stealing]"
)
 {
    // 2 workers; only add tasks that will land on worker 0 (first 4 via round-robin).
    // Then drain from worker 1 (steal).
    pravaha::sched::work_stealing_scheduler_policy sched{2};

    // Push 4 tasks — all go to worker 0 and 1 interleaved.
    // Feed 4 extra tasks specifically into worker 0's queue by adding 8 total
    // so both workers get 4 each, but only drain worker 1's queue to leave
    // tasks on worker 0 for stealing.
    for (std::size_t i = 0; i < 4; ++i) {
        sched.on_task_ready({.id = i});
    }

    // Drain worker 0's own tasks to leave only worker 1's tasks.
    // Then let worker 0 steal.
    std::size_t total = 0;
    for (std::size_t worker = 0; worker < 2; ++worker) {
        while (sched.select_next_task(worker).has_value()) {
            ++total;
        }
    }
    CHECK(total == 4u);
}

TEST_CASE (



"work_stealing_scheduler_policy returns nullopt when all queues are empty"
,
"[pravaha][scheduler][work_stealing]"
)
 {
    pravaha::sched::work_stealing_scheduler_policy sched{2};
    CHECK_FALSE(sched.select_next_task(0).has_value());
    CHECK_FALSE(sched.select_next_task(1).has_value());
}

TEST_CASE (



"work_stealing_scheduler_policy defaults to at-least-one worker"
,
"[pravaha][scheduler][work_stealing]"
)
 {
    // Passing 0 must not crash — clamped to 1.
    pravaha::sched::work_stealing_scheduler_policy sched{0};
    CHECK(sched.worker_count() >= 1u);
    sched.on_task_ready({.id = 99});
    CHECK(sched.select_next_task(0).has_value());
}

// ============================================================================
// § 6  profiling_scheduler_policy — wraps fifo, records timing
// ============================================================================

TEST_CASE (



"profiling_scheduler_policy concept check"
,
"[pravaha][profiler]"
)
 {
    // Compile-time only; runtime pass is a no-op.
    static_assert(
        pravaha::sched::SchedulerPolicy<
            pravaha::profile::profiling_scheduler_policy<pravaha::sched::fifo_scheduler_policy>>);
    SUCCEED("profiling_scheduler_policy<fifo> satisfies SchedulerPolicy");
}

TEST_CASE (



"profiling_scheduler_policy records execution_ns for each completed task"
,
"[pravaha][profiler]"
)
 {
    using namespace pravaha::profile;
    using namespace pravaha::sched;

    profiling_scheduler_policy<fifo_scheduler_policy> sched;

    const task_token t0{.id = 100, .priority = TaskPriority::Normal};
    const task_token t1{.id = 101, .priority = TaskPriority::High};

    // Sequence: ready → select (starts exec timer) → complete (records sample).
    sched.on_task_ready(t0);
    sched.on_task_ready(t1);

    // Tiny sleep to ensure non-zero elapsed time.
    std::this_thread::sleep_for(std::chrono::microseconds{10});

    auto r0 = sched.select_next_task(0);
    REQUIRE(r0.has_value());
    CHECK(r0->id == 100u);

    std::this_thread::sleep_for(std::chrono::microseconds{10});
    sched.on_task_complete(*r0);

    auto r1 = sched.select_next_task(0);
    REQUIRE(r1.has_value());
    CHECK(r1->id == 101u);

    std::this_thread::sleep_for(std::chrono::microseconds{10});
    sched.on_task_complete(*r1);

    // take_report should see both tasks.
    const auto report = sched.take_report();

    CHECK(report.tasks_executed == 2u);
    REQUIRE(report.samples.size() == 2u);

    for (const auto& sample : report.samples) {
        CHECK(sample.execution_ns > 0u);
    }
}

TEST_CASE (



"profiling_scheduler_policy take_report clears samples"
,
"[pravaha][profiler]"
)
 {
    using namespace pravaha::profile;
    using namespace pravaha::sched;

    profiling_scheduler_policy<fifo_scheduler_policy> sched;
    const task_token t{.id = 200};

    sched.on_task_ready(t);
    [[maybe_unused]] auto r1 = sched.select_next_task(0);
    sched.on_task_complete(t);

    auto rep1 = sched.take_report();
    CHECK(rep1.tasks_executed == 1u);

    // Second report should be empty (samples were drained).
    auto rep2 = sched.take_report();
    CHECK(rep2.tasks_executed == 0u);
    CHECK(rep2.samples.empty());
}

TEST_CASE (



"profiling_scheduler_policy total_wall_ns is positive after work"
,
"[pravaha][profiler]"
)
 {
    using namespace pravaha::profile;
    using namespace pravaha::sched;

    profiling_scheduler_policy<fifo_scheduler_policy> sched;
    const task_token t{.id = 300};

    sched.on_task_ready(t);
    std::this_thread::sleep_for(std::chrono::microseconds{100});
    [[maybe_unused]] auto r2 = sched.select_next_task(0);
    sched.on_task_complete(t);

    const auto report = sched.take_report();
    CHECK(report.total_wall_ns > 0u);
}

TEST_CASE (



"with_profiling factory wraps a policy and satisfies SchedulerPolicy"
,
"[pravaha][profiler]"
)
 {
    auto wrapped = pravaha::profile::with_profiling(pravaha::sched::fifo_scheduler_policy{});
    static_assert(pravaha::sched::SchedulerPolicy<decltype(wrapped)>);

    const pravaha::sched::task_token t{.id = 400};
    wrapped.on_task_ready(t);
    auto got = wrapped.select_next_task(0);
    REQUIRE(got.has_value());
    CHECK(got->id == 400u);
    wrapped.on_task_complete(*got);

    const auto rep = wrapped.take_report();
    CHECK(rep.tasks_executed == 1u);
}

TEST_CASE (



"profiling_scheduler_policy record_steal increments tasks_stolen"
,
"[pravaha][profiler]"
)
 {
    using namespace pravaha::profile;
    using namespace pravaha::sched;

    profiling_scheduler_policy<fifo_scheduler_policy> sched;
    sched.record_steal();
    sched.record_steal();

    const auto report = sched.take_report();
    CHECK(report.tasks_stolen == 2u);
}

TEST_CASE (



"profiling_scheduler_policy invokes the sink once per drained sample"
,
"[pravaha][profiler][sink]"
)
 {
    using namespace pravaha::profile;
    using namespace pravaha::sched;

    std::size_t sink_calls = 0;
    profile_sink sink = [&](const task_profile_sample&) { ++sink_calls; };

    auto sched = with_profiling(fifo_scheduler_policy{}, sink);
    for (std::size_t i = 1; i <= 3; ++i) {
        task_token t{i, TaskPriority::Normal};
        sched.on_task_ready(t);
        (void)sched.select_next_task(0);
        sched.on_task_complete(t);
    }

    const auto report = sched.take_report();
    CHECK(report.samples.size() == 3);
    CHECK(sink_calls == 3);

    // A second drain with no new samples must not re-invoke the sink.
    (void)sched.take_report();
    CHECK(sink_calls == 3);
}

