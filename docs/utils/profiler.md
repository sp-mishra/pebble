# 🕒 Profiler Library

A modern, header-only C++ profiling utility for benchmarking code blocks, functions, and algorithms with support for
parallelism, outlier trimming, exception tracking, and detailed statistical reporting.

---

## Features

- **High-precision timing** using `std::chrono`
- **Parallel execution** (multi-threaded benchmarking)
- **Warmup iterations** to stabilize measurements
- **Outlier trimming** (remove slowest/fastest runs)
- **Exception tracking** per iteration
- **Return value collection** (optional)
- **Custom logging** and iteration callbacks
- **Statistical summary**: mean, median, min, max, stddev, percentiles

---

## Quick Start

```cpp
#include "utils/profiler.hpp"

profiler::ProfileConfig config;
config.iterations = 100;
config.label = "MyFunction";
auto result = profiler::measure(config, []() {
    // Code to benchmark
});
std::cout << profiler::format_result(result) << std::endl;
```

---

## API Overview

### ProfileConfig

Configure your profiling session:

| Field                      | Description                                          |
|----------------------------|------------------------------------------------------|
| `iterations`               | Number of measured iterations                        |
| `warmup_iterations`        | Warmup runs before measurement (capped at 1M)        |
| `parallelism`              | Number of threads to use (capped at hw×4)            |
| `label`                    | Optional label for reporting                         |
| `trim_outliers_percentage` | % of runs to trim from each end (outliers) [0-100]   |
| `logger`                   | Optional logger: `void(std::string_view)`            |
| `output_unit`              | Time unit for formatting (default: Microseconds)     |
| `progress_callback`        | Progress reporter: `void(double progress)` [0.0-1.0] |
| `track_memory`             | Enable memory profiling (experimental)               |
| `export_chrome_trace`      | Generate Chrome Tracing format (reserved)            |

**Configuration Validation:**

- `parallelism == 0` → clamped to 1
- `parallelism > hardware_concurrency × 4` → capped (absolute maximum: 128)
- `trim_outliers_percentage < 0` → clamped to 0
- `trim_outliers_percentage > 100` → clamped to 100
- `warmup_iterations > 1'000'000` → capped to 1M
- `iterations == 0` → returns empty result immediately

---

### Profiling Functions

#### `profiler::measure`

```cpp
auto result = profiler::measure(config, func [, on_iteration_callback], args...);
```

- `func`: Callable to benchmark (can return a value or be void)
- `on_iteration_callback` (optional): Called after each iteration. Signature:
    - For void-returning: `bool(std::chrono::nanoseconds duration, std::size_t iteration)`
    - For value-returning: `bool(std::chrono::nanoseconds duration, const T& value, std::size_t iteration)`
    - Return `false` to stop early.

#### Return Value

- If `func` returns void: returns `ProfileResult`
- If `func` returns T: returns `ProfileResultWithData<T>`

---

### ProfileResult

| Field                   | Description                            |
|-------------------------|----------------------------------------|
| `label`                 | Label from config                      |
| `total_duration`        | Total measured time                    |
| `average_duration`      | Mean time per iteration                |
| `min_duration`          | Fastest iteration                      |
| `max_duration`          | Slowest iteration                      |
| `iterations_attempted`  | Total iterations requested             |
| `iterations_succeeded`  | Iterations completed without exception |
| `parallelism_used`      | Threads used                           |
| `warmup_iterations_run` | Warmup runs performed                  |
| `individual_runs`       | Vector of durations per iteration      |
| `unique_exceptions`     | Map: exception message → count         |
| `outlier_info`          | Info on trimmed outliers (if any)      |
| `per_thread_runs`       | Vector of per-thread duration vectors  |
| `memory_stats`          | Optional memory profiling data         |

#### Methods

- `median()` - Compute median using nth_element (O(n) average); thread-local scratch buffer, no data races
- `percentile(double p)` - Get p-th percentile [0-100]; thread-local scratch buffer
- `standard_deviation()` - Population standard deviation
- `variance()` - Population variance
- `coefficient_of_variation()` - CV = stddev / mean
- `confidence_interval_95()` - 95% confidence interval (±1.96σ)
- `is_bimodal()` - Detect bimodal distribution (performance instability)
- `histogram(size_t buckets)` - Get frequency distribution; result cached per bucket count (O(1) on repeat calls)
- `to_csv()` - Export as CSV (iteration, duration_ns)
- `to_json()` - Export as JSON with full metadata
- `to_chrome_trace()` - Export for Chrome tracing viewer
- `format(TimeUnit unit)` - Format with custom time unit

---

### ProfileResultWithData<T>

Extends `ProfileResult` with:

