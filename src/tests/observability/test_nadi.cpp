#include "catch_amalgamated.hpp"
#include "observability/nadi.hpp"
#include "observability/sinks/ring_buffer_sink.hpp"
#include "observability/sinks/chrome_trace_sink.hpp"
#include "observability/sinks/thread_local_sink.hpp"

#include <atomic>
#include <sstream>
#include <thread>
#include <type_traits>
#include <vector>

using namespace utils::nadi;

// ============================================================================
// Shared types
// ============================================================================

// data-plane tests use a two-field KV pulse
using KVStorePulse = Pulse<"KV_Store", Field < "cache_hits", int>
,
Field<"duration_ns", double>
>;

// sink tests use simpler single/dual-field pulses
using DBPulse = Pulse<"DB", Field<"rows", int>>;
using KVPulse = Pulse<"KV", Field < "hits", int>
,
Field<"latency_ns", double>
>;

// RingBufferSink is now type-erased: template params are <Capacity, ArgsCapacity>
using DBRing = RingBufferSink<4>;
using KVRing = RingBufferSink<8>;

// ============================================================================
// Egress helpers  (file-scope — local classes cannot have static members)
// ============================================================================

struct TestSink {
    static constexpr bool enabled = true;
    static constexpr Lossless flow_control = {};

    inline static std::atomic<int> emit_count = 0;

    static void emit(const auto& /*pulse*/) noexcept { ++emit_count; }
};

struct OverwriteSink {
    static constexpr bool enabled = true;
    static constexpr OverwriteOldest flow_control = {};

    static void emit(const auto& /*pulse*/) noexcept {}
};

struct SinkMissingEmit {
    static constexpr bool enabled = true;
    static constexpr DropNewest flow_control = {};
};

struct SinkUnknownFlow {
    static constexpr bool enabled = true;

    struct Mystery {};

    static constexpr Mystery flow_control = {};

    static void emit(const auto&) noexcept {}
};

// ============================================================================
// Ingress helpers
// ============================================================================

struct SinkRecord {
    std::uint64_t id = 0;
    PulsePhase phase = PulsePhase::Begin;
};

struct CaptureSink {
    static constexpr bool enabled = true;
    static constexpr Lossless flow_control = {};

    inline static SinkRecord calls[256]{};
    inline static int count = 0;

    static void emit(const auto& pulse) noexcept {
        if (count < 256)
            calls[count++] = {pulse.id.value, pulse.phase};
    }

    static void reset() noexcept { count = 0; }
};

// ============================================================================
// 1. Data Plane — FixedString NTTP
// ============================================================================

TEST_CASE (



"FixedString: compile-time construction and string_view"
,
"[nadi][fixedstring]"
)
 {
    constexpr FixedString fs{"KV_Store"};
    STATIC_REQUIRE(fs.view().size() == 8);
    REQUIRE(fs.view() == "KV_Store");
}

TEST_CASE (



"FixedString: distinct strings are not equal"
,
"[nadi][fixedstring]"
)
 {
    constexpr FixedString a{"alpha"};
    constexpr FixedString b{"beta_"};
    STATIC_REQUIRE(!(a == b));
}

TEST_CASE (



"FixedString: empty string has zero-length view"
,
"[nadi][fixedstring]"
)
 {
    constexpr FixedString empty{""};
    STATIC_REQUIRE(empty.view().empty());
}

// ============================================================================
// 2. Data Plane — Pulse layout & type traits
// ============================================================================

TEST_CASE (



"Pulse: is trivially destructible"
,
"[nadi][pulse]"
)
 {
    STATIC_REQUIRE(std::is_trivially_destructible_v<KVStorePulse>);
}

TEST_CASE (



"Pulse: size is non-zero (sanity check)"
,
"[nadi][pulse]"
)
 {
    STATIC_REQUIRE(!std::is_empty_v<KVStorePulse>);
}

TEST_CASE (



"Pulse: category NTTP matches expected string"
,
"[nadi][pulse]"
)
 {
    STATIC_REQUIRE(KVStorePulse::category.view().size() == 8);
    REQUIRE(KVStorePulse::category.view() == "KV_Store");
}

