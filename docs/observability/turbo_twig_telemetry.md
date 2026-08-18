# Turbo Twig Telemetry

**Header:** `include/telemetry/turbo_twig_telemetry.hpp`  
**Namespace:** `turbo_twig::telemetry`  
**Opt-in:** not pulled by any other turbo_twig header.  
**Observability family:** sibling to [NADI](nadi.md) — consumes NADI when available.

Cross-framework unified event timeline. Collects structured events from Lithe optimization
passes, Pravaha task execution, and Sutra gradient/tensor dispatch into one time-ordered
stream — with or without NADI.

---

## Canonical Channels

| Channel constant       | Value                | Framework | Payload type            |
|------------------------|----------------------|-----------|-------------------------|
| `kLithePassChannel`    | `"lithe.pass"`       | Lithe     | `nadi_pass_event`       |
| `kLitheEgraphChannel`  | `"lithe.egraph"`     | Lithe     | `nadi_pass_event`       |
| `kPravahaSchedChannel` | `"pravaha.schedule"` | Pravaha   | `pravaha_sched_event`   |
| `kPravahaExecChannel`  | `"pravaha.exec"`     | Pravaha   | `pravaha_exec_event`    |
| `kSutraGradChannel`    | `"sutra.gradient"`   | Sutra     | `sutra_grad_event`      |
| `kSutraTensorChannel`  | `"sutra.tensor"`     | Sutra     | `sutra_tensor_event`    |
| `kFeedbackChannel`     | `"feedback"`         | Cross     | `feedback_sample_event` |

`feedback_sample_event { std::size_t expression_hash; std::string backend_id; double latency_ms; }`
is emitted by the cross-framework Execution Feedback bridge
(`telemetry/execution_feedback.hpp`, namespace `turbo_twig::feedback`) each time a Pravaha task
timing is converted and recorded into the Lithe `feedback_store`. Emission happens only when a
`telemetry_session` is active, so it is zero-cost otherwise. `to_performance_report` counts its
`latency_ms` (converted to ns) in the per-channel latency total.


---

## `telemetry_session` — RAII Collection Scope

`telemetry_session` is non-copyable and non-movable (address stability required for the
thread-local pointer). It builds on an internal `collecting_telemetry_sink` (local
accumulating vector) and optionally notifies registered consumer callbacks.

```cpp
#include "telemetry/turbo_twig_telemetry.hpp"
using namespace turbo_twig::telemetry;

telemetry_session session;                          // RAII: becomes active on this thread

// Register an optional sink consumer (e.g. forward to NADI MultiSink)
session.add_consumer([](const telemetry_event& ev) {
    // forward ev to external sink ...
});

// Emit an event
session.emit(telemetry_event{
    .channel    = kLithePassChannel,
    .event_name = "simplify_add_zero",
    .payload    = nadi_pass_event{
        .pass_name    = "simplify_add_zero",
        .nodes_before = 5,
        .nodes_after  = 3,
        .changed      = true
    }
});

// Access active session from anywhere on the same thread
if (auto* s = telemetry_session::active()) {
    s->emit(/* ... */);
}
// session goes out of scope → previous session restored
```

Nested sessions are supported: destruction restores the previous `tl_active_` pointer.

---

## Consumer Utilities

```cpp
// Extract all Lithe pass events from a collected snapshot
auto events    = session.collect();
auto passes    = to_pass_events(events);        // vector<nadi_pass_event>
auto report    = to_performance_report(events); // per-channel count + mean latency
auto timeline  = session.format_timeline(/*ascii=*/true);
```

`to_performance_report` aggregates event count and total latency per channel, formatted
as a plain-text table. The ASCII timeline orders events by insertion (emit order).

---

## Example: Optimization + Task Execution → Unified Timeline

```cpp
#include "telemetry/turbo_twig_telemetry.hpp"
using namespace turbo_twig::telemetry;

telemetry_session session;

// --- Lithe pass side ---
session.emit({.channel = kLithePassChannel,
              .event_name = "constant_fold",
              .payload = nadi_pass_event{
                  .pass_name = "constant_fold",
                  .nodes_before = 8, .nodes_after = 4, .changed = true}});

// --- Pravaha task execution side ---
session.emit({.channel = kPravahaExecChannel,
              .event_name = "task.complete",
              .payload = pravaha_exec_event{
                  .task_id = 42, .backend = "JThreadBackend", .latency_ns = 18'500}});

// --- Sutra gradient side ---
session.emit({.channel = kSutraGradChannel,
              .event_name = "grad_parallel.wave",
              .payload = sutra_grad_event{.formula_hash = 0xDEAD, .wave_ns = 210'000}});

// Print unified timeline
std::puts(session.format_timeline().c_str());
// Print per-channel performance summary
std::puts(to_performance_report(session.collect()).c_str());
```

---

## NADI Integration

When `observability/nadi.hpp` is available at include time, `TT_TELEMETRY_HAS_NADI` is
defined to `1`. Wire a NADI `MultiSink` as a consumer to forward events into distributed
traces:

```cpp
#if TT_TELEMETRY_HAS_NADI
session.add_consumer([](const telemetry_event& ev) {
    // nadi::emit_pulse(ev.channel, ev.payload);  // forward to NADI
});
#endif
```

The telemetry layer itself never calls NADI directly — forwarding is the caller's choice,
keeping the dependency optional at runtime.