- `std::vector<T> return_values` — all values returned by `func`
- **Forwarding methods**: All `ProfileResult` statistical methods are accessible directly
    - Example: `result.median()` instead of `result.profile.median()`

**Move-only types supported**: `std::unique_ptr`, custom move-only types

---

### MemoryStats (Experimental)

Memory profiling structure (requires custom allocator hooks):

| Field               | Description              |
|---------------------|--------------------------|
| `allocations`       | Total allocation count   |
| `deallocations`     | Total deallocation count |
| `bytes_allocated`   | Total bytes allocated    |
| `bytes_deallocated` | Total bytes deallocated  |
| `peak_memory`       | Peak memory usage        |

#### Methods

- `net_allocations()` - Live allocations
- `net_bytes()` - Live memory bytes
- `reset()` - Reset all counters to zero

**Note**: Memory tracking infrastructure is in place but requires integration with a custom allocator or malloc hooks
for actual tracking.

---

### TimeUnit Enum

```cpp
enum class TimeUnit {
    Nanoseconds,
    Microseconds,
    Milliseconds,
    Seconds
};
```

Used for custom formatting via `ProfileResult::format(TimeUnit)`.

---

### Formatting Results

```cpp
std::string summary = profiler::format_result(result);
```

Prints a human-readable report with all statistics, outlier info, and exceptions.

**Custom Unit Formatting:**

```cpp
auto result = profiler::measure(config, func);

// Format in different time units
auto ns = result.format(profiler::TimeUnit::Nanoseconds);
auto us = result.format(profiler::TimeUnit::Microseconds);
auto ms = result.format(profiler::TimeUnit::Milliseconds);
auto s = result.format(profiler::TimeUnit::Seconds);
```

---

## Advanced Usage

### Parallel Profiling

```cpp
config.parallelism = 4;
auto result = profiler::measure(config, []() { /* ... */ });

// Access per-thread statistics
for (size_t i = 0; i < result.per_thread_runs.size(); ++i) {
    std::cout << "Thread " << i << ": " 
              << result.per_thread_runs[i].size() << " runs\n";
}
```

### Outlier Trimming

```cpp
config.trim_outliers_percentage = 10.0; // Trim 10% slowest/fastest
auto result = profiler::measure(config, func);

if (result.outlier_info) {
    std::cout << "Trimmed " << result.outlier_info->trimmed_high 
              << " high and " << result.outlier_info->trimmed_low 
              << " low outliers\n";
}
```

**Note**: Requires at least 20 iterations to enable trimming.

### Exception Handling

Exceptions thrown by `func` are caught and counted per message.

```cpp
auto result = profiler::measure(config, []() {
    if (rand() % 10 == 0) throw std::runtime_error("Random error");
});

for (const auto& [msg, count] : result.unique_exceptions) {
    std::cout << "Exception '" << msg << "': " << count << " times\n";
}
```

### Progress Reporting

```cpp
config.progress_callback = [](double progress) {
    std::cout << "Progress: " << (progress * 100) << "%\r" << std::flush;
};
auto result = profiler::measure(config, func);
```

**Batching**: Progress is reported at an adaptive stride (~1% of total iterations, clamped to [1, 1024]) to balance
granularity vs. contention. On fast workloads with high parallelism, this reduces `callback_mutex` pressure compared to
a fixed stride.

### Memory Profiling (Experimental)

```cpp
config.track_memory = true;
auto result = profiler::measure(config, []() { /* ... */ });
if (result.memory_stats) {
    std::cout << "Allocations: " << result.memory_stats->allocations << "\n";
    std::cout << "Net memory: " << result.memory_stats->net_bytes() << " bytes\n";
    std::cout << "Peak memory: " << result.memory_stats->peak_memory << " bytes\n";
}
```

**Limitation**: Currently requires custom allocator integration or malloc hooks for actual tracking.

---

## 🆕 New Features

### Scoped Profiling (RAII)

Automatically profile code blocks:

```cpp
{
    PROFILE_SCOPE("MyBlock");
    // Code to profile
} // Automatically reports on scope exit

void my_function() {
    PROFILE_FUNCTION(); // Profiles entire function
    // Code here
}

// With custom config
{
    profiler::ProfileConfig cfg;
    cfg.logger = my_logger;
    profiler::ScopedProfiler p("CustomBlock", cfg);
    // Code here
} // Reports via my_logger
```

### Comparison Mode

Compare two benchmark runs with statistical significance:

```cpp
auto baseline = profiler::measure(config, baseline_func);
auto candidate = profiler::measure(config, candidate_func);

auto comparison = profiler::compare(baseline, candidate);
std::cout << profiler::format_comparison(comparison);
```

Output:

```
--- Comparison: Baseline vs Candidate ---
Speedup Factor:   0.850x
Statistical Test: p=0.0023 (significant)
Verdict:          Faster by 15.0%
```