TEST_CASE (



"Pulse: Field name NTTPs are accessible via tuple element type"
,
"[nadi][pulse]"
)
 {
    using CacheField    = std::tuple_element_t<0, decltype(KVStorePulse::payload)>;
    using DurationField = std::tuple_element_t<1, decltype(KVStorePulse::payload)>;

    STATIC_REQUIRE(CacheField::name.view().size()    == 10);
    STATIC_REQUIRE(DurationField::name.view().size() == 11);
    REQUIRE(CacheField::name.view()    == "cache_hits");
    REQUIRE(DurationField::name.view() == "duration_ns");
}

TEST_CASE (



"Pulse: default-constructed payload holds zero values"
,
"[nadi][pulse]"
)
 {
    KVStorePulse p{};
    REQUIRE(p.id.value     == 0);
    REQUIRE(p.timestamp_ns == 0);
    REQUIRE(p.phase        == PulsePhase::Begin);
    REQUIRE(std::get<0>(p.payload).value == 0);
    REQUIRE(std::get<1>(p.payload).value == 0.0);
}

TEST_CASE (



"Pulse: fields can be set without heap allocation"
,
"[nadi][pulse]"
)
 {
    KVStorePulse p{
        .id           = {42},
        .phase        = PulsePhase::End,
        .timestamp_ns = 1'000'000,
        .payload      = {Field<"cache_hits",  int>{99},
                         Field<"duration_ns", double>{3.14}},
    };
    REQUIRE(p.id.value     == 42);
    REQUIRE(p.phase        == PulsePhase::End);
    REQUIRE(p.timestamp_ns == 1'000'000);
    REQUIRE(std::get<0>(p.payload).value == 99);
    REQUIRE(std::get<1>(p.payload).value == Catch::Approx(3.14));
}

// ============================================================================
// 3. Egress Plane — SinkPolicy concept
// ============================================================================

TEST_CASE (



"SinkPolicy: NoSink satisfies the concept"
,
"[nadi][egress][concept]"
)
 {
    STATIC_REQUIRE(SinkPolicy<NoSink>);
}

TEST_CASE (



"SinkPolicy: TestSink (Lossless) satisfies the concept"
,
"[nadi][egress][concept]"
)
 {
    STATIC_REQUIRE(SinkPolicy<TestSink>);
}

TEST_CASE (



"SinkPolicy: OverwriteSink satisfies the concept"
,
"[nadi][egress][concept]"
)
 {
    STATIC_REQUIRE(SinkPolicy<OverwriteSink>);
}

TEST_CASE (



"SinkPolicy: a type with enabled+flow_control but no emit satisfies SinkPolicy"
,
"[nadi][egress][concept]"
)
 {
    // SinkPolicy now checks only enabled + flow_control.
    // emit() is checked separately via SinkFor<Sink, PulseType>.
    STATIC_REQUIRE(SinkPolicy<SinkMissingEmit>);
    // But it cannot receive a concrete pulse type.
    STATIC_REQUIRE_FALSE((SinkFor<SinkMissingEmit, Pulse<"test">>));
}

TEST_CASE (



"SinkPolicy: a type with unknown flow_control does not satisfy the concept"
,
"[nadi][egress][concept]"
)
 {
    STATIC_REQUIRE_FALSE(SinkPolicy<SinkUnknownFlow>);
}

// ============================================================================
// 4. Egress Plane — NoSink & MultiSink
// ============================================================================

TEST_CASE (



"NoSink: enabled is false"
,
"[nadi][egress][nosink]"
)
 {
    STATIC_REQUIRE(NoSink::enabled == false);
}

TEST_CASE (



"NoSink: emit is a no-op"
,
"[nadi][egress][nosink]"
)
 {
    Pulse<"KV_Store", Field<"hits", int>> p{};
    REQUIRE_NOTHROW(NoSink::emit(p));
}

TEST_CASE (



"MultiSink<NoSink, NoSink>::enabled is false"
,
"[nadi][egress][multisink]"
)
 {
    STATIC_REQUIRE(MultiSink<NoSink, NoSink>::enabled == false);
}

TEST_CASE (



"MultiSink<NoSink, TestSink>::enabled is true"
,
"[nadi][egress][multisink]"
)
 {
    STATIC_REQUIRE(MultiSink<NoSink, TestSink>::enabled == true);
}

