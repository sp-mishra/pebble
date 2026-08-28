# NADI — High-Performance Observability Framework

## Overview

NADI is a modern C++23/26 header-only observability framework designed for low-overhead distributed tracing and
profiling. It provides:

- **Compile-time typed payloads** via fixed-string NTTPs and field aggregation
- **Zero-copy lineage tracking** for execution traces
- **Pluggable sink policies** for multiple output backends (ring buffer, profiler, Chrome trace)
- **Wall-time and cycle-count clocks** for both wall-time and performance profiling
- **Static routing** with compile-time sink validation

## Core Concepts

### Pulse & PulseScope

A **Pulse** is an event carrying:

- `id`: Unique event identifier
- `phase`: Begin, End, Instant, Duration, or Error
- `timestamp_ns`: Wall-time or cycle-count timestamp
- `trace_id`: Root scope ID of the trace tree
- `parent_id`: Immediate parent scope ID (0 = root)
- `payload`: Typed fields (compile-time validated)

**PulseScope** is a RAII scope guard that automatically:

1. Generates a Begin pulse on construction
2. Captures execution lineage (parent-child relationships)
3. Emits an End pulse on destruction
4. Restores prior lineage on exit

### Lineage & Distributed Tracing

Each scope maintains three lineage tokens:

- `trace_id`: This scope's own ID (children see this as their parent_id)
- `parent_id`: Enclosing scope's ID (0 = we are root)
- `root_id`: Root scope of the entire trace tree (propagated downward)

Lineage is **captured at construction time**, not read from thread-local state inside sinks, enabling safe
async/coroutine migration.

### Sink Policies

Sinks consume pulses via the `SinkPolicy` concept:

```cpp
template<typename T>
concept SinkPolicy =
    requires { { T::enabled } -> std::convertible_to<const bool &>; } &&
    requires { { T::flow_control } -> /* one of: DropNewest, OverwriteOldest, Lossless */ } &&
    requires(const Pulse &p) { { T::emit(p) } noexcept; };
```

Flow control strategies:

- **DropNewest**: Discard newest event if buffer full (preserve old events)
- **OverwriteOldest**: Discard oldest event if buffer full (preserve recent events)
- **Lossless**: Allocate on overflow (no data loss)

### Clock Policies

**SteadyClockPolicy** (default):

- Returns wall-time in nanoseconds
- `is_wall_time = true`
- Suitable for distributed tracing and profiling

**TscCycleClockPolicy**:

- Returns CPU cycle count via `rdtsc` (x86) or fallback to `SteadyClockPolicy`
- `is_wall_time = false` signals raw cycle data
- Sinks must handle cycle-to-wall-time conversion separately
- Useful for precise performance profiling

## Usage

### Basic Scope Instrumentation

```cpp
#include "observability/nadi.hpp"

using namespace utils::nadi;

struct LogSink {
    static constexpr bool enabled = true;
    static constexpr DropNewest flow_control = {};

    static constexpr void emit(const auto &pulse) noexcept {
        // Log pulse to file/buffer/stream
    }
};

void my_function() {
    PulseScope<LogSink, "my_function"> scope;
    // ... work happens here ...
} // End pulse emitted on scope exit
```

### Typed Payload Fields

```cpp
PulseScope<LogSink, "db_query",
    Field<"query_id", uint32_t>,
    Field<"row_count", size_t>
> scope(
    Field<"query_id", uint32_t>{.value = 42},
    Field<"row_count", size_t>{.value = 1000}
);
```

### Source Location Capture

```cpp
PulseScope<LogSink, "my_op",
    Field<"__loc__", SourceLocation>
> scope;
// __loc__ automatically populated with call site
```

Note: `source_location::current()` is captured at constructor call site. Wrapper functions disable location capture.
Either call `PulseScope` directly or document the limitation.

### Custom Clock Policy

```cpp
PulseScope<LogSink, "perf_event",
    Field<"cycles", uint64_t>
> scope;
// Uses SteadyClockPolicy by default

// For cycle counting:
BasicPulseScope<TscCycleClockPolicy, LogSink, "perf_event"> scope;
// Sinks must handle is_wall_time=false flag
```

### Multi-Sink Composition

```cpp
using TracingSinks = MultiSink<RingBufferSink, ChromeTraceSink>;
PulseScope<TracingSinks, "operation"> scope;
// Emits to both sinks (enabled sinks only)
```

## Performance Considerations

### Atomic Counter Contention

`generate_event_id()` uses a shared atomic counter. Under high concurrency (>10k events/sec), contention may be visible.
Mitigation:

- Use thread-local ID pools with periodic merge
- Implement NUMA-aware partitioning for multi-socket systems
- Monitor with cycle-count clocks to measure overhead

### Payload Tuple Layout

Payload tuple layout varies by compiler/ABI. ABI stability asserts ensure:

- Field sizes match compile-time expectations
- Layout assumptions in sinks remain valid

If cross-platform serialization is needed, use explicit packing and alignment directives.

### Source Location Overhead

`source_location::current()` is called at constructor time only. Zero runtime cost unless captured to payload.
Conditional capture via `Field<"__loc__", SourceLocation>` keeps minimal overhead for non-profiling paths.

### Clock Overhead

- **SteadyClockPolicy**: ~10-50 ns (depends on platform)
- **TscCycleClockPolicy**: ~3-5 cycles (low overhead, requires calibration)

## Sinks

Sinks are customizable backends. Common implementations:

### NoSink

Discard all pulses while preserving thread-local execution lineage. Use it when
code must propagate a trace context without routing events; omit `PulseScope`
entirely when the instrumentation boundary itself must compile away.

### RingBufferSink

Fixed-size circular buffer. Configurable flow control (DropNewest, OverwriteOldest). Zero allocation.

### ChromeTraceSink

Exports traces in Chrome DevTools JSON format. Supports distributed trace visualization.

### ProfilerSink

Aggregates cycle counts and duration statistics. Reports per-scope metrics.

## Design Rationale

### Why Compile-Time Validation?

Sinks are determined at compile time. This enables:

- Zero vtable overhead
- Static assert on type mismatch
- Dead-code elimination for disabled sinks
- Predictable binary layout

### Why Capture Lineage at Construction?

Reading lineage from thread-local state inside sinks breaks with async/coroutine migration. Capturing at construction
preserves lineage semantics across context switches.

### Why Optional Source Location?

`source_location::current()` has modest overhead. Opt-in via `Field<"__loc__", SourceLocation>` keeps
minimal-instrumentation paths fast.

### Why No Virtual Functions?

Virtual functions add vtable indirection and ABI coupling. Static dispatch ensures:

- Inlining opportunities
- Zero-cost abstraction
- Predictable performance

## ABI Notes

- Payload tuple layout is compiler/ABI-dependent. Static asserts validate assumptions.
- SourceLocation is restricted to trivial types for safe in-place modification.
- Event IDs are globally unique; overflow wraps at `std::uint64_t::max()`.

## Thread Safety

- `PulseScope` is not thread-safe; each thread maintains its own lineage stack via
  `thread_local detail::current_lineage`.
- Sinks must be thread-safe if multiple threads emit concurrently.
- `generate_event_id()` is thread-safe via atomic counter.

## Future Extensions

- Per-thread ID pools to reduce contention
- Adaptive flow control (backpressure)
- Distributed context propagation (W3C Trace Context)
- Zero-copy serialization (protobuf, FlatBuffers)

## See Also

- [Turbo Twig Telemetry](turbo_twig_telemetry.md) — cross-framework unified event timeline (Lithe + Pravaha + Sutra)
  that consumes NADI as an optional sink.