**Statistical Test**: Uses Mann-Whitney U test (non-parametric)

- `p < 0.05` → statistically significant difference
- `speedup_factor < 1.0` → candidate is faster
- `speedup_factor > 1.0` → candidate is slower
- For `n < 30` uses sign-test-based exact approximation (more accurate than normal approx for small samples)

### Enhanced Statistics

```cpp
auto result = profiler::measure(config, func);

// Statistical metrics
auto variance = result.variance();
auto cv = result.coefficient_of_variation();
auto [lower, upper] = result.confidence_interval_95();
bool unstable = result.is_bimodal(); // Detects performance instability

// Distribution analysis
auto hist = result.histogram(10); // 10 buckets
for (size_t i = 0; i < hist.size(); ++i) {
    std::cout << "Bucket " << i << ": " << hist[i] << " samples\n";
}
```

### Data Export

#### CSV Export

```cpp
auto result = profiler::measure(config, func);
std::ofstream("results.csv") << result.to_csv();
```

**Format:**

```
iteration,duration_ns
0,12345
1,12389
2,12301
```

#### JSON Export

```cpp
auto result = profiler::measure(config, func);
auto json = result.to_json();
std::ofstream("results.json") << json.dump(2);
```

**Schema:**

```json
{
  "label": "MyBenchmark",
  "total_duration_ns": 1234567890,
  "average_duration_ns": 12345,
  "min_duration_ns": 10000,
  "max_duration_ns": 15000,
  "iterations_attempted": 100,
  "iterations_succeeded": 98,
  "parallelism_used": 4,
  "warmup_iterations_run": 10,
  "individual_runs_ns": [12345, 12389, ...],
  "unique_exceptions": {
    "error message": 2
  },
  "outlier_info": {
    "trimmed_low": 5,
    "trimmed_high": 5,
    "percentage": 10.0
  }
}
```

#### Chrome Tracing Export

Export results for visualization in Chrome's `chrome://tracing`:

```cpp
auto result = profiler::measure(config, func);
std::ofstream("trace.json") << result.to_chrome_trace();
```

Then:

1. Open Chrome browser
2. Navigate to `chrome://tracing`
3. Click "Load" and select `trace.json`
4. Visualize iteration timings as a timeline

---

## Example: Collecting Return Values

```cpp
auto result = profiler::measure(config, []() -> int { return 42; });
for (int v : result.return_values) {
    // Use measured return values
}
```

---

## Example: Early Stop with Callback

```cpp
auto result = profiler::measure(
    config,
    []() { /* ... */ },
    [&](std::chrono::nanoseconds d, std::size_t i) {
        return i < 10; // Stop after 10 iterations
    }
);
```

---

## 🛠️ Production Usage & Recommendations

### Configuration Best Practices

```cpp
profiler::ProfileConfig cfg;

// 1. Sufficient iterations for statistical validity
cfg.iterations = 1000; // Minimum 100, prefer 1000+

// 2. Warmup to stabilize caches and branch predictors
cfg.warmup_iterations = 100;

// 3. Parallelism for throughput testing
cfg.parallelism = std::thread::hardware_concurrency();

// 4. Outlier trimming for stable metrics
cfg.trim_outliers_percentage = 5.0; // 5% from each end

// 5. Custom logger for integration
cfg.logger = [](std::string_view s) {
    my_logging_system->log(s);
};

// 6. Progress for long-running benchmarks
cfg.progress_callback = [](double p) {
    if (p == 1.0) std::cout << "Complete!\n";
};
```

### Validation Workflow

1. **Validate configuration**:
    - parallelism ≥ 1 and capped to a sane value (e.g., 4× hardware threads)
    - iterations > 0, warmup bounded (avoid accidental huge warmups)
    - outlier trimming in [0, 100]

2. **Keep output target agnostic**:
    - Use `ProfileConfig::logger` for reporting; avoid direct std::cout

3. **Prefer aggregated runs**:
    - Use `parallelism` to saturate CPU, but watch for contention in the profiled code

4. **Use statistical signals**:
    - `variance`, `coefficient_of_variation`, `confidence_interval_95`, `is_bimodal`

5. **Export for tooling**:
    - `to_csv()`, `to_json()`, `to_chrome_trace()` for external analysis

6. **Track regressions**:
    - Store prior results and compare using `profiler::compare` and `format_comparison`

### Recommended Workflow