TEST_CASE (



"MultiSink<TestSink>::enabled is true (single enabled sink)"
,
"[nadi][egress][multisink]"
)
 {
    STATIC_REQUIRE(MultiSink<TestSink>::enabled == true);
}

TEST_CASE (



"MultiSink::emit only calls enabled sinks"
,
"[nadi][egress][multisink]"
)
 {
    Pulse<"KV_Store", Field<"hits", int>> p{};
    TestSink::emit_count = 0;

    MultiSink<NoSink, TestSink>::emit(p);
    REQUIRE(TestSink::emit_count == 1);

    MultiSink<NoSink, NoSink>::emit(p);
    REQUIRE(TestSink::emit_count == 1);

    MultiSink<TestSink, TestSink>::emit(p);
    REQUIRE(TestSink::emit_count == 3);
}

TEST_CASE (



"MultiSink satisfies SinkPolicy itself"
,
"[nadi][egress][multisink]"
)
 {
    STATIC_REQUIRE(SinkPolicy<MultiSink<NoSink, TestSink>>);
}

// ============================================================================
// 5. Lineage — capture / restore
// ============================================================================

TEST_CASE (



"capture_lineage: returns zero-valued token on a fresh thread"
,
"[nadi][lineage]"
)
 {
    restore_lineage({});
    auto tok = capture_lineage();
    REQUIRE(tok.trace_id.value  == 0);
    REQUIRE(tok.parent_id.value == 0);
}

TEST_CASE (



"restore_lineage: sets the thread-local state"
,
"[nadi][lineage]"
)
 {
    restore_lineage({});
    restore_lineage({{42}, {7}});
    auto got = capture_lineage();
    REQUIRE(got.trace_id.value  == 42);
    REQUIRE(got.parent_id.value == 7);
    restore_lineage({});
}

// ============================================================================
// 6. Lineage — ScopedLineage RAII
// ============================================================================

TEST_CASE (



"ScopedLineage: applies token on construction"
,
"[nadi][lineage][scoped]"
)
 {
    restore_lineage({});
    {
        ScopedLineage scope{{{100}, {200}}};
        REQUIRE(capture_lineage().trace_id.value  == 100);
        REQUIRE(capture_lineage().parent_id.value == 200);
    }
}

TEST_CASE (



"ScopedLineage: restores previous token on destruction"
,
"[nadi][lineage][scoped]"
)
 {
    restore_lineage({{1}, {2}});
    {
        ScopedLineage scope{{{99}, {88}}};
        REQUIRE(capture_lineage().trace_id.value == 99);
    }
    REQUIRE(capture_lineage().trace_id.value  == 1);
    REQUIRE(capture_lineage().parent_id.value == 2);
    restore_lineage({});
}

TEST_CASE (



"ScopedLineage: nesting restores each level correctly"
,
"[nadi][lineage][scoped]"
)
 {
    restore_lineage({});
    {
        ScopedLineage s1{{{10}, {0}}};
        REQUIRE(capture_lineage().trace_id.value == 10);
        {
            ScopedLineage s2{{{20}, {10}}};
            REQUIRE(capture_lineage().trace_id.value == 20);
        }
        REQUIRE(capture_lineage().trace_id.value == 10);
    }
    REQUIRE(capture_lineage().trace_id.value == 0);
}

TEST_CASE (



"ScopedLineage: thread-local state is isolated between threads"
,
"[nadi][lineage][threading]"
)
 {
    restore_lineage({{42}, {0}});
    std::uint64_t child_value = 99;
    std::thread t([&] { child_value = capture_lineage().trace_id.value; });
    t.join();
    REQUIRE(child_value == 0);
    REQUIRE(capture_lineage().trace_id.value == 42);
    restore_lineage({});
}

// ============================================================================
// 7. Ingress Plane — route_pulse & utilities
// ============================================================================

TEST_CASE (



"route_pulse: disabled sink is a no-op"
,
"[nadi][ingress][router]"
)
 {
    Pulse<"T"> p{};
    route_pulse<NoSink>(p);
    SUCCEED("NoSink route compiled and ran without side effects");
}

