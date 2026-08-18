#include "catch_amalgamated.hpp"
#include "observability/sinks/profiler_sink.hpp"

#include <thread>
#include <vector>

using namespace utils::nadi;

// ---------------------------------------------------------------------------
// Shared sink types
// ---------------------------------------------------------------------------

using GraphSink = ProfilerSink<"Graph">;
using DbSink = ProfilerSink<"DB", 8>; // tiny MaxSamples for overflow tests
using OtherSink = ProfilerSink<"Other">;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

using GraphPulse = Pulse<"Graph", Field<"nodes", int>>;
using DBPulse = Pulse<"DB", Field<"rows", int>>;
using OtherPulse = Pulse<"Other">;

// Emit a matched Begin/End pair into GraphSink directly.
static void push_graph_sample(std::uint64_t id, std::uint64_t begin_ns, std::uint64_t end_ns) {
    GraphSink::emit(GraphPulse{
        .id = {id},
        .phase = PulsePhase::Begin,
        .timestamp_ns = begin_ns,
        .payload = {Field < "nodes", int>{1}},
    });
    GraphSink::emit(GraphPulse{
        .id = {id},
        .phase = PulsePhase::End,
        .timestamp_ns = end_ns,
        .payload = {Field < "nodes", int>{1}},
    });
}

// ============================================================================
// SinkPolicy conformance
// ============================================================================

TEST_CASE (



"ProfilerSink satisfies SinkPolicy"
,
"[nadi][profiler][policy]"
)
 {
    STATIC_REQUIRE(SinkPolicy<GraphSink>);
    STATIC_REQUIRE(SinkPolicy<DbSink>);
}

TEST_CASE (



"ProfilerSink: enabled=true, DropNewest flow_control"
,
"[nadi][profiler][policy]"
)
 {
    STATIC_REQUIRE(GraphSink::enabled == true);
    STATIC_REQUIRE(std::same_as<
        std::remove_cv_t<decltype(GraphSink::flow_control)>, DropNewest>);
}

// ============================================================================
// Category filter
// ============================================================================

TEST_CASE (



"ProfilerSink: compile-time category filter ignores non-matching pulses"
,
"[nadi][profiler][filter]"
)
 {
    GraphSink::reset();

    // Emit a DB pulse into GraphSink — must be silently ignored.
    GraphSink::emit(DBPulse{.id = {99}, .phase = PulsePhase::Begin,
                            .timestamp_ns = 100,
                            .payload = {Field<"rows", int>{5}}});
    GraphSink::emit(DBPulse{.id = {99}, .phase = PulsePhase::End,
                            .timestamp_ns = 200,
                            .payload = {Field<"rows", int>{5}}});

    auto res = GraphSink::build_result();
    REQUIRE(res.iterations_succeeded == 0);
}

TEST_CASE (



"ProfilerSink: compile-time category filter accepts matching pulses"
,
"[nadi][profiler][filter]"
)
 {
    GraphSink::reset();

    // Emit matching GraphPulse into GraphSink — must be recorded.
    push_graph_sample(1, 0, 100);

    auto res = GraphSink::build_result();
    REQUIRE(res.iterations_succeeded == 1);
}

// ============================================================================
// Basic sampling
// ============================================================================

TEST_CASE (



"ProfilerSink: records correct duration for a single Begin/End pair"
,
"[nadi][profiler][sampling]"
)
 {
    GraphSink::reset();
    push_graph_sample(1, 1000, 1500); // 500 ns

    auto res = GraphSink::build_result("test");
    REQUIRE(res.iterations_succeeded == 1);
    REQUIRE(res.individual_runs.size() == 1);
    REQUIRE(res.individual_runs[0].count() == 500);
    REQUIRE(res.min_duration.count() == 500);
    REQUIRE(res.max_duration.count() == 500);
    REQUIRE(res.total_duration.count() == 500);
}

TEST_CASE (



"ProfilerSink: accumulates multiple samples correctly"
,
"[nadi][profiler][sampling]"
)
 {
    GraphSink::reset();
    push_graph_sample(1, 0,   100);   // 100 ns
    push_graph_sample(2, 200, 500);   // 300 ns
    push_graph_sample(3, 600, 1600);  // 1000 ns

    auto res = GraphSink::build_result();
    REQUIRE(res.iterations_succeeded == 3);
    REQUIRE(res.min_duration.count() == 100);
    REQUIRE(res.max_duration.count() == 1000);
    REQUIRE(res.total_duration.count() == 1400);
    REQUIRE(res.average_duration.count() == 1400 / 3);
}

// ============================================================================
// Nesting / stack matching
// ============================================================================