```cpp
// 1. Warmup → measure with parallelism
profiler::ProfileConfig cfg;
cfg.iterations = 10'000;
cfg.warmup_iterations = 100;
cfg.parallelism = std::thread::hardware_concurrency();
cfg.trim_outliers_percentage = 5.0;

auto result = profiler::measure(cfg, [](){ /* work */ });

// 2. Trim outliers → inspect histogram and percentiles
if (result.outlier_info) {
    std::cout << "Trimmed outliers for cleaner results\n";
}

// 3. Check for performance instability
if (result.is_bimodal()) {
    std::cout << "WARNING: Bimodal distribution detected!\n";
    std::cout << "Check for background processes or thermal throttling.\n";
}

// 4. Export trace → visualize hotspots
std::ofstream("trace.json") << result.to_chrome_trace();

// 5. Compare against baseline → CI gate
auto baseline = load_baseline_from_db();
auto comparison = profiler::compare(baseline, result);
if (comparison.is_significant && comparison.speedup_factor > 1.1) {
    std::cerr << "REGRESSION: " << comparison.verdict << "\n";
    return EXIT_FAILURE;
}
```

---

## ⚙️ Performance Notes

### Algorithm Complexity

- **Median and percentile**: O(n) average using `std::nth_element` instead of O(n log n) full sort
- **Histogram**: O(n) single pass with constant-time bucket assignment
- **Variance/StdDev**: O(n) single pass with online accumulation
- **Per-thread vectors**: Pre-reserved to minimize reallocations

### Overhead Reduction

- **Progress reporting**: Adaptive stride (~1% of total iterations, [1, 1024]) reduces mutex contention
- **Exception tracking**: Uses map with small-string optimization
- **Warmup**: Measured overhead is subtracted from reported times
- **Move semantics**: Move-only return types (e.g., `std::unique_ptr`) are moved without `const_cast`
- **Thread-safe scratch**: `median()`, `percentile()`, `histogram()` use thread-local buffers — no shared mutable state

### Benchmarking the Profiler

The profiler measures its own overhead and reports it. On modern CPUs:

- Empty lambda: ~10-50 ns overhead per iteration
- Function call: ~50-100 ns overhead per iteration
- With return values: ~100-200 ns overhead per iteration

---

## 📊 Interpreting Results

### Coefficient of Variation (CV)

```cpp
double cv = result.coefficient_of_variation();
```

- **CV < 0.05 (5%)**: Excellent stability
- **CV 0.05-0.15 (5-15%)**: Good, acceptable variance
- **CV 0.15-0.30 (15-30%)**: High variance, investigate causes
- **CV > 0.30 (>30%)**: Very high variance, results may be unreliable

### Bimodal Detection

```cpp
if (result.is_bimodal()) {
    // Performance instability detected
}
```

Indicates two distinct performance modes, often caused by:

- CPU frequency scaling / turbo boost
- OS scheduling (context switches)
- Cache/TLB effects
- Background processes

**Recommendation**: Increase iterations and warmup, or run on an isolated system.

### Confidence Intervals

```cpp
auto [lower, upper] = result.confidence_interval_95();
```

The 95% confidence interval means: "We are 95% confident the true mean lies between `lower` and `upper`."

Narrower intervals → more precise measurements.

---

## 🐛 Troubleshooting

### "Median is 0 nanoseconds"

**Cause**: Function executes too fast (sub-nanosecond).

**Solution**: Add measurable work or use return values to prevent optimization.

```cpp
// Bad: may be optimized away
auto result = profiler::measure(config, []() {});

// Good: prevents optimization
auto result = profiler::measure(config, []() {
    volatile int x = 42;
    return x;
});
```

### "Outlier trimming not applied"

**Cause**: Fewer than 20 iterations.

**Solution**: Use at least 20 iterations for outlier trimming.

### "Progress callback not called"

**Cause**: Adaptive batching (stride = ~1% of total iterations) or zero iterations.

**Solution**: With `iterations < 100`, stride is 1 so every iteration calls the callback. With larger counts, use
`progress == 1.0` to detect completion.

### "Memory stats are zero"

**Cause**: Memory tracking requires custom allocator integration.

**Solution**: This is a placeholder for future allocator hook integration.

### "RDTSC measurements are unstable"

**Cause**: CPU frequency scaling or core migration.

**Solution**:

1. Disable frequency scaling (Linux): `sudo cpupower frequency-set -g performance`
2. Pin thread to single core (see above)
3. Increase warmup iterations to stabilize TSC

---

## Notes

- **Profiler overhead** is measured and reported (typically 10-200ns per iteration with steady_clock, 5-20ns with
  RDTSC).
- **Thread safety**: All measurements and callbacks are thread-safe.
- **Minimum iterations for outlier trimming**: 20
- **Memory tracking**: Requires manual `track_allocation`/`track_deallocation` calls or custom allocator integration
- **RDTSC availability**: x86/x64 only, check `PROFILER_HAS_RDTSC` macro
- **Chrome tracing nesting**: Supported via `ChromeTraceBuilder` with parent IDs