TEST_CASE (



"route_pulse: enabled sink receives the pulse"
,
"[nadi][ingress][router]"
)
 {
    CaptureSink::reset();
    Pulse<"T"> p{.id = {77}, .phase = PulsePhase::Instant};
    route_pulse<CaptureSink>(p);
    REQUIRE(CaptureSink::count == 1);
    REQUIRE(CaptureSink::calls[0].id    == 77);
    REQUIRE(CaptureSink::calls[0].phase == PulsePhase::Instant);
}

TEST_CASE (



"now_ns: returns non-zero, monotonically non-decreasing values"
,
"[nadi][ingress][time]"
)
 {
    auto t1 = now_ns();
    auto t2 = now_ns();
    REQUIRE(t1 > 0);
    REQUIRE(t2 >= t1);
}

TEST_CASE (



"generate_event_id: returns strictly increasing IDs"
,
"[nadi][ingress][id]"
)
 {
    auto a = generate_event_id();
    auto b = generate_event_id();
    auto c = generate_event_id();
    REQUIRE(b.value == a.value + 1);
    REQUIRE(c.value == b.value + 1);
}

// ============================================================================
// 8. Ingress Plane — PulseScope
// ============================================================================

TEST_CASE (



"PulseScope<NoSink>: constructs and destructs without side effects"
,
"[nadi][ingress][scope]"
)
 {
    restore_lineage({});
    CaptureSink::reset();
    { PulseScope<NoSink, "Test"> scope{}; }
    REQUIRE(CaptureSink::count == 0);
}

TEST_CASE (



"PulseScope<CaptureSink>: emits Begin on construction and End on destruction"
,
"[nadi][ingress][scope]"
)
 {
    CaptureSink::reset();
    restore_lineage({});
    {
        PulseScope<CaptureSink, "Op"> scope{};
        REQUIRE(CaptureSink::count == 1);
        REQUIRE(CaptureSink::calls[0].phase == PulsePhase::Begin);
    }
    REQUIRE(CaptureSink::count == 2);
    REQUIRE(CaptureSink::calls[1].phase == PulsePhase::End);
}

TEST_CASE (



"PulseScope: Begin and End carry the same EventId"
,
"[nadi][ingress][scope]"
)
 {
    CaptureSink::reset();
    restore_lineage({});
    { PulseScope<CaptureSink, "Op"> scope{}; }
    REQUIRE(CaptureSink::calls[0].id == CaptureSink::calls[1].id);
    REQUIRE(CaptureSink::calls[0].id != 0);
}

TEST_CASE (



"PulseScope: updates thread-local lineage while active and restores on exit"
,
"[nadi][ingress][scope][lineage]"
)
 {
    restore_lineage({});
    std::uint64_t inner_trace  = 0;
    std::uint64_t inner_parent = 0;
    {
        PulseScope<NoSink, "Outer"> outer{};
        inner_trace  = capture_lineage().trace_id.value;
        inner_parent = capture_lineage().parent_id.value;
    }
    REQUIRE(inner_trace  != 0);
    REQUIRE(inner_parent == 0);
    REQUIRE(capture_lineage().trace_id.value == 0);
}

TEST_CASE (



"PulseScope: nested scopes form a correct parent-child chain"
,
"[nadi][ingress][scope][lineage]"
)
 {
    restore_lineage({});
    std::uint64_t outer_id = 0, inner_id = 0, inner_parent = 0;
    {
        PulseScope<NoSink, "Outer"> outer{};
        outer_id = capture_lineage().trace_id.value;
        {
            PulseScope<NoSink, "Inner"> inner{};
            inner_id     = capture_lineage().trace_id.value;
            inner_parent = capture_lineage().parent_id.value;
        }
        REQUIRE(capture_lineage().trace_id.value == outer_id);
    }
    REQUIRE(inner_id     != outer_id);
    REQUIRE(inner_parent == outer_id);
    REQUIRE(capture_lineage().trace_id.value == 0);
}

TEST_CASE (



"PulseScope: accepts user-supplied Fields in payload"
,
"[nadi][ingress][scope]"
)
 {
    CaptureSink::reset();
    restore_lineage({});
    {
        PulseScope<CaptureSink, "KV",
                   Field<"cache_hits", int>,
                   Field<"latency_ns", double>>
            scope{Field<"cache_hits", int>{42}, Field<"latency_ns", double>{1.5}};
    }
    REQUIRE(CaptureSink::count == 2);
}