TEST_CASE (



"ProfilerSink: correctly matches nested Begin/End pairs by EventId"
,
"[nadi][profiler][nesting]"
)
 {
    GraphSink::reset();

    // Outer: id=1, inner: id=2, both "Graph" category.
    GraphSink::emit(GraphPulse{.id={1}, .phase=PulsePhase::Begin,  .timestamp_ns=0});
    GraphSink::emit(GraphPulse{.id={2}, .phase=PulsePhase::Begin,  .timestamp_ns=10});
    GraphSink::emit(GraphPulse{.id={2}, .phase=PulsePhase::End,    .timestamp_ns=50});  // inner: 40
    GraphSink::emit(GraphPulse{.id={1}, .phase=PulsePhase::End,    .timestamp_ns=100}); // outer: 100

    auto res = GraphSink::build_result();
    REQUIRE(res.iterations_succeeded == 2);

    // Both durations must be present; order of storage may vary.
    std::vector<std::int64_t> durations;
    for (auto& d : res.individual_runs)
        durations.push_back(d.count());
    std::sort(durations.begin(), durations.end());

    REQUIRE(durations[0] == 40);
    REQUIRE(durations[1] == 100);
}

// ============================================================================
// DropNewest overflow
// ============================================================================

TEST_CASE (



"ProfilerSink: drops samples beyond MaxSamples (DropNewest)"
,
"[nadi][profiler][overflow]"
)
 {
    DbSink::reset(); // MaxSamples = 8

    for (std::uint64_t i = 0; i < 20; ++i) {
        DbSink::emit(DBPulse{.id={i}, .phase=PulsePhase::Begin,  .timestamp_ns=i*100});
        DbSink::emit(DBPulse{.id={i}, .phase=PulsePhase::End,    .timestamp_ns=i*100+50});
    }

    auto res = DbSink::build_result();
    REQUIRE(res.iterations_succeeded == 8); // capped at MaxSamples
}

// ============================================================================
// reset()
// ============================================================================

TEST_CASE (



"ProfilerSink: reset clears all samples"
,
"[nadi][profiler][reset]"
)
 {
    GraphSink::reset();
    push_graph_sample(1, 0, 100);
    push_graph_sample(2, 0, 200);

    GraphSink::reset();
    auto res = GraphSink::build_result();
    REQUIRE(res.iterations_succeeded == 0);
    REQUIRE(res.individual_runs.empty());
}

TEST_CASE (



"ProfilerSink: re-emission after reset starts fresh"
,
"[nadi][profiler][reset]"
)
 {
    GraphSink::reset();
    push_graph_sample(1, 0, 100);
    GraphSink::reset();
    push_graph_sample(2, 0, 300);

    auto res = GraphSink::build_result();
    REQUIRE(res.iterations_succeeded == 1);
    REQUIRE(res.individual_runs[0].count() == 300);
}

// ============================================================================
// label
// ============================================================================

TEST_CASE (



"ProfilerSink: build_result uses category name as default label"
,
"[nadi][profiler][label]"
)
 {
    GraphSink::reset();
    auto res = GraphSink::build_result();
    REQUIRE(res.label == "Graph");
}

TEST_CASE (



"ProfilerSink: build_result respects explicit label override"
,
"[nadi][profiler][label]"
)
 {
    GraphSink::reset();
    auto res = GraphSink::build_result("LiteGraph TopoSort");
    REQUIRE(res.label == "LiteGraph TopoSort");
}

// ============================================================================
// PulseScope integration
// ============================================================================

TEST_CASE (



"ProfilerSink: works end-to-end with PulseScope"
,
"[nadi][profiler][scope]"
)
 {
    GraphSink::reset();
    restore_lineage({});

    {
        PulseScope<GraphSink, "Graph", Field<"nodes", int>> scope{
            Field<"nodes", int>{512}};
        volatile int x = 0;
        for (int i = 0; i < 10000; ++i) x += i;
        (void)x;
    }

    auto res = GraphSink::build_result("scope_test");
    REQUIRE(res.iterations_succeeded == 1);
    REQUIRE(res.individual_runs[0].count() > 0);
}

// ============================================================================
// Concurrent writers
// ============================================================================

TEST_CASE (



"ProfilerSink: concurrent emitters do not corrupt sample count"
,
"[nadi][profiler][threading]"
)
 {
    GraphSink::reset();

    constexpr int N_THREADS = 4;
    constexpr int PER_THREAD = 25;

    std::vector<std::thread> threads;
    threads.reserve(N_THREADS);
    for (int t = 0; t < N_THREADS; ++t) {
        threads.emplace_back([t] {
            for (int i = 0; i < PER_THREAD; ++i) {
                const std::uint64_t id = static_cast<std::uint64_t>(t * 1000 + i);
                GraphSink::emit(GraphPulse{
                    .id={id}, .phase=PulsePhase::Begin, .timestamp_ns=0});
                GraphSink::emit(GraphPulse{
                    .id={id}, .phase=PulsePhase::End,   .timestamp_ns=100});
            }
        });
    }
    for (auto& th : threads) th.join();

    auto res = GraphSink::build_result();
    REQUIRE(res.iterations_succeeded == N_THREADS * PER_THREAD);
}
