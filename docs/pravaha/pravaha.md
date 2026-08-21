# Pravaha: Task-Graph Orchestration Engine (C++23)

**Current State:** High-performance, header-only, modern C++23 task-graph framework supporting single-threaded (
InlineBackend), multi-threaded (JThreadBackend), and coroutine-based execution. Optimized with SmallVector for reduced
allocations and unordered_map→map for memory efficiency.

---

## Table of Contents

- [Architecture Overview](#architecture-overview)
- [Algorithms Used](#algorithms-used)
- [Dependency Boundary](#dependency-boundary)
- [DSL Layer](#dsl-layer-pravahahpp)
- [IR Layer (Task IR)](#ir-layer-task-ir)
- [Execution Model](#execution-model-executionhpp)
- [Execution Backends](#execution-backends)
- [Performance & Optimization](#performance--optimization)
- [Example Usage](#example-usage)
- [Concepts](#concepts)
- [Error Handling](#error-handling)
- [State Diagram](#state-diagram)
- [TaskPriority](#taskpriority)
- [Cancellation](#cancellation)
- [Best Practices](#best-practices)
- [Sutra Integration: JIT Lowering to Pravaha IR](#sutra-integration-jit-lowering-to-pravaha-ir)
- [Parallel Numeric Differentiation](#parallel-numeric-differentiation--grad_parallel-and-build_backprop_dag)
- [Heterogeneous Execution Overlay](#heterogeneous-execution-overlay-pravaha_heterohpp)
- [Scheduler Policies](#scheduler-policies-pravahaschedulersscheduler_policyhpp)
- [Lithe Execution Analysis Handoff](#lithe-execution-analysis-handoff-litheexec)
- [References](#references)

---

## Dependency Boundary

`#include "pravaha/pravaha.hpp"` is the task-graph core. It does not include Lithe.
It uses the small, header-only Vākya structural layer only for the existing textual
task-DSl identity representation; it does not pull compiler passes, code generation,
JIT runtime, or GPU backends.

`#include "pravaha/compute.hpp"` opts into fused numerical execution. Its expression
type is a Vākya tree, so host SIMD and Metal consume the same lightweight form without
a compiler dependency. A caller already using Lithe can pass its re-exported Vākya trees
without conversion, but Lithe optimisation is caller-owned.

`#include "pravaha/adapters/lithe_runtime.hpp"` is an optional downstream Lithe
add-on. It is always safe to include: without `edsl/lithe_runtime.hpp` on the include
path, `pravaha::adapters::lithe_runtime::available` is `false` and no Lithe symbols are
declared. With Lithe present it provides fuel, GC-observer, and native-FFI task adapters.

`#include "pravaha/backends/vulkan_gpu.hpp"` is likewise an optional downstream
Lithe/Vulkan add-on. Without both Lithe Vulkan headers it exposes only
`pravaha::backends::vulkan::available == false`. Core, SIMD, and Metal remain usable
without either optional integration. This keeps Pebble below Lithe in the dependency
graph while retaining a source-compatible add-on seam for the future Lithe repository.

### CMake consumption

When Pebble is added as a subdirectory, downstream code should depend on the
header-only component rather than copying include paths:

```cmake
add_subdirectory(path/to/pebble)
target_link_libraries(my_lithe_or_application PRIVATE pebble::pravaha)
```

Pravaha requires C++23. Pebble selects C++26 on macOS toolchains that support it
and otherwise retains the C++23 baseline, so downstream projects can use newer
language features without raising Pravaha's minimum requirement.

---

## Algorithms Used

| Concern | Algorithm | Where |
|---|---|---|
| Graph validation | Cycle detection + topological sort over TaskIR | `pravaha.hpp` |
| Scheduling (default) | Priority ready-queue (High > Normal > Low) | `pravaha.hpp` |
| Scheduling (DAG) | Critical-path / longest-remaining-path scheduling | `schedulers/critical_path_scheduler.hpp` |
| Scheduling (parallel) | Work-stealing (per-worker deques + victim steal) | `schedulers/work_stealing_scheduler.hpp` |
| Scheduling (affinity) | Locality/NUMA hint routing | `schedulers/locality_scheduler.hpp` |
| Scheduling (GPU) | GPU-batch dispatch queue | `schedulers/gpu_scheduler.hpp` |
| Async model | P2300-style sender/receiver combinators | `execution.hpp` |
| Join policies | AllOrNothing, CollectAll, AnySuccess, Quorum(k) | `pravaha.hpp` |
| Cancellation | Cooperative token/source/scope + coroutine points | `backends/coroutine.hpp` |
| Parallel primitives | lazy_parallel_for / reduce / transform; eager parallel_reduce | `pravaha.hpp` |
| SIMD backend | Fused element-wise via Google Highway (static dispatch) | `backends/host_simd.hpp` |
| GPU backend (Metal) | MSL emit + Kosha PSO cache; threadgroup-barrier reduction tree | `backends/metal_gpu.hpp` |
| GPU backend (Vulkan) | Direct SPIR-V emit + workgroup shared-mem reduction; Kahan `sum` | `backends/vulkan_gpu.hpp` |
| Backend routing | Compile-time type filter + runtime cost race (`priority × 2³² + cost`) | `pravaha_hetero.hpp` |
| Kernel dedup | Vākya `structural_hash` → shared cache key | `compute.hpp` |
| Parallel reverse-mode AD | Wave-parallel backprop over Sutra DAG on JThreadBackend | `sutra/pravaha_ext.hpp` |

---

## Architecture Overview

```
┌─────────────────────────────────────────────────────────┐
│  User Code: DSL Composition                            │
│  task("work", []{}) | seq | all_of | ...              │
└──────────────────┬──────────────────────────────────────┘
                   │
┌──────────────────▼──────────────────────────────────────┐
│  DSL Layer (pravaha.hpp)                               │
│  • TaskExpr, SequenceExpr, ParallelExpr                │
│  • Join policies: AllOrNothing, CollectAll, Quorum    │
│  • parallel_for, parallel_reduce, parallel_transform  │
└──────────────────┬──────────────────────────────────────┘
                   │
┌──────────────────▼──────────────────────────────────────┐
│  IR Lowering & Validation (pravaha.hpp)               │
│  • Expression → TaskIR graph construction              │
│  • Cycle detection & topological sort                  │
│  • Join group formation & validation                   │
│  • Payload serialization metadata (type-erasure)       │
└──────────────────┬──────────────────────────────────────┘
                   │
┌──────────────────▼──────────────────────────────────────┐
│  Execution Models (execution.hpp)                      │
│  • Sender/Receiver combinators (P2300-style async)    │
│  • sync_wait, sync_wait_value, then, when_all         │
│  • upon_error, upon_stopped, tap_*                    │
│  • Schedulers: inline_scheduler, jthread_scheduler    │
└──────────────────┬──────────────────────────────────────┘
                   │
┌──────────────────▼──────────────────────────────────────┐
│  Runner & Scheduling (pravaha.hpp)                    │
│  • Backend selection (parameterized)                  │
│  • Task submission & ready-queue management           │
│  • Result slot type-erasure (placement new)           │
│  • Topological scheduling                             │
└──────────────────┬──────────────────────────────────────┘
                   │
┌──────────────────▼──────────────────────────────────────┐
│  Execution Backends (backends/coroutine.hpp)           │
│  • InlineBackend: synchronous, single-threaded        │
│  • JThreadBackend: multi-threaded via std::jthread    │
│  • CoroutineBackend: C++20 coroutine-based (async)   │
└─────────────────────────────────────────────────────────┘
```

---

## DSL Layer (pravaha.hpp)

### Expression Types

| Type                    | Purpose                   | Example                           |
|-------------------------|---------------------------|-----------------------------------|
| `TaskExpr<F>`           | Single task with callable | `task("work", [](){})`            |
| `SequenceExpr<L,R>`     | Sequential composition    | `seq(expr_a, expr_b)`             |
| `ParallelExpr<L,R>`     | Parallel fork-join        | `all_of(expr_a, expr_b)`          |
| `ParallelForExpr<I,F>`  | Data-parallel iteration   | `parallel_for(range, fn)`         |
| `ParallelReduceExpr`    | Reduce over chunks        | `parallel_reduce(data, init, fn)` |
| `ParallelTransformExpr` | Map over elements         | `parallel_transform(range, fn)`   |

### Composition Operators

```cpp
// Sequential: A → B
auto seq_expr = seq(task_a, task_b);

// Parallel: A ∥ B (fork-join)
auto par_expr = all_of(task_a, task_b, task_c);

// Data parallel
auto parallel = parallel_for(range, process_fn);
```

### Join Policies

| Policy           | Behavior                                            |
|------------------|-----------------------------------------------------|
| **AllOrNothing** | All predecessors must succeed; abort on any failure |
| **CollectAll**   | Aggregate results regardless of individual failures |
| **AnySuccess**   | Succeed if any predecessor succeeded                |
| **Quorum(k)**    | Succeed if ≥k predecessors succeeded                |

---

## IR Layer (Task IR)

### Purpose

Bridges DSL expressions to executable task graphs. Handles:

- Expression → IR lowering via visitor pattern
- Cycle detection (DAG validation)
- Topological sort for scheduling
- Join group formation
- Type-erased payload management

### IR Structure

```cpp
struct TaskIR {
    std::vector<IrNode> nodes;           // Task descriptors
    std::vector<IrEdge> edges;           // Precedence relations
    std::vector<IrJoinGroup> join_groups; // Join policy groups
    std::unordered_map<TaskId, ...> result_slots;
};

struct IrNode {
    TaskId id;
    std::string name;
    TaskPriority priority;
    ExecutionDomain domain;
    JoinPolicy join;
    std::shared_ptr<void> fn_storage;    // Type-erased callable
};
```

---

## Execution Model (execution.hpp)

### P2300-Style Sender/Receiver

Pravaha implements asynchronous primitives inspired by P2300 (executors):

```cpp
// Sender represents async work
struct sender<Expr, Backend> {
    Expr expr;
    std::shared_ptr<Backend> backend;
    std::optional<PravahaError> validation_error;
};

// Receiver handles completion
struct receiver_like concept {
    void set_value();
    void set_error(const PravahaError&);
    void set_stopped();
};

// Connect sender to receiver → operation_state
auto op = connect(sender, receiver);
start(op);  // launch async work
```

### Combinators

| Combinator                          | Purpose                             |
|-------------------------------------|-------------------------------------|
| `then(sender, name, fn)`            | Chain continuation after completion |
| `when_all(sender_a, sender_b, ...)` | Wait for all senders                |
| `tap_error(sender, fn)`             | Observer on error (no stop)         |
| `tap_stopped(sender, fn)`           | Observer on cancellation            |
| `upon_error(sender, fn)`            | Alias for tap_error                 |
| `upon_stopped(sender, fn)`          | Alias for tap_stopped               |

### Synchronous Wait

```cpp
auto result = sync_wait(sender);  // block until done
if (result.value) { /* success */ }
if (result.error) { /* failed */ }
if (result.stopped) { /* canceled */ }

auto value_result = sync_wait_value<int>(sender);
if (value_result.value) {
    int val = *value_result.result;  // extract value
}
```

### Schedulers

```cpp
// Inline (single-threaded)
execution::inline_scheduler sched;
auto sender = execution::schedule(sched);

// Jthread (multi-threaded)
execution::jthread_scheduler sched(4);  // 4 workers
auto sender = execution::schedule(sched);
```

---

## Execution Backends

### Backend Selection

| Use Case                    | Backend                | Reason                                |
|-----------------------------|------------------------|---------------------------------------|
| Unit testing, debugging     | InlineBackend          | Deterministic, no thread overhead     |
| Production CPU-bound        | JThreadBackend         | Hardware parallelism; tunable workers |
| Production I/O-bound        | CoroutineBackend       | Low context-switch; cooperative       |
| Real-time (< 1µs latency)   | InlineBackend          | No scheduling jitter                  |
| Embedded (constrained)      | InlineBackend          | No thread management                  |
| High throughput (100K+/sec) | JThreadBackend (tuned) | Parallelism > contention              |

### InlineBackend

**Synchronous, single-threaded, deterministic execution.**

```cpp
pravaha::Runner<> runner;  // default = InlineBackend
auto result = runner.submit(expr);
```

**Characteristics:**

- No threads — runs on caller's thread
- Fully deterministic — same input = same order
- No concurrency primitives needed
- Ideal for testing, debugging, embedded

### JThreadBackend

**Multi-threaded execution with worker pool.**

```cpp
JThreadBackend backend(4);           // 4 workers
JThreadBackend backend(4, 1000);     // 4 workers, max 1000 queued

Runner<JThreadBackend> runner(backend);
auto result = runner.submit(expr);
```

**Characteristics:**

- True parallelism via `std::jthread` pool
- Priority-based ready-queue (High > Normal > Low)
- Internal mutex + condition variables (lock-free ready-queue coming)
- Task-stealing fairness across workers

### CoroutineBackend

**Cooperative coroutine-based execution (C++20).**

```cpp
backends::CoroutineBackend backend;
Runner<backends::CoroutineBackend> runner(backend);
auto result = runner.submit(awaitable_expr);
```

**Characteristics:**

- Suspendable tasks via C++20 coroutines
- No OS threads — application thread runs event loop
- Ideal for I/O multiplexing, async pipelines
- Hierarchical suspension for nested coroutines

**Awaitable Types:**

- `yield_now()` — yield to other tasks
- `sleep_for(duration)` — timer-based wakeup
- `manual_reset_awaitable` — sync primitive (like Event)
- `async_channel<T>` — async MPSC channel
- `cancellation_point()` — cooperative cancellation check

---

## Performance & Optimization

### Container Optimization (C++23)

Pravaha uses modern C++ containers strategically:

| Component          | Container                                  | Optimization                    |
|--------------------|--------------------------------------------|---------------------------------|
| Task IR nodes      | `std::vector<IrNode>`                      | Dense, cache-friendly iteration |
| Task IR edges      | `std::vector<IrEdge>`                      | Sequential access pattern       |
| Suspended frames   | `std::map<void*, SuspendedFrame>`          | Ordered lookups, memory-stable  |
| Canceled frame set | `SmallVector<void*, 512>`                  | Typically small; avoids heap    |
| Ready queue        | `std::deque<coroutine_handle<>>`           | LIFO access + cache locality    |
| Timers             | `std::multimap<time_point, handle>`        | Ordered by expiry; O(log n)     |
| Result slots       | `std::unordered_map<TypeHash, ResultSlot>` | Type-erased payload storage     |

### Allocation Patterns

- **Header-only design** — zero linking overhead
- **SmallVector for bounded collections** — avoids heap for typical workloads
- **Type-erased placement new** — no vtable indirection
- **Shared pointers for backends** — reference-counted lifecycle
- **No virtual functions** — inline-friendly; zero indirection

### Correctness

- **Thread-safe backends** — mutexes + condition variables
- **Cycle detection** — topological sort validates DAG
- **Join group validation** — ensures predecessor consistency
- **Type safety** — schema hashing for payload type checks
- **Error propagation** — `Outcome<T>` (std::expected) for error handling

---

## Example Usage

### Simple Sequential

```cpp
using namespace pravaha;

auto work = seq(
    task("fetch", []() { /* ... */ return data; }),
    task("process", [](auto data) { /* ... */ })
);

Runner<> runner;
auto result = runner.submit(work);
```

### Parallel Fork-Join

```cpp
auto parallel = all_of(
    task("compute_a", []() { return val_a; }),
    task("compute_b", []() { return val_b; })
);

Runner<> runner;
auto result = runner.submit(parallel);
```

### With Join Policy

```cpp
auto resilient = all_of(
    task("retry_1", []() { /* ... */ }),
    task("retry_2", []() { /* ... */ })
);
resilient.set_join_policy(JoinPolicy{JoinPolicyKind::AnySuccess, 0});

Runner<> runner;
auto result = runner.submit(resilient);
```

### Data Parallel

```cpp
std::vector<int> data = {1, 2, 3, 4, 5};
auto transform = parallel_for(
    data,
    [](int x) { return x * 2; }
);

Runner<> runner;
auto result = runner.submit(transform);
```

### With Execution Model

```cpp
using namespace pravaha::execution;

auto sender = from_expr(my_expr);

auto result = sync_wait(
    then(
        then(sender, "step1", []() { /* ... */ }),
        "step2",
        []() { /* ... */ }
    )
);
```

---

## Concepts

### Payload Concept

Any type that is move-constructible and destructible.

```cpp
template<typename T>
concept Payload = std::move_constructible<T> && std::destructible<T>;
```

### Sender Concept

```cpp
template<class T>
concept sender_like = requires(T s) {
    typename std::remove_cvref_t<T>::expr_type;
    typename std::remove_cvref_t<T>::backend_type;
    get_expr(s);
};
```

### Receiver Concept

```cpp
template<class T>
concept receiver_like = requires(T r, const PravahaError& err) {
    r.set_value();
    r.set_error(err);
    r.set_stopped();
};
```

---

## Error Handling

### ErrorKind Enumeration

```cpp
enum class ErrorKind {
    ParseError, ValidationError, CycleDetected, SymbolNotFound,
    TypeMismatch, ExecutorUnavailable, DomainConstraintViolation,
    PayloadNotSerializable, PayloadNotTransferable, TaskFailed,
    TaskCanceled, QueueRejected, Timeout, InternalError,
    ResourceExhausted, InvalidArgument
};
```

### PravahaError

```cpp
struct PravahaError : std::exception {
    ErrorKind kind;
    std::string message;
    std::string task_identity;
    std::source_location location;
    
    const char* what() const noexcept override;
};
```

### Result Type

```cpp
template<class T>
using Outcome = std::expected<T, PravahaError>;
```

---

## State Diagram

```
Created → Ready → Scheduled → Running → {Succeeded, Failed, Canceled, Skipped}
```

| State     | Meaning                                         |
|-----------|-------------------------------------------------|
| Created   | Task instantiated; not yet ready                |
| Ready     | Dependencies satisfied; eligible for scheduling |
| Scheduled | Assigned to worker/thread                       |
| Running   | Currently executing                             |
| Succeeded | Completed with success                          |
| Failed    | Completed with error                            |
| Canceled  | Canceled via CancellationToken                  |
| Skipped   | Skipped due to join policy                      |

---

## TaskPriority

```cpp
enum class TaskPriority { Low, Normal, High };
```

Ready-queue dequeuing respects priority: `High > Normal > Low`.

---

## Cancellation

### CancellationToken

```cpp
struct CancellationToken {
    bool stop_requested() const;
    void request_stop();
};
```

Cancellation is **cooperative**: tasks must explicitly check `stop_requested()` or hit a cancellation point.
`CoroutineBackend::request_stop()` additionally guarantees that frames already marked ready but not yet
resumed are canceled rather than resumed, so a `mark_ready` followed by `request_stop` never re-enters the coroutine.

### Cancellation Points

In coroutine backend:

```cpp
co_await cancellation_point();  // throws if canceled
```

In JThreadBackend:

```cpp
if (token && token->stop_requested()) {
    throw PravahaError{ErrorKind::TaskCanceled, "..."};
}
```

---

## Best Practices

1. **Choose backend based on workload** — inline for tests, jthread for production compute, coroutine for I/O
2. **Keep tasks small** — favor many small tasks over few large ones (better scheduling parallelism)
3. **Immutable payloads** — avoid shared state; use result aggregation instead
4. **Explicit error handling** — check `Outcome<T>.has_value()` or use `.value_or()`
5. **Monitor queue depth** — high queue depth indicates thread starvation
6. **Use high priority sparingly** — priority inversion risk; keep High tasks minimal
7. **Batch I/O** — aggregate I/O operations; let CoroutineBackend multiplex

---

## Sutra Integration: JIT Lowering to Pravaha IR

`sutra/pravaha_ext.hpp` bridges the Sutra symbolic EDSL to Pravaha task graphs.
`sutra/sutra.hpp` contains the JIT compiler (`target_id::jit_x86_64`) and the
`lower_to_mir` / `lower_to_pravaha_ir` lowering pipeline.

### Op Support Matrix

| Op                                                 | `lower_to_mir` (JIT)                 | `lower_to_pravaha_ir`     | Notes                                                                                                                                                  |
|----------------------------------------------------|--------------------------------------|---------------------------|--------------------------------------------------------------------------------------------------------------------------------------------------------|
| Arithmetic (`add` `sub` `mul` `div` `neg` `mod`)   | ✅ native MIR opcodes                 | ✅ lambda                  | Directly mapped                                                                                                                                        |
| Comparisons (`eq` `ne` `lt` `le` `gt` `ge`)        | ✅ `fcmp_*`                           | ✅ lambda                  |                                                                                                                                                        |
| Logical (`and_` `or_` `not_`)                      | ✅ `fcmp_*`                           | ✅ lambda                  |                                                                                                                                                        |
| `pow`                                              | ✅ indirect_call bridge               | ✅ `std::pow` lambda       | `eval_fn` set in builtin descriptor                                                                                                                    |
| `if_`                                              | ❌ no `fsel` opcode → scalar fallback | ✅ lazy branch coordinator | MIR lacks select; Pravaha IR: branch coordinator at `ExecutionDomain::Inline`, `AnySuccess` join policy, inactive branch slot → NaN (`State::Skipped`) |
| `seq_`                                             | ✅ `mov` child[1]                     | ✅ forward child[1]        | Side effects resolved in post-order                                                                                                                    |
| `sum_` (N-ary)                                     | ✅ left-linear `fadd` chain           | ✅ N-ary accumulation      | Variadic: any arity                                                                                                                                    |
| Plugin ops with `eval_fn`, arity 1–8, non-stateful | ✅ indirect_call bridge               | ✅ eval_fn lambda          | sin, cos, exp, tanh, relu, etc.                                                                                                                        |
| Plugin ops, stateful                               | ❌ → scalar fallback                  | ❌ → nullopt               | Stateful ops must run in host context                                                                                                                  |
| Plugin ops, no `eval_fn`                           | ❌ → scalar fallback                  | ❌ → nullopt               |                                                                                                                                                        |
| `call_` (unresolved)                               | ❌ → scalar fallback                  | ❌ → nullopt               | Must be resolved before compilation                                                                                                                    |

### JIT Bridge Mechanism

Plugin ops and `op::pow` use an indirect_call bridge:

```
eval_fn ptr → load_imm (GPR)
child FP pregs → fp_to_gpr
indirect_call [sym, fn_bits, arg_0..arg_N-1]
result GPR → gpr_to_fp (FP preg)
```

Bridge functions (`plugin_bridge_1` … `plugin_bridge_8`) adapt
`double(*)(const double*, uint8_t)` to the `int64_t(int64_t, ...)` ABI
expected by the linker indirect_call mechanism.

### `op::seq_` Lowering

`seq_(side_effect, value)`: post-order traversal guarantees the side_effect
node is evaluated before the parent. The `seq_` node simply forwards child[1]'s
register/slot — zero overhead in both MIR and Pravaha IR.

### `op::sum_` Lowering

In MIR: emitted as a left-linear `fadd` chain — `acc = c0+c1`, then
`acc = acc+c2`, … up to any arity. No bridge overhead; pure arithmetic.

In Pravaha IR: a single compute lambda accumulates all child slots.

### Windowed MIR Constant Folding

`windowed_mir_fold` walks post-order and attempts to constant-fold bounded
subgraphs via the MIR peephole pipeline. Plugin ops with `eval_fn` (including
`pow`, `sin`, `cos`, etc.) are now included in foldable windows, enabling
expressions like `sin(π/2)` to fold to `1.0` at compile time when all inputs
are constants.

### Stateful Op Routing

Any plugin op with `effect_kind::stateful` in its `effect_mask_` bypasses JIT
and Pravaha IR lowering and is routed to the scalar interpreter. This preserves
host-side execution context semantics for ops with observable side effects.

---

## Roadmap

1. **CoroutineBackend stability** — in progress; async channel needs testing
2. **Locality hints** — future: `ExecutionDomain` binding (NUMA awareness)
3. **Lock-free ready-queue** — planned for JThreadBackend (currently mutex-protected)
4. **Hierarchical task groups** — task namespacing + nested join policies
5. **Trace/debug tooling** — chrome trace sink (partial); enhanced introspection planned

---

## Parallel Numeric Differentiation — `grad_parallel` and `build_backprop_dag`

Sutra's `context::grad()` builds a symbolic formula DAG suitable for further
differentiation and compilation. `context::grad_parallel()` is a complementary
entry point that performs reverse-mode automatic differentiation numerically,
using `JThreadBackend` for wave-parallel backprop. It returns a `lit_f64`
formula_ref containing the computed scalar gradient.

### When to use each

|                                    | `context::grad()`                    | `context::grad_parallel()`             |
|------------------------------------|--------------------------------------|----------------------------------------|
| Output                             | Symbolic formula DAG                 | Numeric scalar (`lit_f64`)             |
| Can differentiate again?           | Yes                                  | No                                     |
| Can compile with different params? | Yes                                  | No — requires new call per param set   |
| Parallelism                        | None (sequential reverse pass)       | Wave-parallel via JThreadBackend       |
| Zero-allocation (warm)             | Yes                                  | No — allocates JThreadBackend per call |
| Best for                           | Higher-order grad, symbolic analysis | Large DAGs, one-shot numeric gradients |

### `context::grad_parallel()` — API

```cpp
// Defined in: include/sutra/sutra.hpp
// Must include: sutra/sutra.hpp

formula_ref context::grad_parallel(
    formula_ref f,
    const param& wrt,
    const std::unordered_map<symbol_id, double>& env);

formula_ref context::grad_parallel(
    formula_ref f,
    const var& wrt,
    const std::unordered_map<symbol_id, double>& env);
```

**env** maps `symbol_id → double` for all parameters and variables referenced
by `f`. Use `ctx.intern("name")` to obtain `symbol_id` values.

**Example:**

```cpp
context ctx;
ctx.use(sutra::math::extension{});

param x("x"), y("y");
formula_ref f = math::sigmoid(x * x + y);   // f(x,y) = sigmoid(x²+y)

symbol_id x_id = ctx.intern("x");
symbol_id y_id = ctx.intern("y");
std::unordered_map<symbol_id, double> env{{x_id, 2.0}, {y_id, 1.0}};

formula_ref df_dx = ctx.grad_parallel(f, x, env);
// df_dx is a lit_f64 node; extract the value:
double grad_val = ctx.owned_store().at(df_dx.root).lit.f64;
```

### Algorithm — five steps

1. **Migrate** `f` to `owned_store` if needed.
2. **Post-order walk** + symbol resolution (reuses `GradWorkspace`).
3. **Forward pass** — evaluate all node values numerically in topo order, building a
   `fwd_vals[node_index]` array. Leaf nodes read from `env`; op-nodes compute from children.
4. **Wave-parallel reverse pass**:
    - Compute wave depths (same algorithm as `build_pravaha_parallel`).
    - Process waves from root to leaves (highest depth → 0).
    - Per wave: submit each node as a `TaskCommand` to `JThreadBackend`; block on
      `std::latch` until all tasks in the wave complete.
    - Each task loads the parent adjoint atomically, calls `desc->diff_rule()` to get
      the symbolic partial, evaluates it numerically via `eval_tree(..., fwd_vals)`,
      and accumulates `parent_adj * partial_val` into the child's `adj[]` slot using
      a CAS loop.
5. **Extract** `adj[target_node]` as the final gradient scalar.

### `build_backprop_dag()` — structured artifact API

```cpp
// Defined in: include/sutra/pravaha_ext.hpp
// Must include: sutra/pravaha_ext.hpp (which includes sutra.hpp)

[[nodiscard]] backprop_artifact build_backprop_dag(
    const formula_store& store,
    node_index root,
    const std::vector<node_index>& topo,
    const std::unordered_map<symbol_id, double>& env,
    const op_registry& op_reg);
```

Returns a `backprop_artifact` containing:

- **`waves`** — vector of wave groups in root-first order; each wave holds
  `BackpropTask` descriptors (`idx`, `node_copy`, `desc` pointer).
- **`fwd_vals`** — forward-pass values indexed by `node_index`.
- **`root`** — root node_index (seed adjoint here to `1.0`).
- **`max_node_idx`** — size the adjoint accumulator array to `max_node_idx + 1`.

This is useful when you need the structured wave grouping for custom execution
strategies (e.g. GPU dispatch, profiling, serialization) rather than the
`JThreadBackend`-based dispatch embedded in `grad_parallel()`.

### Thread safety

- `grad_parallel()` owns its `JThreadBackend` and atomic accumulator array.
  It is safe to call concurrently on different `context` objects.
- Concurrent calls on the **same** `context` are not safe because `grad_parallel()`
  modifies `owned_store_` (migrating formulas, allocating the result node).

---

## Building & Testing

### Build

```bash
mkdir build && cd build
cmake -DCMAKE_CXX_STANDARD=23 ..
cmake --build .
```

### Tests

```bash
ctest --output-on-failure
```

---

---

## Heterogeneous Execution Overlay (`pravaha_hetero.hpp`)

**Files:**

- `include/pravaha/pravaha_hetero.hpp` — core: compute types, routing, NADI, `basic_hetero_executor` (auto-includes SIMD
  backend), `capability.hpp`
- `include/pravaha/backends/capability.hpp` — `ComputeBackend` concept, `backend_traits` (compute routing axis),
  `backend_set`
- `include/pravaha/backends/host_simd.hpp` — CPU SIMD backend (Highway) + `HostSimdBackend` wrapper; included by
  `pravaha_hetero.hpp`
- `include/pravaha/backends/metal_gpu.hpp` — Metal GPU backend + MSL emitter + Kosha cache + `MetalGpuBackend` wrapper (
  priority=200); include explicitly for GPU dispatch on Apple
- `include/pravaha/backends/vulkan_gpu.hpp` — Vulkan GPU backend + direct SPIR-V emitter (strategy 3b, no glslang) +
  Kosha cache + `VulkanGpuBackend` wrapper (priority=150); reuses Lithe `vk_build_pipeline` /
  `vk_alloc_pools_and_wrap` / `vulkan_resource::bind_storage_buffers` + `dispatch_sync` for
  device/pipeline/descriptor/fence lifetime; Pravaha owns only `VkBuffer` alloc + host staging + readback (pooled via
  `staging_pool` so warm dispatches skip per-call alloc); shared `VkDevice` held by an injectable,
  thread-safe-on-first-use `device_provider` (replaces the old static singleton); reductions use a true GPU tree in
  Workgroup shared memory (`OpControlBarrier`) with the two-pass CPU-fold retained as fallback; element type is
  compile-time `compute::element_type_for<T>`
- `include/pravaha/pravaha_expr.hpp` — user-facing eDSL surface (includes `pravaha_hetero.hpp`)

**Namespaces:** `pravaha::compute`, `pravaha::hetero`, `pravaha::backends`, `pravaha::backends::simd_detail`,
`pravaha::backends::metal`, `pravaha::backends::metal::msl`, `pravaha::backends::vulkan`,
`pravaha::backends::vulkan::spirv`
**Status:** Parts 1–6 complete + Vulkan GPU backend added. Concept-driven backend registry. Full invariant matrix
verified. `pravaha::distributed` topology stubs added. `compute_view::operator[]` (C++23 multidim subscript) added.

A generic, header-only EDSL overlay for dispatching Lithe expression graphs to heterogeneous compute backends (CPU SIMD,
Metal GPU, Vulkan GPU). Satisfies four invariants:

| Invariant                  | Guarantee                                                                                                   |
|----------------------------|-------------------------------------------------------------------------------------------------------------|
| 1. No AST contamination    | `lithe::node<>` never gains hardware fields; metadata lives in `execution_context` keyed by structural hash |
| 2. Const-correct views     | Immutable source → compile error on writable view (`make_view` requires non-const T)                        |
| 3. No silent degradation   | Every fallback emits a NADI event (Parts 2–3)                                                               |
| 4. Async context isolation | No thread-local hardware metadata; all bound to explicit context object                                     |

### Architecture

```
Lithe expression AST  (node<Tag, Children...>)
        │
        │  [optional] lithe::compiler::optimize_preset<O2>()  ← routing_policy::optimize_before_codegen
        │  structural_hash()  ← topology only, no HW fields (Invariant 1)
        │
execution_context  ← overlay_: hash → node_metadata (grid, buffers, domain)
        │
basic_hetero_executor<Backends...>
        │
        │  backend_score() / backend_reduce_score()   ← compile-time type filter +
        │                                                runtime cost race (priority × 2^32 + cost)
        │
        ├── MetalGpuBackend    (priority=200, elementwise≥256KB, reduce≥1MB)  — Apple only
        ├── VulkanGpuBackend   (priority=150, elementwise≥256KB, reduce≥1MB)  — HAS_VULKAN/MoltenVK
        └── HostSimdBackend    (priority=10, always available)
                │
                ├── host_simd_backend  (Part 2 — Highway SIMD)
                └── [fallback] scalar loop
```

**Default backend sets** (auto-selected by include chain):

| Platform                                                   | Set                                 |
|------------------------------------------------------------|-------------------------------------|
| Apple + `HAS_METAL_CPP` + `LITHE_VULKAN_BACKEND_AVAILABLE` | `Metal(200), Vulkan(150), SIMD(10)` |
| Apple + `HAS_METAL_CPP` only                               | `Metal(200), SIMD(10)`              |
| `LITHE_VULKAN_BACKEND_AVAILABLE` (no Metal)                | `Vulkan(150), SIMD(10)`             |
| Neither                                                    | `SIMD(10)`                          |

**No `#if` in dispatch.** Platform detection is confined to backend wrappers and default set definitions.
`basic_hetero_executor` iterates the backend tuple purely via `if constexpr` + fold expressions.

### `namespace pravaha::compute`

#### Type aliases

```cpp
using dim_t      = std::uint64_t;
using index_vec  = containers::dynamic::SmallVector<dim_t, 64 * sizeof(dim_t)>;  // 64 dims inline
using stride_vec = containers::dynamic::SmallVector<dim_t, 64 * sizeof(dim_t)>;
```

#### `memory_layout`

| Value           | Meaning                         |
|-----------------|---------------------------------|
| `contiguous`    | Flat linear buffer              |
| `strided`       | Custom multi-stride             |
| `tiled_2d`      | Cache-blocked 2D                |
| `packed_simd`   | Matched to HW vector width      |
| `host_coherent` | Apple unified memory, zero-copy |

#### `data_element_type`

`unknown`, `bool8`, `i8`, `i16`, `i32`, `i64`, `u8`, `u16`, `u32`, `u64`, `f16`, `bf16`, `f32`, `f64`, `complex64`,
`complex128`

```cpp
constexpr std::size_t   element_size(data_element_type t) noexcept;
constexpr std::string_view msl_scalar_name(data_element_type t) noexcept;
// Note: f64 returns "float" — Part 3 router rejects f64 for GPU upstream.
```

#### `buffer_descriptor`

```cpp
struct buffer_descriptor {
    index_vec         shape;
    stride_vec        strides;
    memory_layout     layout       = memory_layout::contiguous;
    data_element_type element_type = data_element_type::unknown;
    std::uint16_t     alignment    = 64;
    bool              writable     = false;
    bool              is_unified   = true;   // Apple Silicon zero-copy default

    constexpr dim_t       element_count()   const noexcept;  // product(shape)
    constexpr std::size_t footprint_bytes() const noexcept;  // routing decision variable
};
```

#### `compute_view<T>` — const-correctness gate (Invariant 2)

```cpp
template <typename T>
struct compute_view {
    T*                data;
    buffer_descriptor desc;
    dim_t             offset;

    T*       base() noexcept;
    const T* base() const noexcept;

    // Real offset/stride slicing (Part D). range{begin,end,step} selects a
    // sub-range; a bare integer collapses a dimension. Sets layout=strided.
    struct range { dim_t begin = 0; dim_t end = 0; dim_t step = 1; };
    template <typename... Sel>
    compute_view slice(Sel... sel) const;

    stride_vec effective_strides() const;   // row-major strides when desc.strides empty
    dim_t      inner_stride()     const;     // 1 = packed; >1 = strided
    bool       is_contiguous()    const;     // SIMD fast-path predicate
};

// make_view: requires non-const T → sets desc.writable = true
template <typename T>
compute_view<T>       make_view(T* data, buffer_descriptor desc);

// make_const_view: always read-only
template <typename T>
compute_view<const T> make_const_view(const T* data, buffer_descriptor desc);
```

Attempting `make_view(const_ptr, ...)` is a `static_assert` compile error (Invariant 2).

---

### `namespace pravaha::hetero`

#### `compute_domain`

```cpp
enum class compute_domain : std::uint8_t {
    auto_select = 0,
    host_simd   = 1,   // Part 2
    metal_gpu   = 2,   // Part 3
    vulkan      = 3    // Part 6 — Vulkan GPU (MoltenVK on macOS, native elsewhere)
};
```

Does NOT modify `pravaha::ExecutionDomain`. Kept separate per the no-AST-contamination invariant.

#### `compute_grid_descriptor`

```cpp
struct compute_grid_descriptor {
    std::array<dim_t, 3> global_size = {1, 1, 1};
    std::array<dim_t, 3> local_size  = {1, 1, 1};
    dim_t                simd_width  = 0;   // 0 = auto-detect

    static compute_grid_descriptor from_flat(dim_t n, dim_t tg = 256) noexcept;
};
```

#### Structural Hash (Invariant 1 keystone)

```cpp
// Thin uint64 adapter over lithe::emit::structural_hash.
// Tag identity: lithe::emit::tag_descriptor<Tag>::stable_id (extension-band ids ≥ 1000).
// lit constants: structural_payload_hash(lit_node<T>&) ADL hook folds value via double bit-cast.
// static_assert(sizeof(size_t)==8) guards the macOS-first 64-bit target assumption.
template <typename E>
std::uint64_t structural_hash(const E& expr) noexcept;
```

Two structurally identical expressions → identical hash → shared kernel cache entry. Hardware fields are never in the
hash; they live in `execution_context`. Distinct `lit` constants (e.g. `lit(1.0f)` vs `lit(2.0f)`) produce distinct keys
via the payload hook — a baked-in MSL literal is part of the compiled kernel.

#### `execution_context`

```cpp
struct execution_context {
    void                  bind(uint64_t hash, node_metadata meta);
    const node_metadata*  lookup(uint64_t hash) const noexcept;  // nullptr if missing
    std::size_t           size()  const noexcept;
    void                  clear() noexcept;
};
```

Owns `std::unordered_map<uint64_t, node_metadata>`. No thread-local storage (Invariant 4). Pass explicitly to every
backend call.

`node_metadata` holds: `input_desc`, `output_desc`, `compute_grid_descriptor`, `compute_domain preferred`.

#### Routing Engine

```cpp
struct routing_policy {
    std::size_t    gpu_threshold_bytes = 256 * 1024;   // >= → try GPU
    compute_domain force               = compute_domain::auto_select;
    bool           allow_gpu           = true;
};

// Pure, noexcept, allocation-free. Deterministic.
compute_domain route(const buffer_descriptor& desc, const routing_policy& policy) noexcept;
```

**Routing rules (in order):**

1. `policy.force != auto_select` → return `policy.force`
2. `element_type == f64` → `host_simd` (no Metal f64 scalar)
3. `footprint_bytes >= gpu_threshold_bytes && allow_gpu` → `metal_gpu`
4. Otherwise → `host_simd`

---

### Part Roadmap

| Part       | Content                                                                                                                                                                                                                                                          | File                                                                                 |
|------------|------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|--------------------------------------------------------------------------------------|
| 1 (done)   | Foundation types, execution context, routing                                                                                                                                                                                                                     | `pravaha_hetero.hpp`                                                                 |
| 2 (done)   | `host_simd_backend` (Highway SIMD)                                                                                                                                                                                                                               | `backends/host_simd.hpp`                                                             |
| 3 (done)   | `metal_gpu_backend`, MSL emitter                                                                                                                                                                                                                                 | `backends/metal_gpu.hpp`                                                             |
| 4 (done)   | Kernel cache (Kosha), `hetero_executor`, NADI telemetry                                                                                                                                                                                                          | `pravaha_hetero.hpp`, `backends/metal_gpu.hpp`                                       |
| 5 (done)   | Example, invariant matrix, docs, runbook, hardening backlog                                                                                                                                                                                                      | `example_pravaha_hetero.hpp` (testfw pattern), `test_npravaha_hetero.cpp` (appended) |
| 6 (done)   | `compute_view::operator[]`, `pravaha::distributed` descriptor stubs                                                                                                                                                                                              | `pravaha_hetero.hpp`                                                                 |
| 7 (done)   | Vulkan GPU backend: direct SPIR-V emitter + MoltenVK dispatch + `VulkanGpuBackend` (priority=150)                                                                                                                                                                | `backends/vulkan_gpu.hpp`                                                            |
| 7.1 (done) | Vulkan hardening: `staging_pool` (kills per-dispatch alloc), true GPU reduction tree (Workgroup shared-mem fold, two-pass retained as fallback), injectable `device_provider` (replaces static singleton), compile-time `element_type_for<T>` (no hardcoded f32) | `backends/vulkan_gpu.hpp`, `pravaha_hetero.hpp`                                      |
| A (done)   | `lit_node` value honored + distinct hash; eDSL wired (`pravaha_expr.hpp`)                                                                                                                                                                                        | `pravaha_expr.hpp`, `pravaha_hetero.hpp`, `backends/metal_gpu.hpp`                   |
| B (done)   | Math builtins on SIMD vector body + Metal (`sqrt/exp/log/sin/cos/abs`)                                                                                                                                                                                           | `backends/host_simd.hpp`, `backends/metal_gpu.hpp`                                   |
| C (done)   | Multi-input `y = f(x0, x1, …)` via `input<N>` leaves + buffer table                                                                                                                                                                                              | `pravaha_expr.hpp`, `pravaha_hetero.hpp`, `backends/metal_gpu.hpp`                   |
| D (done)   | Real view slicing / `operator[]` offset+stride math; strided SIMD path                                                                                                                                                                                           | `pravaha_hetero.hpp`, `backends/host_simd.hpp`                                       |
| E (done)   | Reductions (`reduce_sum/max/min`) — SIMD `hn::Reduce*` + GPU threadgroup barrier                                                                                                                                                                                 | `pravaha_expr.hpp`, `backends/host_simd.hpp`, `backends/metal_gpu.hpp`               |

---

### `namespace pravaha::backends::metal` — Part 3: Metal GPU Backend

**File:** `include/pravaha/pravaha_metal.hpp` (new — includes `pravaha_hetero.hpp`)
**Impl TU:** None — MLX (`dependencies/mlx/mlx/backend/metal/device.cpp`) already defines
`NS/CA/MTL_PRIVATE_IMPLEMENTATION`. Adding a second definition causes 1927 duplicate symbol linker errors; do NOT
re-define these macros.
**Platform:** macOS only for device/dispatch. MSL emitter is platform-independent.
**CMake guard:** `HAS_METAL_CPP` — detected from `dependencies/metal-cpp/Metal/Metal.hpp`.

#### MSL Emitter (`namespace pravaha::backends::metal::msl`)

Platform-independent. No `HAS_METAL_CPP` guard — MSL string tests run on Linux CI too.

**Design note:** MSL emission is deliberately NOT delegated to Lithe (no generic source-text GPU backend exists in
Lithe). Only the arithmetic operator spelling (`+ - * /`) comes from `lithe::emit::tag_descriptor<tag>::symbol`; all MSL
builtin call-forms (`sqrt(`, `fabs(` etc.) remain local — they are MSL-specific, not bare symbols. The binary branch is
guarded by `static_assert(tag_descriptor<tag>::arity==2)`.

`is_input_leaf<N>` + `uses_input_leaves<E>()` use `lithe::tree::any_tag_satisfies` to detect `input<N>` leaves; no
hand-rolled recursion.

```cpp
// Emit one MSL scalar expression fragment into `os`. `var` = input variable
// base name ("x"). `indexed` selects the leaf naming scheme: false → bare `var`
// (single-input `call_tag` trees), true → `var0/var1/…` (multi-input `input<N>`
// trees). Chosen automatically by `emit_kernel` via `uses_input_leaves<E>()`.
template <typename E>
void emit_expr(std::ostream& os, const E& expr, std::string_view var,
               bool indexed = false);

// Produce a complete MSL compute kernel string for dst[i] = expr(src[i]).
// Supported tags: add, sub, mul, div, neg, call (leaf = input).
// No threadgroup_barrier — element-wise kernels are embarrassingly parallel.
template <typename E>
[[nodiscard]] std::string emit_kernel(const E& expr,
                                      compute::data_element_type elem,
                                      std::string_view kernel_name = "pravaha_kernel");
```

Scalar type mapping: `f32`→`float`, `f16`→`half`, `i32`→`int`, `u32`→`uint`, etc. (delegates to
`compute::msl_scalar_name`).

Convenience aliases: `pravaha::backends::metal::emit_expr` and `emit_kernel` forward to `msl::`.

#### `metal_gpu_backend` (macOS + HAS_METAL_CPP)

```cpp
struct metal_gpu_backend {
    MTL::Device*       device;  // retained by CreateSystemDefaultDevice
    MTL::CommandQueue* queue;   // owned; released in destructor

    static metal_gpu_backend& instance();  // process-wide singleton
    bool available() const noexcept;

    // Compile MSL source → PSO. Caller owns returned pointer (+1 retain).
    Outcome<MTL::ComputePipelineState*> compile(const std::string& msl,
                                                std::string_view fn_name = "pravaha_kernel") const;

    // Dispatch compiled PSO: dst[i] = kernel(src[i]).
    // Zero-copy when both views have is_unified=true and page-aligned base ptrs.
    template <typename T>
    Outcome<void> dispatch(MTL::ComputePipelineState* pso,
                           compute::compute_view<T> dst,
                           compute::compute_view<const T> src,
                           const hetero::compute_grid_descriptor& grid);
};
```

**Memory rule:** every metal-cpp `new*` returns +1 retain; release explicitly. Buffers released per-dispatch; PSO
released by caller (or by Part-4 cache on eviction).

**Zero-copy path:** `newBuffer(ptr, bytes, StorageModeShared, nullptr)` wraps host pointer directly — valid on Apple
unified memory when pointer is 4 KB page-aligned. Fallback: `newBuffer(bytes) + memcpy` in/out.

#### `run_gpu_uncached` (uncached, for Part 3 testing)

```cpp
template <typename T, typename E>
Outcome<void> run_gpu_uncached(const E& expr,
                               compute::compute_view<T> dst,
                               compute::compute_view<const T> src);
```

Emit MSL → compile → dispatch → release PSO in one call. No caching — Part 4 adds Kosha kernel cache to avoid
recompiling.

#### Part 4: Kernel cache (`namespace pravaha::backends::metal`)

`pravaha_metal.hpp` (inside `#if HAS_METAL_CPP`) adds a process-wide sharded LRU cache for compiled Metal pipeline
states. Eviction is safe via `NS::SharedPtr` (Option A): when an entry is evicted, the shared pointer destructor
releases the Metal resource.

```cpp
// Value type — SharedPtr handles eviction-safe lifetime.
using pipeline_ptr   = NS::SharedPtr<MTL::ComputePipelineState>;
using pipeline_cache =
    kosha::adapter::ShardedCache<
        kosha::core::Cache<std::uint64_t, pipeline_ptr>, 8 /*shards*/>;

// Process-wide singleton — 256 distinct compiled kernels retained.
[[nodiscard]] inline pipeline_cache& kernel_cache();

// Returns cached (or freshly compiled) pipeline for `expr`.
// Emits NADI kernel_cache_hit / kernel_cache_miss events.
template <typename E>
[[nodiscard]] Outcome<pipeline_ptr>
get_or_compile(const E& expr, compute::data_element_type elem);
```

Key: `structural_hash(expr)` (topology-only, Invariant 1). Second call with identical tree → hit; same `pipeline_ptr`
returned, cache size unchanged.

#### Part 4: `basic_hetero_executor` / `hetero_executor` (`namespace pravaha::hetero`)

Concept-driven, open-closed executor parameterized on a compile-time backend tuple. No `#if`, no `switch(domain)`. New
backends are added by satisfying `ComputeBackend` and listing them in `backend_set`.

```cpp
// Extensible executor — add backends by extending the type list.
template <typename... Backends>
struct basic_hetero_executor {
    routing_policy policy{};
    [[no_unique_address]] std::tuple<Backends...> backends_{};

    template <typename T, typename E>
    Outcome<void> execute(const E& expr, compute::compute_view<T> dst,
                          compute::compute_view<const T> src, const execution_context& ctx);

    template <expr::reduce_op Op, typename T, typename Child>
    Outcome<T> reduce(const Child& child, compute::compute_view<const T> src,
                      const execution_context& ctx);
    // … multi-input overloads omitted for brevity
};

// Platform defaults (Metal+SIMD on Apple, SIMD-only otherwise):
using hetero_executor = default_hetero_executor;  // backward-compatible alias
```

**ComputeBackend concept** (`backends/capability.hpp`):

```cpp
template <typename B>
concept ComputeBackend = requires(B be, std::size_t hash,
                                  data_element_type type, const buffer_descriptor& desc) {
    { B::static_metadata() } noexcept -> std::same_as<backend_metadata>;  // name + priority
    { be.is_available()   } noexcept -> std::same_as<bool>;
    { be.supports_expression(hash, type) } noexcept -> std::same_as<bool>;
    { be.evaluate_cost(desc, hash)       } noexcept -> std::same_as<std::uint64_t>;
};
// Optional: evaluate_reduce_cost() — used instead of evaluate_cost() for reduction paths.
```

**Routing order per call:**

1. `routing_policy::optimize_before_codegen` → run O3 canonicalization at the executor entry (see *Canonicalization
   boundary* below). The default preset is `lithe::preset::O3`; set `optimize_before_codegen = false` to bypass (
   identity pass, zero overhead).
2. `routing_policy::force == host_simd` → short-circuit to SIMD without entering cost race.
3. `execution_context` preferred override (hash lookup, `host_simd` domain only — backward compat).
4. **Cost race**: for each backend, `backend_score()` / `backend_reduce_score()` = `priority × 2³²+ cost`. Zero =
   unavailable/unsupported. Highest score wins.
5. **Cascade fallback**: on `Outcome` failure, zeroes winner's score → re-runs cost race with next candidate. Each step
   emits NADI `backend_fallback` pulse.
6. All backends exhausted → direct SIMD scalar path.

**Canonicalization boundary** — runs as step 0, before hashing and the cost race. Defined in `pravaha::hetero`:

```cpp
// Concept: expression type acceptable to all backends (flat AST, no std::variant shell).
template <class E>
concept FlatExpression = lithe::Expression<E> && !lithe::VariantExpr<E>;

// Always-on canonicalization: applies Preset (default O3), strips the phase wrapper,
// static_asserts FlatExpression on the result.
template <class Preset = lithe::preset::O3, lithe::Expression E>
[[nodiscard]] auto canonicalize_apply(E&& expr);

// Conditional canonicalization: if optimize==true runs canonicalize_apply, else
// passes expr through unchanged. Uses continuation-passing (Fn receives the result)
// so the type stays concrete in each branch — no type erasure.
template <class E, class Fn>
decltype(auto) with_canon(bool optimize, E&& expr, Fn&& fn);
```

- `canonicalize_apply` calls `O3{}(expr)` then `lithe::unwrap_expr()` to strip the `optimized_expr<T>` wrapper, leaving
  a plain `node<Tag,...>` that satisfies `FlatExpression`.
- `with_canon` is the pattern used by all four executor entry points (single/multi execute, single/multi reduce):
  `with_canon(policy.optimize_before_codegen, expr, [&](auto&& canon){ ... })`. The lambda receives either the original
  type or the canonicalized type; both branches are concretely typed.
- **`structural_hash` is called on the canonical form**, not the raw expression — algebraically-equal trees (e.g.
  `x + lit(0.0f)` and `x` after O3) collapse to one cache key.
- **`true_cse_pass` caveat:** `lithe::preset::O3` runs `simplify_add_zero`, `mul_identity`, `constant_fold_arith`,
  `strength_reduction`, and `dead_subtree_elimination`. The `true_cse_pass` in O3 is a **fixpoint no-op placeholder** —
  structural CSE (sharing repeated subexpressions) is not performed. The hash collapse guarantee covers fold/simplify
  reductions only; it does not deduplicate structurally distinct trees that happen to be semantically equivalent.

**Built-in backends:**

| Backend           | Priority | Elementwise GPU threshold           | Reduce GPU threshold         |
|-------------------|----------|-------------------------------------|------------------------------|
| `MetalGpuBackend` | 200      | 256 KB (`kGpuElementwiseThreshold`) | 1 MB (`kGpuReduceThreshold`) |
| `HostSimdBackend` | 10       | always available                    | always available             |

`MetalGpuBackend::evaluate_reduce_cost()` uses `kGpuReduceThreshold`; `evaluate_cost()` uses `kGpuElementwiseThreshold`.
`backend_reduce_score()` calls `evaluate_reduce_cost()` when present via `if constexpr requires`.

#### CMake additions

```cmake
# Detection (after MLX block):
if(APPLE)
    set(METAL_CPP_DIR ${CMAKE_SOURCE_DIR}/dependencies/metal-cpp)
    # ... sets HAS_METAL_CPP, links Metal/Foundation/QuartzCore frameworks
endif()

# Both executable and test targets (no extra sources — MLX owns the impl TU):
if(HAS_METAL_CPP)
    target_link_libraries(<target> PRIVATE
        ${METAL_FRAMEWORK} ${FOUNDATION_FRAMEWORK} ${QUARTZCORE_FRAMEWORK})
    target_compile_definitions(<target> PRIVATE HAS_METAL_CPP=1)
endif()
```

`pravaha_metal_impl.cpp` does NOT exist — MLX already owns the private implementation symbols. Never re-create it.

### `namespace pravaha::backends` — Part 2: Host SIMD Backend

Fused element-wise SIMD evaluator using Google Highway in static-dispatch mode (mirrors
`include/containers/tree/NAryTree.hpp` convention).

#### `host_simd_backend`

```cpp
struct host_simd_backend {
    // Element-wise: dst[i] = expr(src[i]) for i in [0, n).
    // Single-input model (v0.1). T must be float or double.
    // No heap allocation on hot path — vectors live in registers.
    template <typename T, typename E>
    Outcome<void> execute(const E&                        expr,
                          compute::compute_view<T>        dst,
                          compute::compute_view<const T>  src,
                          const hetero::execution_context& ctx);
};
```

Execution: vector body (`hn::LoadU` / `hn::StoreU` for contiguous; `hn::GatherIndex` / `hn::ScatterIndex` for strided) +
Highway masked remainder via `hn::LoadN` / `hn::StoreN` (contiguous paths) or scalar cleanup (strided-remainder,
stride-overflow fallback). Lane count detected at runtime via `hn::Lanes(d)`. No scalar cleanup remains for contiguous
non-power-of-two `n`.

Supported Lithe tags: `add_tag`, `sub_tag`, `mul_tag`, `div_tag`, `neg_tag`, `call_tag` (leaf = input vector).

#### `run_simd_or_fallback`

```cpp
template <typename T, typename E>
Outcome<void> run_simd_or_fallback(const E& expr,
                                   compute::compute_view<T> dst,
                                   compute::compute_view<const T> src,
                                   const hetero::execution_context& ctx);
```

Compile-time dispatch: if `simd_detail::is_simd_capable<E>()` → `host_simd_backend::execute`. Otherwise: emit
`hetero::emit_fallback_event` (Invariant 3) + scalar interpreter loop.

#### `namespace pravaha::backends::simd_detail`

| Symbol                      | Kind             | Purpose                                                                                                            |
|-----------------------------|------------------|--------------------------------------------------------------------------------------------------------------------|
| `eval_vec<D,E>(d, x, expr)` | template fn      | Walk Lithe AST, emit Highway ops on vector `x`; binary branch guarded by `static_assert(tag_descriptor::arity==2)` |
| `eval_scalar<T,E>(x, expr)` | template fn      | Scalar tail / fallback evaluator; same arity guard                                                                 |
| `simd_tag_ok<N>`            | predicate struct | Backend-local SIMD tag set; `value=true` iff node tag is supported                                                 |
| `is_simd_capable<E>()`      | consteval fn     | `lithe::tree::all_tags_satisfy<E, simd_tag_ok>()` — generic fold, no hand-rolled recursion                         |
| `slot_contrib`              | functor          | Per-node input-slot contribution: `input<N>`→N+1, `call_tag`→1, else 0                                             |
| `input_slot_count<E>()`     | consteval fn     | `lithe::tree::fold<E>(slot_contrib{}, max, 0)` — max-slot reduction                                                |

**`is_simd_capable` usage:**

```cpp
STATIC_REQUIRE(simd_detail::is_simd_capable<decltype(add_expr)>());   // true
STATIC_REQUIRE_FALSE(simd_detail::is_simd_capable<decltype(lt_expr)>()); // false
```

#### `namespace pravaha::hetero` additions (Parts 2 & 4)

```cpp
// NADI telemetry. Default sink = NoSink (zero cost).
// Override: #define PRAVAHA_HETERO_SINK MyCaptureSink before including.
using hetero_sink = PRAVAHA_HETERO_SINK;  // alias resolved at include time

// Emitters (Invariant 3 — every fallback fires one of these):
inline void emit_backend_selected(compute_domain, uint64_t hash, size_t bytes) noexcept;
inline void emit_cache_event(bool hit, uint64_t hash) noexcept;
inline void emit_fallback_event(std::string_view reason) noexcept;

// RAII GPU dispatch scope (TscCycleClockPolicy):
struct nadi_gpu_dispatch_scope { /* emits Begin/End pulse */ };

struct simd_events {
    static constexpr auto exec_begin = "pravaha.hetero.simd_exec";  // PulseScope category
};
```

#### Highway include pattern (static dispatch)

```cpp
#include <hwy/highway.h>
#include <hwy/aligned_allocator.h>
namespace hn = hwy::HWY_NAMESPACE;  // per-TU active target
const hn::ScalableTag<float> d;
// hn::LoadU, hn::StoreU, hn::Add, hn::Mul, hn::Sub, hn::Div, hn::Neg, hn::Lanes
```

No `foreach_target.h` — matches existing repo convention.

---

### Scope (v0.1)

- Element-wise `add/sub/mul/div/neg` over a single input buffer (`call_tag` leaf).
- Float element types on the SIMD path; f64 stays on SIMD (no Metal f64 scalar).
- Single machine, macOS-first. No distributed/RPC, no Vulkan, no matrix/tensor ops.

---

### `namespace pravaha::distributed` — Topology Descriptor Stubs

Pure data layout types. No network calls. Distributed runtime is a future separate edition.
Defined in `pravaha_hetero.hpp` alongside the hetero overlay.

```cpp
namespace pravaha::distributed {

using NodeId = std::uint32_t;

struct remote_node_descriptor {
    NodeId        id         = 0;
    std::string   ip_address;
    std::uint16_t port       = 50051;
    bool          is_active  = true;
};

struct distributed_payload_metadata {
    std::size_t   payload_bytes = 0;
    std::uint64_t schema_hash   = 0;  // cross-node binary layout validation
    bool          is_blittable  = true;
};

struct rpc_task_descriptor {
    std::string                  target_symbol;
    NodeId                       assigned_node_id = 0;
    pravaha::JoinPolicy          network_join_policy{};
    distributed_payload_metadata serialization_meta;
};

} // namespace pravaha::distributed
```

| Type                           | Purpose                                                      |
|--------------------------------|--------------------------------------------------------------|
| `remote_node_descriptor`       | Endpoint descriptor for a networked compute node             |
| `distributed_payload_metadata` | Binary payload size + schema hash for cross-node type safety |
| `rpc_task_descriptor`          | Task fragment dispatched to a remote node with join policy   |

---

### `compute_view<T>::operator[]` (C++23 multidimensional subscript)

`operator[](Sel...)` is the spec-required name for multi-dimensional view slicing.
Delegates to `slice()` — real offset+stride math (Part D). A `range{begin,end,step}`
selects a sub-range; a bare integer collapses that dimension.

```cpp
auto even = view[compute::range{0, N, 2}];  // stride-2 view (32 of 64 elements)
auto row  = view.slice(1);                   // select row 1; dim collapses to 1
// Strided reads use hn::GatherIndex; strided writes use hn::ScatterIndex.
// The Metal GPU path falls back to SIMD for non-contiguous views (kernel assumes packed buffers).
```

---

### eDSL surface (`pravaha_expr.hpp`) — Parts A–E

The user-facing expression layer. `#include "pravaha/pravaha_expr.hpp"` (pulls in
`pravaha_hetero.hpp`). Whiteboard-grade notation via Lithe operator overloads.

```cpp
using namespace pravaha::expr;

var x;                       // input<0> — binds buffer slot 0
input<1> y;                  // second input buffer (multi-input, Part C)

auto e   = x * x + lit(1.0f);        // constant honored on every path (Part A)
auto axpy = lit(3.0f) * x + y;       // AXPY — two-input kernel (Part C)
auto g   = sqrt(exp(sin(x)));        // math builtins: SIMD + MSL (Part B)

auto r   = reduce_sum(x * x);        // reduction wrapper (Part E)
// executor.reduce<reduce_op::sum, float>(reduce_child(r), src, ctx) → Outcome<float>
```

- **`lit(v)`** — typed constant leaf (`lit_tag`). Its value is folded into the
  structural hash via an ADL `structural_payload_hash` hook (see `pravaha_expr.hpp`), so
  `lit(1.0f)` and `lit(2.0f)` compile to distinct cached kernels.
- **`input<N>` / `var`** — indexed input leaves; `var == input<0>`. `input_slot_count<E>()`
  derives the compile-time buffer count `K`; backends bind one buffer per slot.
- **Tag registration** — All Pravaha tags (`input_tag<N>`, `lit_tag`, math tags, `reduce_tag`)
  are registered with Lithe via `lithe::emit::tag_descriptor` specializations
  (extension-band ids ≥ 1000) in `pravaha_hetero.hpp`. This makes them visible to
  `lithe::emit::structural_hash` and `lithe::tree` compile-time folds. See
  [Tag Metadata & Extensibility](../../edsl/lithe.md#tag-metadata--extensibility).
- **Math free functions** — `sqrt/exp/log/sin/cos/abs` vectorize via Highway contrib
  (`hn::Sqrt/Exp/Log/Sin/Cos/Abs`) and emit MSL builtins; `neg`, `sq` also provided.
- **`reduce_sum/reduce_max/reduce_min(e)`** — wrap an element-wise child into a
  reduction. SIMD folds lane partials then `hn::ReduceSum/Max/Min`; the remainder for
  `sum` uses `hn::LoadN` masked partial-load (Highway tail masking) while `max`/`min`
  use a scalar tail to avoid identity-value pollution. GPU emits a two-stage threadgroup
  reduction with `threadgroup_barrier` (the only barrier in the overlay). Routed by
  `routing_policy::reduce_gpu_threshold_bytes` (default 1 MB). Multi-input reduce (e.g.
  `reduce_sum(x0 * x1)` for dot products) routes to `run_reduce_simd_multi<Op,T,K>` /
  `execute_reduction_multi` using the same `sum_accum_t` wide accumulator.
- **Sum accumulation is wide by design.** A `sum` reduction over an `f32` buffer
  accumulates the running total in `double`, not `float` — naive `f32` accumulation
  over ~1e8 terms loses everything below the running magnitude (error ≫ 1e-4). The
  reduction still *returns* the element type `T`; only the internal accumulator
  promotes (`sum_accum_t<Op,T>` = `double` iff `Op==sum && T==float`, else `T`).
  `max`/`min` never lose precision, so they accumulate in native `T`. This is applied
  at every fold point: the SIMD vector body flushes each chunk's horizontal into the
  wide scalar so the `f32` partial never grows large; the GPU host-side fold of the
  ~N/256 threadgroup partials runs in `double`; and — since Metal has no `f64` — the
  GPU threadgroup reduction tree uses **Kahan compensation** for `sum` (the widest
  accuracy available on-device). No caller-side `N` reduction is needed; large-N
  reductions stay accurate internally.

---

### Hardening Backlog

| Item                                             | Status                  | Where it lands                                                                                                                                                         |
|--------------------------------------------------|-------------------------|------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| Multi-input buffers (`y = f(x0, x1, ...)`)       | **done (Part C)**       | `input<N>` leaves; `execute<T,K>` buffer table                                                                                                                         |
| View slicing (real offset math)                  | **done (Part D)**       | `compute_view::slice` / `operator[]`                                                                                                                                   |
| Reductions + `threadgroup_barrier`               | **done (Part E)**       | `reduce_*` path; barrier in reduce kernel only                                                                                                                         |
| Extra tags (`exp/log/sqrt/abs`)                  | **done (Part B)**       | tag tables in `eval_vec`/`eval_scalar`/`emit_expr`                                                                                                                     |
| Lithe O3 canonicalization boundary               | **done (Phase 1)**      | `FlatExpression` concept + `canonicalize_apply` + `with_canon` in `pravaha_hetero.hpp`; `structural_hash` called on canonical form; cache-key fragmentation eliminated |
| Vectorized strided writes via scatter            | **done (Phase 2.2)**    | `hn::ScatterIndex` in `host_simd.hpp` execute path; scalar fallback for stride-overflow only                                                                           |
| Highway masked tail (`LoadN`/`StoreN`)           | **done (Phase 2.3)**    | Contiguous execute + `sum` reduce remainder fully vectorized; `max`/`min` scalar tail retained (identity-value safety)                                                 |
| Multi-buffer GPU reduce wire-up                  | **done (Phase 2.1)**    | `emit_reduce_kernel_multi` / `dispatch_reduce_multi` / `run_reduce_simd_multi` already present; cost race + wide accumulator confirmed on multi path                   |
| Include hygiene (Highway / metal-cpp confined)   | **done (Phase 3)**      | Highway includes confined to `backends/host_simd.hpp`; metal-cpp confined to `backends/metal_gpu.hpp`; no `#if` in `basic_hetero_executor`                             |
| Non-Apple GPU (Vulkan/SPIR-V)                    | deferred (out of scope) | New `pravaha_vulkan.hpp` behind its own guard                                                                                                                          |
| Distributed / RPC graph splitting                | deferred (out of scope) | Separate distributed edition                                                                                                                                           |
| `newBufferWithBytesNoCopy` true zero-copy        | deferred                | Tighten alignment path in `dispatch`                                                                                                                                   |
| GPU strided-view kernel (offset/stride uniforms) | deferred (SIMD covers)  | Strided views fall back to SIMD reduce/element-wise                                                                                                                    |

---

---

## Scheduler Policies (`pravaha/schedulers/scheduler_policy.hpp`)

### `SchedulerPolicy` Concept

```cpp
template <class P>
concept SchedulerPolicy = requires(P& p, task_token t, std::size_t worker_id) {
    { p.on_task_ready(t) }            -> std::same_as<void>;
    { p.on_task_complete(t) }         -> std::same_as<void>;
    { p.select_next_task(worker_id) } -> std::same_as<std::optional<task_token>>;
};
```

`task_token` carries `{id, priority, dag_depth, locality_hint}`.  
GPU tasks: `locality_hint == ~0uz` (tested via `task_token::is_gpu()`).  
Heterogeneous tags (`ExecutionDomain::External`) route tasks to external engines; no Metal/Vulkan enum is exposed at
this layer.

### Built-in Policies

| Policy                           | Header                        | When to use                                                                                                         |
|----------------------------------|-------------------------------|---------------------------------------------------------------------------------------------------------------------|
| `fifo_scheduler_policy`          | `scheduler_policy.hpp`        | Baseline correctness, low-contention workloads                                                                      |
| `priority_scheduler_policy`      | `scheduler_policy.hpp`        | When High > Normal > Low task ordering matters (mirrors `JThreadBackend`)                                           |
| `critical_path_scheduler_policy` | `critical_path_scheduler.hpp` | DAG-aware; minimises critical-path latency; call `init_from_dag(ir)` once after IR build                            |
| `work_stealing_scheduler_policy` | `work_stealing_scheduler.hpp` | CPU-bound parallel workloads; per-worker deques + victim steal                                                      |
| `locality_scheduler_policy`      | `locality_scheduler.hpp`      | NUMA / cluster affinity; routes tasks to the worker whose `locality_hint` matches                                   |
| `gpu_scheduler_policy`           | `gpu_scheduler.hpp`           | Routes tasks with `is_gpu()==true` to a GPU dispatch queue; other tasks fall through to a configurable inner policy |

### Slotting into `Runner`

Policies are composable as a policy set. A `Runner` forwards `on_task_ready` /
`on_task_complete` / `select_next_task` to the active policy:

```cpp
using namespace pravaha::sched;

critical_path_scheduler_policy sched;
sched.init_from_dag(ir);  // one-time setup
// pass sched into JThreadBackend's scheduling hook (or use directly)
```

### `profiling_scheduler_policy` Overlay

`pravaha_profiler.hpp` provides `profiling_scheduler_policy<Inner>` — a transparent
wrapper that records per-task latencies via NADI and delegates all scheduling decisions
to the inner policy:

```cpp
profiling_scheduler_policy<priority_scheduler_policy> instrumented;
// Records latency per task; exposes profiler::measure-compatible stats.
```

---

## Lithe Execution Analysis Handoff (`lithe::exec`)

Pravaha receives parallel and GPU plans that have already been gated by Lithe's automatic execution
analysis layer (`lithe::exec`, opt-in via `include/edsl/lithe_exec/lithe_exec.hpp`).

**Handoff protocol:**

- Legality and profitability decisions are made upstream in `lithe::exec::auto_execution_pass`.
- Only plans with `execution_kind::threaded` or `execution_kind::gpu` and `analysis_outcome::proven_legal`
  (or `unknown` with runtime guards passing) are lowered to `hl::task_decomposition_plan` and forwarded.
- Scalar fallback plans are emitted by `lithe::exec` for any `analysis_outcome::unknown` region;
  Pravaha executes the fast path and may report fallback telemetry when a guard fails at runtime.
- Guard predicates (no_alias, aligned, min_trip_count, device_available, device_resident) are evaluated
  before dispatch; failing guards route to the scalar fallback plan.
- `lithe::intelligence::schedule_bridge` maps the chosen `execution_kind` to a `schedule_policy_id`
  for Pravaha's scheduler selection.

**Pravaha is unchanged by this layer** — `task_decomposition_plan` is the same POD it always was;
`lithe::exec` only gates which plans arrive.

---

## References

- **C++23 Features:** Concepts, Ranges, Coroutines (C++20), `std::expected` (C++23)
- **Design Inspiration:** P2300 (Executors), P2049 (std::coroutine)
- **Architecture:** DAG task scheduling, type-erased payloads, sender/receiver async