// ============================================================================
// 9. Sinks — RingBufferSink
// ============================================================================

TEST_CASE (



"RingBufferSink satisfies SinkPolicy"
,
"[nadi][sink][ring]"
)
 {
    STATIC_REQUIRE(SinkPolicy<DBRing>);
    STATIC_REQUIRE(SinkPolicy<KVRing>);
}

TEST_CASE (



"RingBufferSink: enabled and OverwriteOldest flow_control"
,
"[nadi][sink][ring]"
)
 {
    STATIC_REQUIRE(DBRing::enabled == true);
    STATIC_REQUIRE(std::same_as<
        std::remove_cv_t<decltype(DBRing::flow_control)>, OverwriteOldest>);
}

TEST_CASE (



"RingBufferSink: empty buffer returns nullopt"
,
"[nadi][sink][ring]"
)
 {
    auto& rb = KVRing::instance();
    while (rb.try_pop()) {}
    REQUIRE_FALSE(rb.try_pop().has_value());
}

TEST_CASE (



"RingBufferSink: push and pop round-trips a pulse"
,
"[nadi][sink][ring]"
)
 {
    auto& rb = DBRing::instance();
    while (rb.try_pop()) {}

    DBPulse p{.id = {7}, .phase = PulsePhase::Begin, .timestamp_ns = 42,
              .payload = {Field<"rows", int>{99}}};
    DBRing::emit(p);

    auto got = rb.try_pop();
    REQUIRE(got.has_value());
    REQUIRE(got->id.value == 7);
    REQUIRE(got->phase == PulsePhase::Begin);
    REQUIRE(got->category_view() == "DB");

    // Verify the "rows" field was serialised into args.
    bool found_rows = false;
    got->for_each_arg([&](std::string_view k, std::string_view v) {
        if (k == "rows" && v == "99") found_rows = true;
    });
    REQUIRE(found_rows);
    REQUIRE_FALSE(rb.try_pop().has_value());
}

TEST_CASE (



"RingBufferSink: heterogeneous pulse types share one buffer"
,
"[nadi][sink][ring]"
)
 {
    // DBRing=RingBufferSink<4>, KVRing=RingBufferSink<8> — different Capacity,
    // different static instances. Use the same Capacity for both to get one buffer.
    using SharedRing = RingBufferSink<16>;
    auto& rb = SharedRing::instance();
    while (rb.try_pop()) {}

    SharedRing::emit(DBPulse{.id = {1}, .payload = {Field<"rows", int>{10}}});
    SharedRing::emit(KVPulse{.id = {2}, .payload = {Field<"hits", int>{5},
                                                      Field<"latency_ns", double>{1.0}}});
    std::size_t count = 0;
    while (rb.try_pop()) ++count;
    REQUIRE(count == 2);
}

TEST_CASE (



"RingBufferSink: wraps cleanly when capacity exceeded"
,
"[nadi][sink][ring]"
)
 {
    auto& rb = DBRing::instance();
    while (rb.try_pop()) {}
    for (int i = 1; i <= 6; ++i)
        DBRing::emit(DBPulse{.id = {static_cast<std::uint64_t>(i)},
                             .payload = {Field<"rows", int>{i * 10}}});
    std::vector<std::uint64_t> ids;
    while (auto e = rb.try_pop())
        ids.push_back(e->id.value);
    REQUIRE(ids.size() <= 4);
    REQUIRE_FALSE(ids.empty());
    for (auto id : ids)
        REQUIRE((id >= 3 && id <= 6));
    for (std::size_t i = 1; i < ids.size(); ++i)
        REQUIRE(ids[i] == ids[i - 1] + 1);
}

TEST_CASE (



"RingBufferSink: concurrent writers do not corrupt entries"
,
"[nadi][sink][ring][threading]"
)
 {
    constexpr std::size_t N_THREADS  = 4;
    constexpr std::size_t WRITES_PER = 50;
    auto& rb = KVRing::instance();
    while (rb.try_pop()) {}
    std::vector<std::thread> threads;
    threads.reserve(N_THREADS);
    for (std::size_t t = 0; t < N_THREADS; ++t) {
        threads.emplace_back([t] {
            for (std::size_t i = 0; i < WRITES_PER; ++i)
                KVRing::emit(KVPulse{
                    .id      = {t * 1000 + i},
                    .payload = {Field<"hits", int>{static_cast<int>(i)},
                                Field<"latency_ns", double>{static_cast<double>(i)}},
                });
        });
    }
    for (auto& th : threads) th.join();
    std::size_t drained = 0;
    while (auto p = rb.try_pop()) {
        ++drained;
        REQUIRE(p->id.value < N_THREADS * 1000 + WRITES_PER);
    }
    REQUIRE(drained <= N_THREADS * WRITES_PER);
}

// ============================================================================
// 10. Sinks — ChromeTraceSink
// ============================================================================

TEST_CASE (



"ChromeTraceSink satisfies SinkPolicy"
,
"[nadi][sink][chrome]"
)
 {
    STATIC_REQUIRE(SinkPolicy<ChromeTraceSink>);
}

TEST_CASE (



"ChromeTraceSink: enabled and Lossless flow_control"
,
"[nadi][sink][chrome]"
)
 {
    STATIC_REQUIRE(ChromeTraceSink::enabled == true);
    STATIC_REQUIRE(std::same_as<
        std::remove_cv_t<decltype(ChromeTraceSink::flow_control)>, Lossless>);
}

TEST_CASE (



"ChromeTraceSink: emits valid JSON with correct phase characters"
,
"[nadi][sink][chrome]"
)
 {
    std::ostringstream oss;
    ChromeTraceSink::out = &oss;
    ChromeTraceSink::emit(DBPulse{.id = {1}, .phase = PulsePhase::Begin,
                                  .timestamp_ns = 1000,
                                  .payload = {Field<"rows", int>{42}}});
    const auto s = oss.str();
    REQUIRE(s.find(R"("name":"DB")") != std::string::npos);
    REQUIRE(s.find(R"("ph":"B")")    != std::string::npos);
    REQUIRE(s.find(R"("ts":1)")      != std::string::npos);
    REQUIRE(s.find(R"("id":1)")      != std::string::npos);
    ChromeTraceSink::out = nullptr;
}

TEST_CASE (



"ChromeTraceSink: args object contains typed field"
,
"[nadi][sink][chrome]"
)
 {
    std::ostringstream oss;
    ChromeTraceSink::out = &oss;
    ChromeTraceSink::emit(DBPulse{.id = {2}, .phase = PulsePhase::End,
                                  .timestamp_ns = 5000,
                                  .payload = {Field<"rows", int>{77}}});
    const auto s = oss.str();
    REQUIRE(s.find(R"("args":{)")  != std::string::npos);
    REQUIRE(s.find(R"("rows":77)") != std::string::npos);
    REQUIRE(s.find(R"("ph":"E")")  != std::string::npos);
    ChromeTraceSink::out = nullptr;
}

TEST_CASE (



"ChromeTraceSink: multi-field pulse expands all args"
,
"[nadi][sink][chrome]"
)
 {
    std::ostringstream oss;
    ChromeTraceSink::out = &oss;
    ChromeTraceSink::emit(KVPulse{.id = {3}, .phase = PulsePhase::Instant,
                                  .timestamp_ns = 2000,
                                  .payload = {Field<"hits", int>{5},
                                              Field<"latency_ns", double>{1.25}}});
    const auto s = oss.str();
    REQUIRE(s.find(R"("hits":5)")      != std::string::npos);
    REQUIRE(s.find(R"("latency_ns":)") != std::string::npos);
    REQUIRE(s.find(R"("ph":"I")")      != std::string::npos);
    ChromeTraceSink::out = nullptr;
}

TEST_CASE (



"ChromeTraceSink: opt-in SourceLocation field is serialised"
,
"[nadi][sink][chrome]"
)
 {
    std::ostringstream oss;
    ChromeTraceSink::out = &oss;
    using LocPulse = Pulse<"Op",
                           Field<"val", int>,
                           Field<"__loc__", SourceLocation>>;
    ChromeTraceSink::emit(LocPulse{
        .id      = {10},
        .phase   = PulsePhase::Begin,
        .payload = {Field<"val", int>{9},
                    Field<"__loc__", SourceLocation>{"main.cpp", 42}},
    });
    const auto s = oss.str();
    REQUIRE(s.find(R"("val":9)")   != std::string::npos);
    REQUIRE(s.find(R"("__loc__":)") != std::string::npos);
    REQUIRE(s.find("main.cpp")     != std::string::npos);
    ChromeTraceSink::out = nullptr;
}

// ============================================================================
// 11. Sinks — ThreadLocalSink
// ============================================================================

TEST_CASE (



"ThreadLocalSink satisfies SinkPolicy"
,
"[nadi][sink][threadlocal]"
)
 {
    STATIC_REQUIRE(SinkPolicy<ThreadLocalSink>);
}

TEST_CASE (



"ThreadLocalSink: enabled is true and flow_control is DropNewest"
,
"[nadi][sink][threadlocal]"
)
 {
    STATIC_REQUIRE(ThreadLocalSink::enabled == true);
    STATIC_REQUIRE(std::same_as<
        std::remove_cv_t<decltype(ThreadLocalSink::flow_control)>, DropNewest>);
}

TEST_CASE (



"ThreadLocalSink: has_pulse_handler returns false when no handler is installed"
,
"[nadi][sink][threadlocal]"
)
 {
    set_pulse_handler({});
    REQUIRE_FALSE(has_pulse_handler());
}

TEST_CASE (



"ThreadLocalSink: emit is a no-op when no handler is installed"
,
"[nadi][sink][threadlocal]"
)
 {
    set_pulse_handler({});
    DBPulse p{.id = {1}, .phase = PulsePhase::Begin, .payload = {Field<"rows", int>{5}}};
    REQUIRE_NOTHROW(ThreadLocalSink::emit(p));
}

TEST_CASE (



"ThreadLocalSink: has_pulse_handler returns true after installing a handler"
,
"[nadi][sink][threadlocal]"
)
 {
    set_pulse_handler([](const PulseRecord&) {});
    REQUIRE(has_pulse_handler());
    set_pulse_handler({});
}

TEST_CASE (



"ThreadLocalSink: installed handler is called on emit"
,
"[nadi][sink][threadlocal]"
)
 {
    int call_count = 0;
    set_pulse_handler([&](const PulseRecord&) { ++call_count; });

    DBPulse p{.id = {2}, .phase = PulsePhase::End, .payload = {Field<"rows", int>{7}}};
    ThreadLocalSink::emit(p);
    REQUIRE(call_count == 1);

    set_pulse_handler({});
}

TEST_CASE (



"ThreadLocalSink: PulseRecord fields match the emitted pulse"
,
"[nadi][sink][threadlocal]"
)
 {
    PulseRecord received{};
    set_pulse_handler([&](const PulseRecord& r) { received = r; });

    DBPulse p{
        .id           = {42},
        .phase        = PulsePhase::Instant,
        .timestamp_ns = 9'000'000,
        .trace_id     = 11,
        .parent_id    = 5,
        .payload      = {Field<"rows", int>{99}},
    };
    ThreadLocalSink::emit(p);

    REQUIRE(received.category     == "DB");
    REQUIRE(received.event_id     == 42);
    REQUIRE(received.phase        == PulsePhase::Instant);
    REQUIRE(received.timestamp_ns == 9'000'000);
    REQUIRE(received.trace_id     == 11);
    REQUIRE(received.parent_id    == 5);

    set_pulse_handler({});
}

TEST_CASE (



"ThreadLocalSink: handler is uninstalled by passing an empty function"
,
"[nadi][sink][threadlocal]"
)
 {
    int call_count = 0;
    set_pulse_handler([&](const PulseRecord&) { ++call_count; });
    REQUIRE(has_pulse_handler());

    set_pulse_handler({});
    REQUIRE_FALSE(has_pulse_handler());

    DBPulse p{.id = {3}, .payload = {Field<"rows", int>{0}}};
    ThreadLocalSink::emit(p);
    REQUIRE(call_count == 0);
}

TEST_CASE (



"ThreadLocalSink: handler is replaced when set_pulse_handler is called again"
,
"[nadi][sink][threadlocal]"
)
 {
    int first_count = 0, second_count = 0;
    set_pulse_handler([&](const PulseRecord&) { ++first_count; });
    set_pulse_handler([&](const PulseRecord&) { ++second_count; });

    DBPulse p{.id = {4}, .payload = {Field<"rows", int>{0}}};
    ThreadLocalSink::emit(p);

    REQUIRE(first_count  == 0);
    REQUIRE(second_count == 1);
    set_pulse_handler({});
}

TEST_CASE (



"ThreadLocalSink: PulseScope<ThreadLocalSink> emits Begin and End to handler"
,
"[nadi][sink][threadlocal][scope]"
)
 {
    std::vector<PulseRecord> records;
    set_pulse_handler([&](const PulseRecord& r) { records.push_back(r); });
    restore_lineage({});

    { PulseScope<ThreadLocalSink, "Op"> scope{}; }

    REQUIRE(records.size() == 2);
    REQUIRE(records[0].phase    == PulsePhase::Begin);
    REQUIRE(records[1].phase    == PulsePhase::End);
    REQUIRE(records[0].event_id == records[1].event_id);
    REQUIRE(records[0].event_id != 0);
    REQUIRE(records[0].category == "Op");

    set_pulse_handler({});
}

TEST_CASE (



"ThreadLocalSink: PulseScope carries correct trace_id and parent_id to handler"
,
"[nadi][sink][threadlocal][scope][lineage]"
)
 {
    std::vector<PulseRecord> records;
    set_pulse_handler([&](const PulseRecord& r) { records.push_back(r); });
    restore_lineage({});

    {
        PulseScope<ThreadLocalSink, "Outer"> outer{};
        { PulseScope<ThreadLocalSink, "Inner"> inner{}; }
    }

    // 4 pulses: Outer-Begin, Inner-Begin, Inner-End, Outer-End
    REQUIRE(records.size() == 4);

    const auto outer_begin = records[0];
    const auto inner_begin = records[1];
    const auto inner_end   = records[2];
    const auto outer_end   = records[3];

    // Outer is a root scope — trace_id equals its own event_id, parent_id is 0.
    REQUIRE(outer_begin.trace_id  == outer_begin.event_id);
    REQUIRE(outer_begin.parent_id == 0);

    // Inner's parent_id must be the outer scope's event_id.
    REQUIRE(inner_begin.parent_id == outer_begin.event_id);

    // trace_id propagates: both carry the same root trace_id.
    REQUIRE(inner_begin.trace_id == outer_begin.trace_id);

    // Begin and End share the same event_id.
    REQUIRE(inner_begin.event_id == inner_end.event_id);
    REQUIRE(outer_begin.event_id == outer_end.event_id);

    set_pulse_handler({});
    restore_lineage({});
}

TEST_CASE (



"ThreadLocalSink: handler is isolated per thread"
,
"[nadi][sink][threadlocal][threading]"
)
 {
    int main_count  = 0;
    int child_count = 0;

    set_pulse_handler([&](const PulseRecord&) { ++main_count; });

    std::thread t([&] {
        // Child thread starts with no handler — installing one is independent.
        REQUIRE_FALSE(has_pulse_handler());
        set_pulse_handler([&](const PulseRecord&) { ++child_count; });

        DBPulse p{.id = {10}, .payload = {Field<"rows", int>{1}}};
        ThreadLocalSink::emit(p);

        set_pulse_handler({});
    });
    t.join();

    // Main thread's handler must not have fired.
    REQUIRE(main_count  == 0);
    REQUIRE(child_count == 1);
    REQUIRE(has_pulse_handler());

    set_pulse_handler({});
}

TEST_CASE (



"ThreadLocalSink: emit is safe when handler throws (noexcept boundary)"
,
"[nadi][sink][threadlocal]"
)
 {
    // ThreadLocalSink::emit is noexcept; a throwing handler must not propagate.
    // We verify the function is declared noexcept.
    DBPulse p{.id = {99}, .payload = {Field<"rows", int>{0}}};
    STATIC_REQUIRE(noexcept(ThreadLocalSink::emit(p)));
}

