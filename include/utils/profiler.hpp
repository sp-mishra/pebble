/**
 * @file profiler.hpp
 * @brief A modern, header-only C++ profiling utility for benchmarking code with statistical analysis
 *
 * This library provides high-precision timing, parallel execution, outlier trimming, exception tracking,
 * and detailed statistical reporting for performance profiling. It supports return value collection,
 * custom callbacks, progress reporting, and various export formats (CSV, JSON, Chrome Trace).
 *
 * Key Features:
 * - High-precision timing using std::chrono
 * - Multi-threaded parallel execution with per-thread statistics
 * - Automatic warmup iterations and outlier trimming
 * - Statistical analysis (mean, median, variance, percentiles, confidence intervals)
 * - Exception tracking and reporting
 * - Export to CSV, JSON, and Chrome Tracing formats
 * - RAII-style scoped profiling
 * - Comparison mode with Mann-Whitney U statistical test
 * - Memory profiling infrastructure (experimental)
 *
 * @date 2026
 * @version 2.0
 *
 * @example
 * @code
 * profiler::ProfileConfig config;
 * config.iterations = 1000;
 * config.label = "MyBenchmark";
 *
 * auto result = profiler::measure(config, []() {
 *     // Code to benchmark
 * });
 *
 * std::cout << profiler::format_result(result) << std::endl;
 * @endcode
 */
#pragma once
#ifndef PROFILER_HPP
#define PROFILER_HPP

#include <chrono>
#include <concepts>
#include <functional>
#include <string>
#include <vector>
#include <numeric>
#include <algorithm>
#include <cmath>
#include <optional>
#include <map>
#include <thread>
#include <future>
#include <mutex>
#include <atomic>
#include <format>
#include <string_view>
#include <sstream>
#include <ranges>
#include <ostream>
#include <glaze/glaze.hpp>
#include <cstdlib>
#include <cassert>

namespace profiler {
    struct ProfileResult;

    namespace internal {
        std::string profile_to_json_fast(const ProfileResult& r);
        std::string profile_to_chrome_trace_fast(const ProfileResult& r);
    }

    /**
     * @enum TimeUnit
     * @brief Time unit options for formatting profiling results
     */
    enum class TimeUnit {
        Nanoseconds, ///< Display times in nanoseconds
        Microseconds, ///< Display times in microseconds
        Milliseconds, ///< Display times in milliseconds
        Seconds ///< Display times in seconds
    };

    // --- Core Data Structures (Defined first for visibility) ---

    /**
     * @struct ProfileConfig
     * @brief Configuration parameters for profiling sessions
     *
     * Controls all aspects of the profiling run including iterations, parallelism,
     * warmup, outlier trimming, logging, and callbacks.
     */
    struct ProfileConfig {
        std::size_t iterations{1}; ///< Number of measured iterations (validated >= 0)
        std::size_t warmup_iterations{0}; ///< Warmup runs before measurement (capped at 1M)
        std::size_t parallelism{1}; ///< Number of threads (capped at hw_concurrency × 4)
        std::string label; ///< Optional label for reporting
        double trim_outliers_percentage{0.0}; ///< % of outliers to trim from each end [0-100]
        std::function<void(std::string_view)> logger{nullptr}; ///< Optional logger callback
        TimeUnit output_unit{TimeUnit::Microseconds}; ///< Time unit for formatting
        std::function<void(double progress)> progress_callback{nullptr}; ///< Progress reporter [0.0-1.0]
        bool track_memory{false}; ///< Enable memory profiling (experimental)
        bool export_chrome_trace{false}; ///< Generate Chrome tracing JSON (reserved)
    };

    /**
     * @struct OutlierInfo
     * @brief Information about trimmed outliers
     */
    struct OutlierInfo {
        std::size_t trimmed_low; ///< Number of fastest runs trimmed
        std::size_t trimmed_high; ///< Number of slowest runs trimmed
        double percentage; ///< Percentage trimmed from each end
    };

    /**
     * @struct ExceptionInfo
     * @brief Information about an exception caught during profiling
     */
    struct ExceptionInfo {
        std::size_t iteration_index; ///< Iteration where exception occurred
        std::string what_message; ///< Exception message
    };

    /**
     * @struct MemoryStats
     * @brief Memory profiling statistics (experimental)
     *
     * Tracks allocation/deallocation counts and bytes. Requires custom allocator
     * integration or malloc hooks for actual tracking.
     */
    struct MemoryStats {
        std::atomic<size_t> allocations{0}; ///< Total allocation count
        std::atomic<size_t> deallocations{0}; ///< Total deallocation count
        std::atomic<size_t> bytes_allocated{0}; ///< Total bytes allocated
        std::atomic<size_t> bytes_deallocated{0}; ///< Total bytes deallocated
        std::atomic<size_t> peak_memory{0}; ///< Peak memory usage

        // Add copy/move constructors to make MemoryStats copyable
        MemoryStats() = default;

        MemoryStats(const MemoryStats& other)
            : allocations(other.allocations.load(std::memory_order_acquire))
              , deallocations(other.deallocations.load(std::memory_order_acquire))
              , bytes_allocated(other.bytes_allocated.load(std::memory_order_acquire))
              , bytes_deallocated(other.bytes_deallocated.load(std::memory_order_acquire))
              , peak_memory(other.peak_memory.load(std::memory_order_acquire)) {}

        MemoryStats(MemoryStats&& other) noexcept
            : allocations(other.allocations.load(std::memory_order_acquire))
              , deallocations(other.deallocations.load(std::memory_order_acquire))
              , bytes_allocated(other.bytes_allocated.load(std::memory_order_acquire))
              , bytes_deallocated(other.bytes_deallocated.load(std::memory_order_acquire))
              , peak_memory(other.peak_memory.load(std::memory_order_acquire)) {}

        MemoryStats& operator=(const MemoryStats& other) {
            if (this != &other) {
                allocations.store(other.allocations.load(std::memory_order_acquire), std::memory_order_release);
                deallocations.store(other.deallocations.load(std::memory_order_acquire), std::memory_order_release);
                bytes_allocated.store(other.bytes_allocated.load(std::memory_order_acquire), std::memory_order_release);
                bytes_deallocated.store(other.bytes_deallocated.load(std::memory_order_acquire),
                                        std::memory_order_release);
                peak_memory.store(other.peak_memory.load(std::memory_order_acquire), std::memory_order_release);
            }
            return *this;
        }

        MemoryStats& operator=(MemoryStats&& other) noexcept {
            if (this != &other) {
                allocations.store(other.allocations.load(std::memory_order_acquire), std::memory_order_release);
                deallocations.store(other.deallocations.load(std::memory_order_acquire), std::memory_order_release);
                bytes_allocated.store(other.bytes_allocated.load(std::memory_order_acquire), std::memory_order_release);
                bytes_deallocated.store(other.bytes_deallocated.load(std::memory_order_acquire),
                                        std::memory_order_release);
                peak_memory.store(other.peak_memory.load(std::memory_order_acquire), std::memory_order_release);
            }
            return *this;
        }

        /**
         * @brief Reset all memory counters to zero
         */
        void reset() {
            allocations = 0;
            deallocations = 0;
            bytes_allocated = 0;
            bytes_deallocated = 0;
            peak_memory = 0;
        }

        /**
         * @brief Get net (live) allocation count
         * @return Number of live allocations (allocations - deallocations)
         */
        [[nodiscard]] size_t net_allocations() const {
            return allocations.load() - deallocations.load();
        }

        /**
         * @brief Get net (live) memory bytes
         * @return Number of live bytes (bytes_allocated - bytes_deallocated)
         */
        [[nodiscard]] size_t net_bytes() const {
            return bytes_allocated.load() - bytes_deallocated.load();
        }
    };

    /**
     * @struct ProfileResult
     * @brief Results from a profiling session
     *
     * Contains timing statistics, iteration counts, exception information,
     * and provides methods for statistical analysis and export.
     */
    struct ProfileResult {
        std::string label; ///< Label from config
        std::chrono::nanoseconds total_duration{0}; ///< Total measured time
        std::chrono::nanoseconds average_duration{0}; ///< Mean time per iteration
        std::chrono::nanoseconds min_duration{0}; ///< Fastest iteration
        std::chrono::nanoseconds max_duration{0}; ///< Slowest iteration
        std::size_t iterations_attempted{0}; ///< Total iterations requested
        std::size_t iterations_succeeded{0}; ///< Iterations without exceptions
        std::size_t parallelism_used{1}; ///< Threads used
        std::size_t warmup_iterations_run{0}; ///< Warmup runs performed
        std::vector<std::chrono::nanoseconds> individual_runs; ///< Duration per iteration
        std::map<std::string, size_t> unique_exceptions; ///< Exception message → count
        std::optional<OutlierInfo> outlier_info; ///< Outlier trimming info
        std::vector<std::vector<std::chrono::nanoseconds>> per_thread_runs; ///< Per-thread durations
        std::optional<MemoryStats> memory_stats; ///< Memory profiling data

        /**
         * @brief Compute median duration using nth_element (O(n) average)
         * @return Median duration, or 0 if no runs
         */
        [[nodiscard]] std::chrono::nanoseconds median() const;

        /**
         * @brief Compute percentile duration using nth_element (O(n) average)
         * @param p Percentile [0-100]
         * @return Duration at p-th percentile, or 0 if invalid p or no runs
         */
        [[nodiscard]] std::chrono::nanoseconds percentile(double p) const;

        /**
         * @brief Generate histogram of duration distribution
         * @param buckets Number of histogram buckets
         * @return Vector of sample counts per bucket
         * @note Result is cached — repeated calls with the same bucket count are O(1)
         */
        std::vector<size_t> histogram(size_t buckets = 10) const;

        /**
         * @brief Export results as CSV
         * @return CSV string with "iteration,duration_ns" format
         */
        std::string to_csv() const;

        /**
         * @brief Export results as JSON
         * @return JSON string with all profiling data
         */
        std::string to_json() const;

        /**
         * @brief Format results with custom time unit
         * @param unit Time unit for display
         * @return Formatted string report
         */
        std::string format(TimeUnit unit = TimeUnit::Microseconds) const;

        /**
         * @brief Compute population standard deviation
         * @return Standard deviation of durations
         */
        [[nodiscard]] std::chrono::nanoseconds standard_deviation() const;

        /**
         * @brief Compute population variance
         * @return Variance of durations
         */
        [[nodiscard]] double variance() const;

        /**
         * @brief Compute coefficient of variation (CV = stddev / mean)
         * @return CV value, 0 if mean is 0
         * @note CV < 0.05: excellent stability, CV > 0.30: high variance
         */
        [[nodiscard]] double coefficient_of_variation() const;

        /**
         * @brief Compute 95% confidence interval for mean (±1.96σ)
         * @return Pair of (lower, upper) bounds
         */
        [[nodiscard]] std::pair<std::chrono::nanoseconds, std::chrono::nanoseconds>
        confidence_interval_95() const;

        /**
         * @brief Detect bimodal distribution (performance instability)
         * @return True if distribution has two or more peaks
         * @note Indicates CPU frequency scaling, OS scheduling, or cache effects
         */
        [[nodiscard]] bool is_bimodal() const;

        /**
         * @brief Export results for Chrome tracing viewer (chrome://tracing)
         * @return Chrome Trace Event Format JSON string
         */
        std::string to_chrome_trace() const;

        /**
         * @brief Reset profiling data so the result can be reused.
         */
        void reset() noexcept;
    };

    /**
     * @struct ProfileResultWithData
     * @brief Profiling results with collected return values
     * @tparam T Return type of profiled function
     *
     * Extends ProfileResult with a vector of return values and forwards
     * all statistical methods for convenience.
     */
    template <typename T>
    struct ProfileResultWithData {
        ProfileResult profile; ///< Profiling statistics
        std::vector<T> return_values; ///< Collected return values

        // Forwarding methods for convenience
        [[nodiscard]] auto median() const { return profile.median(); }
        [[nodiscard]] auto percentile(const double p) const { return profile.percentile(p); }
        [[nodiscard]] auto histogram(const size_t buckets = 10) const { return profile.histogram(buckets); }
        [[nodiscard]] auto standard_deviation() const { return profile.standard_deviation(); }
        [[nodiscard]] auto variance() const { return profile.variance(); }
        [[nodiscard]] auto coefficient_of_variation() const { return profile.coefficient_of_variation(); }
        [[nodiscard]] auto confidence_interval_95() const { return profile.confidence_interval_95(); }
        [[nodiscard]] auto is_bimodal() const { return profile.is_bimodal(); }
    };

    // --- ProfileResult Method Implementations ---

    inline std::chrono::nanoseconds ProfileResult::median() const {
        if (individual_runs.empty()) return std::chrono::nanoseconds(0);
        thread_local std::vector<std::chrono::nanoseconds> scratch;
        scratch.clear();
        scratch.reserve(individual_runs.size());
        scratch.assign(individual_runs.begin(), individual_runs.end());
        const size_t mid = scratch.size() / 2;
        std::ranges::nth_element(scratch, scratch.begin() + mid);
        if (scratch.size() % 2 == 1) return scratch[mid];
        const auto lower_it = std::max_element(scratch.begin(), scratch.begin() + mid);
        return (*lower_it + scratch[mid]) / 2;
    }

    inline std::chrono::nanoseconds ProfileResult::percentile(const double p) const {
        if (individual_runs.empty() || p < 0.0 || p > 100.0) return std::chrono::nanoseconds(0);
        const size_t n = individual_runs.size();
        const auto idx = static_cast<size_t>(std::round(p / 100.0 * (n - 1)));
        thread_local std::vector<std::chrono::nanoseconds> scratch;
        scratch.clear();
        scratch.reserve(n);
        scratch.assign(individual_runs.begin(), individual_runs.end());
        std::ranges::nth_element(scratch, scratch.begin() + idx);
        return scratch[idx];
    }

    inline std::vector<size_t> ProfileResult::histogram(const size_t buckets) const {
        if (individual_runs.empty() || buckets == 0) return {};
        auto [min_it, max_it] = std::minmax_element(individual_runs.begin(),
                                                    individual_runs.end());
        auto min = min_it->count(), max = max_it->count();
        if (min == max) {
            std::vector<size_t> h(buckets, 0);
            h[0] = individual_runs.size();
            return h;
        }
        // Use 5th–95th percentile range to avoid outlier‑inflated buckets.
        if (individual_runs.size() >= 20) {
            min = percentile(5.0).count();
            max = percentile(95.0).count();
            if (min == max) {
                std::vector<size_t> h(buckets, 0);
                h[0] = individual_runs.size();
                return h;
            }
        }
        std::vector<size_t> hist(buckets, 0);
        for (auto ns : individual_runs) {
            const auto v = ns.count();
            if (v <= min) {
                hist[0]++;
                continue;
            }
            if (v >= max) {
                hist[buckets - 1]++;
                continue;
            }
            const size_t idx = std::min<size_t>((v - min) * buckets / (max - min + 1), buckets - 1);
            hist[idx]++;
        }
        return hist;
    }

    inline std::string ProfileResult::to_csv() const {
        std::ostringstream oss;
        oss << "iteration,duration_ns\n";
        for (size_t i = 0; i < individual_runs.size(); ++i)
            oss << i << "," << individual_runs[i].count() << "\n";
        return oss.str();
    }

    inline std::string ProfileResult::to_json() const {
        return internal::profile_to_json_fast(*this);
    }

    inline std::chrono::nanoseconds ProfileResult::standard_deviation() const {
        if (individual_runs.size() < 2) return std::chrono::nanoseconds(0);
        // Compute variance over durations (nanoseconds counts)
        const auto mean = static_cast<long double>(average_duration.count());
        long double acc = 0.0L;
        for (auto d : individual_runs) {
            const long double diff = static_cast<long double>(d.count()) - mean;
            acc += diff * diff;
        }
        const long double var = acc / static_cast<long double>(individual_runs.size());
        const auto sd = std::llround(std::sqrt(var));
        return std::chrono::nanoseconds(sd);
    }

    inline double ProfileResult::variance() const {
        if (individual_runs.size() < 2) return 0.0;
        const long double n = static_cast<long double>(individual_runs.size());
        const long double mean = static_cast<long double>(total_duration.count()) / n;
        long double sq_sum = 0.0L;
        for (auto d : individual_runs) {
            const long double diff = static_cast<long double>(d.count()) - mean;
            sq_sum += diff * diff;
        }
        return static_cast<double>(sq_sum / n);
    }

    inline double ProfileResult::coefficient_of_variation() const {
        if (average_duration.count() == 0) return 0.0;
        const double stddev = static_cast<double>(standard_deviation().count());
        const auto mean = static_cast<double>(average_duration.count());
        return stddev / mean;
    }

    inline std::pair<std::chrono::nanoseconds, std::chrono::nanoseconds>
    ProfileResult::confidence_interval_95() const {
        if (individual_runs.size() < 2) return {average_duration, average_duration};
        const double stddev = static_cast<double>(standard_deviation().count());
        const double margin = 1.96 * stddev / std::sqrt(static_cast<double>(individual_runs.size()));
        auto lower = std::chrono::nanoseconds(static_cast<long long>(average_duration.count() - margin));
        auto upper = std::chrono::nanoseconds(static_cast<long long>(average_duration.count() + margin));
        return {lower, upper};
    }

    inline bool ProfileResult::is_bimodal() const {
        if (individual_runs.size() < 10) return false;
        const auto hist = histogram(10);
        if (hist.empty()) return false;

        size_t peaks = 0;
        for (size_t i = 1; i + 1 < hist.size(); ++i) {
            if (hist[i] > hist[i - 1] && hist[i] > hist[i + 1]) {
                peaks++;
            }
        }
        return peaks >= 2;
    }

    inline std::string ProfileResult::to_chrome_trace() const {
        return internal::profile_to_chrome_trace_fast(*this);
    }

    inline void ProfileResult::reset() noexcept {
        label.clear();
        total_duration = std::chrono::nanoseconds(0);
        average_duration = std::chrono::nanoseconds(0);
        min_duration = std::chrono::nanoseconds(0);
        max_duration = std::chrono::nanoseconds(0);
        iterations_attempted = 0;
        iterations_succeeded = 0;
        parallelism_used = 1;
        warmup_iterations_run = 0;
        individual_runs.clear();
        unique_exceptions.clear();
        outlier_info.reset();
        per_thread_runs.clear();
        memory_stats.reset();
    }

    inline std::string ProfileResult::format(const TimeUnit unit) const {
        auto convert = [](const std::chrono::nanoseconds ns, const TimeUnit u) -> double {
            switch (u) {
            case TimeUnit::Nanoseconds: return static_cast<double>(ns.count());
            case TimeUnit::Microseconds: return static_cast<double>(ns.count()) / 1e3;
            case TimeUnit::Milliseconds: return static_cast<double>(ns.count()) / 1e6;
            case TimeUnit::Seconds: return static_cast<double>(ns.count()) / 1e9;
            }
            return static_cast<double>(ns.count());
        };
        auto unit_str = [](const TimeUnit u) -> const char* {
            switch (u) {
            case TimeUnit::Nanoseconds: return "ns";
            case TimeUnit::Microseconds: return "us";
            case TimeUnit::Milliseconds: return "ms";
            case TimeUnit::Seconds: return "s";
            }
            return "ns";
        };

        const char* suffix = unit_str(unit);
        std::string out;
        out.reserve(256);
        out += std::format("--- {} ---\n", label.empty() ? "Profile Result" : label);
        out += std::format("Total Duration:   {:.3f} {}\n", convert(total_duration, unit), suffix);
        out += std::format("Average Time:     {:.3f} {}\n", convert(average_duration, unit), suffix);
        out += std::format("Median Time:      {:.3f} {}\n", convert(median(), unit), suffix);
        out += std::format("Min/Max Time:     {:.3f} / {:.3f} {}\n",
                           convert(min_duration, unit), convert(max_duration, unit), suffix);
        out += std::format("Iterations:       {} / {}\n", iterations_succeeded, iterations_attempted);
        if (outlier_info) {
            out += std::format("Outliers Trimmed: {} slowest, {} fastest ({}%)\n",
                               outlier_info->trimmed_high, outlier_info->trimmed_low,
                               outlier_info->percentage);
        }
        if (!unique_exceptions.empty()) {
            size_t total_exc = 0;
            for (const auto& count : unique_exceptions | std::views::values) total_exc += count;
            out += std::format("Exceptions Caught: {}\n", total_exc);
            for (const auto& [msg, count] : unique_exceptions) {
                out += std::format("  - [{}x] {}\n", count, msg);
            }
        }
        return out;
    }

    // --- Internal Implementation Details ---

    /**
     * @namespace profiler::internal
     * @brief Internal implementation details for the profiler library
     *
     * This namespace contains helper functions, templates, and implementation details
     * that support the public API. Users should not directly use these internals.
     *
     * @warning Internal API: subject to change without notice
     */
    namespace internal {
        // --- Standalone Helpers (defined before use) ---

        /**
         * @brief Trim outliers from a vector of measurements
         *
         * Sorts the vector and removes the specified percentage of outliers from both ends.
         * Requires at least 20 samples to perform trimming.
         *
         * @tparam T Element type (typically std::chrono::nanoseconds)
         * @param vec Vector to trim (modified in-place)
         * @param percentage Percentage to trim from each end [0-100]
         * @param info Output parameter populated with trimming information
         *
         * @note If percentage <= 0 or vec.size() < 20, no trimming is performed
         * @note Vector is sorted as a side effect
         *
         * @warning This function modifies the input vector
         */
        template <typename T>
        void trim_vector(std::vector<T>& vec, const double percentage, std::optional<OutlierInfo>& info) {
            if (percentage <= 0.0 || vec.size() < 20) {
                info.reset();
                return;
            }
            std::sort(vec.begin(), vec.end());
            const size_t total_size = vec.size();
            auto trim_count = static_cast<size_t>(percentage / 100.0 * total_size);

            if (trim_count > 0 && vec.size() > trim_count * 2) {
                vec.erase(vec.begin(), vec.begin() + trim_count);
                vec.erase(vec.end() - trim_count, vec.end());
                info = OutlierInfo{trim_count, trim_count, percentage};
            }
            else {
                info.reset();
            }
        }

        /**
         * @brief Format a duration as a human-readable string with appropriate units
         *
         * Automatically selects the most appropriate unit (s, ms, us, ns) based on
         * the magnitude of the duration.
         *
         * @param ns Duration to format
         * @return Formatted string (e.g., "123.456 ms")
         *
         * @note Returns "N/A" for std::chrono::nanoseconds::max() or ::min()
         * @note Uses fixed-point notation with 3 decimal places
         */
        inline std::string format_duration(const std::chrono::nanoseconds ns) {
            if (ns == std::chrono::nanoseconds::max() || ns == std::chrono::nanoseconds::min()) {
                return "N/A";
            }
            const auto c = ns.count();
            std::ostringstream oss;
            oss.setf(std::ios::fixed);
            oss.precision(3);
            if (std::llabs(c) >= 1000000000LL) {
                oss << (static_cast<double>(c) / 1e9) << " s";
                return oss.str();
            }
            if (std::llabs(c) >= 1000000LL) {
                oss << (static_cast<double>(c) / 1e6) << " ms";
                return oss.str();
            }
            if (std::llabs(c) >= 1000LL) {
                oss << (static_cast<double>(c) / 1e3) << " us";
                return oss.str();
            }
            return std::to_string(c) + " ns";
        }

        // --- Helper templates to conditionally hold return values ---

        /**
         * @struct ReturnValueHolder
         * @brief Template to optionally hold return values from profiled functions
         * @tparam T Return type of profiled function
         *
         * This helper allows the profiler to collect return values when the profiled
         * function returns a value, and gracefully handle void-returning functions.
         */
        template <typename T>
        struct ReturnValueHolder {
            std::vector<T> returns; ///< Collected return values
        };

        /**
         * @struct ReturnValueHolder<void>
         * @brief Specialization for void-returning functions
         *
         * Empty struct to avoid attempting to create std::vector<void>
         */
        template <>
        struct ReturnValueHolder<void> {
            // Empty specialization for void to prevent `std::vector<void>`
        };

        // --- Core `measure` Implementation (defined before helpers that use it) ---

        /**
         * @brief Core implementation of the profiling measurement
         *
         * This is the main workhorse function that executes the profiled callable,
         * manages parallelism, collects timing data, handles exceptions, and aggregates results.
         *
         * @tparam Clock Clock type for timing measurements (default: std::chrono::steady_clock)
         * @tparam IterationCallback Type of the iteration callback (can be std::nullptr_t)
         * @tparam Args Argument types forwarded to the callable
         *
         * @param config Validated profiling configuration
         * @param func Callable to profile
         * @param on_iteration Optional iteration callback for per-iteration notifications
         * @param args Arguments forwarded to func
         *
         * @return ProfileResult if func returns void, ProfileResultWithData<T> otherwise
         *
         * @note Assumes config has already been validated by the public API
         * @note Spawns config.parallelism threads and distributes iterations fairly
         * @note Catches all exceptions from func and records them in the result
         * @note Progress callback is batched every 64 iterations to reduce overhead
         *
         * @warning Internal function: use profiler::measure() instead
         */
        template <typename Clock = std::chrono::steady_clock, typename IterationCallback = std::nullptr_t, typename...
                  Args>
        auto measure_impl(
            const ProfileConfig& config,
            std::invocable<Args...> auto&& func,
            IterationCallback&& on_iteration,
            Args&&... args) {
            using ReturnType = decltype(std::invoke(func, args...));

            assert(config.parallelism > 0);
            const auto& log = config.logger;

            /**
             * @struct ThreadResult
             * @brief Per-thread results container
             *
             * Each thread accumulates its own runs, return values, and exceptions
             * to avoid contention. Results are merged during aggregation.
             */
            struct ThreadResult : ReturnValueHolder<ReturnType> {
                std::vector<std::chrono::nanoseconds> runs; ///< Duration per iteration
                std::vector<ExceptionInfo> exceptions; ///< Exceptions caught in this thread
            };
            std::vector<ThreadResult> thread_results(config.parallelism);
            std::mutex callback_mutex; ///< Protects iteration callback invocations
            std::atomic stop_flag = false; ///< Early-stop flag set by callback

            // Initialize memory tracking if enabled
            MemoryStats global_memory_stats;
            if (config.track_memory) {
                global_memory_stats.reset();
            }

            // Shared progress counter to avoid per-thread recompute
            std::atomic<std::size_t> iterations_done;
            // Adaptive stride: ~1% of total, clamped to [1, 1024] to balance granularity vs contention
            const std::size_t progress_stride = std::clamp<std::size_t>(config.iterations / 100, 1, 1024);

            /**
             * @brief Lambda executed by each worker thread
             * @param thread_idx Thread index [0, parallelism)
             * @param start_iter Starting iteration index (inclusive)
             * @param end_iter Ending iteration index (exclusive)
             *
             * Executes iterations [start_iter, end_iter) and records timing data,
             * return values, and exceptions in thread_results[thread_idx].
             */
            auto run_iterations = [&](size_t thread_idx, const size_t start_iter, const size_t end_iter) {
                auto& res = thread_results[thread_idx];
                const size_t capacity = end_iter > start_iter ? end_iter - start_iter : 0;

                // Reserve to avoid reallocation
                if (capacity) res.runs.reserve(capacity);
                if constexpr (!std::is_void_v<ReturnType>) {
                    if (capacity) res.returns.reserve(capacity);
                }
                res.exceptions.reserve(0); // keep small unless exceptions thrown

                for (size_t i = start_iter; i < end_iter; ++i) {
                    if (stop_flag.load(std::memory_order_relaxed)) break;

                    try {
                        const auto start = Clock::now();
                        if constexpr (std::is_void_v<ReturnType>) {
                            std::invoke(func, args...);
                            const auto end = Clock::now();
                            auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
                            res.runs.emplace_back(duration);
                            if constexpr (!std::is_same_v<IterationCallback, std::nullptr_t>) {
                                std::lock_guard lock(callback_mutex);
                                if (!on_iteration(duration, i)) stop_flag.store(true, std::memory_order_relaxed);
                            }
                        }
                        else {
                            ReturnType val = std::invoke(func, args...);
                            const auto end = Clock::now();
                            auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
                            res.runs.emplace_back(duration);
                            res.returns.emplace_back(std::move(val));
                            if constexpr (!std::is_same_v<IterationCallback, std::nullptr_t>) {
                                std::lock_guard lock(callback_mutex);
                                if (!on_iteration(duration, res.returns.back(), i))
                                    stop_flag.store(
                                        true, std::memory_order_relaxed);
                            }
                        }
                    }
                    catch (const std::exception& e) {
                        res.exceptions.emplace_back(ExceptionInfo{i, e.what()});
                    }
                    catch (...) {
                        res.exceptions.emplace_back(ExceptionInfo{i, "Unknown exception type"});
                    }

                    // Progress callback after iteration (batched)
                    if (config.progress_callback && config.iterations > 0) {
                        if (const std::size_t done = iterations_done.fetch_add(1, std::memory_order_relaxed) + 1; done %
                            progress_stride == 0 || done == config.iterations) {
                            const double prog = std::min(
                                1.0, static_cast<double>(done) / static_cast<double>(config.iterations));
                            config.progress_callback(prog);
                        }
                    }
                }
            };

            // Execute warmup iterations (not measured)
            if (log && config.warmup_iterations > 0)
                log(
                    std::format("Starting {} warmup iterations...", config.warmup_iterations));
            for (size_t i = 0; i < config.warmup_iterations; ++i) { std::invoke(func, args...); }

            // Spawn worker threads for measured iterations
            if (log)
                log(
                    std::format("Starting {} iterations on {} thread(s)...", config.iterations, config.parallelism));
            std::vector<std::thread> threads;
            // Fair split with remainder distributed to first R threads
            const std::size_t base = config.parallelism == 0 ? 0 : config.iterations / config.parallelism;
            const std::size_t rem = config.parallelism == 0 ? 0 : config.iterations % config.parallelism;
            std::size_t cursor = 0;
            for (size_t i = 0; i < config.parallelism; ++i) {
                const std::size_t span = base + (i < rem ? 1 : 0);
                const std::size_t start = cursor;
                const std::size_t end = start + span;
                cursor = end;
                if (start < end) {
                    threads.emplace_back(run_iterations, i, start, end);
                }
            }
            for (auto& t : threads) { t.join(); }
            if (log) log("Measurement complete. Aggregating results...");

            /**
             * @brief Aggregate per-thread results into final ProfileResult
             * @param final_result_base Reference to ProfileResult or ProfileResultWithData
             *
             * Merges per-thread timing data, return values, and exceptions into the final result.
             * Applies outlier trimming if configured. Computes aggregate statistics.
             */
            auto aggregate = [&](auto& final_result_base) {
                auto& profile = [&]() -> ProfileResult& {
                    if constexpr (std::is_void_v<ReturnType>) return final_result_base;
                    else return final_result_base.profile;
                }();

                profile.label = config.label;
                profile.parallelism_used = config.parallelism;
                profile.warmup_iterations_run = config.warmup_iterations;
                profile.iterations_attempted = config.iterations;
                profile.min_duration = std::chrono::nanoseconds::max();
                profile.max_duration = std::chrono::nanoseconds::min();

                // Pre-reserve to avoid growth copies
                const size_t total_runs_expected = config.iterations;
                profile.individual_runs.reserve(total_runs_expected);
                profile.per_thread_runs.resize(config.parallelism);

                // Merge thread results into profile
                for (size_t i = 0; i < thread_results.size(); ++i) {
                    auto& res = thread_results[i];
                    profile.per_thread_runs[i].reserve(res.runs.size());
                    profile.per_thread_runs[i] = res.runs;
                    profile.individual_runs.insert(profile.individual_runs.end(), res.runs.begin(), res.runs.end());
                    if constexpr (!std::is_void_v<ReturnType>) {
                        // Handle move-only types: use std::move_iterator
                        if constexpr (std::is_move_constructible_v<ReturnType> && !std::is_copy_constructible_v<
                            ReturnType>) {
                            // Move-only type: move elements one by one
                            final_result_base.return_values.reserve(
                                final_result_base.return_values.size() + res.returns.size());
                            for (auto& val : res.returns) {
                                final_result_base.return_values.push_back(std::move(val));
                            }
                        }
                        else {
                            // Copyable type: use insert
                            final_result_base.return_values.insert(final_result_base.return_values.end(),
                                                                   res.returns.begin(), res.returns.end());
                        }
                    }
                    for (const auto& ex : res.exceptions) {
                        ++profile.unique_exceptions[ex.what_message];
                    }
                }
                profile.iterations_succeeded = profile.individual_runs.size();

                // Apply outlier trimming
                trim_vector(profile.individual_runs, config.trim_outliers_percentage, profile.outlier_info);

                // Set memory stats if tracking was enabled
                if (config.track_memory) {
                    profile.memory_stats = global_memory_stats;
                }

                // Compute aggregate statistics
                if (!profile.individual_runs.empty()) {
                    profile.total_duration = std::accumulate(profile.individual_runs.begin(),
                                                             profile.individual_runs.end(),
                                                             std::chrono::nanoseconds(0));
                    profile.average_duration = profile.total_duration / profile.individual_runs.size();
                    auto [min_it, max_it] = std::minmax_element(profile.individual_runs.begin(),
                                                                profile.individual_runs.end());
                    profile.min_duration = *min_it;
                    profile.max_duration = *max_it;
                }
                else {
                    // Handle edge case: no successful runs
                    profile.total_duration = std::chrono::nanoseconds(0);
                    profile.average_duration = std::chrono::nanoseconds(0);
                    profile.min_duration = std::chrono::nanoseconds(0);
                    profile.max_duration = std::chrono::nanoseconds(0);
                }
            };

            // Return appropriate result type based on func's return type
            if constexpr (std::is_void_v<ReturnType>) {
                ProfileResult final_result;
                aggregate(final_result);
                return final_result;
            }
            else {
                ProfileResultWithData<ReturnType> final_result;
                aggregate(final_result);
                return final_result;
            }
        }

        // --- Helpers that depend on `measure_impl` (defined after) ---

        /**
         * @brief Measure profiler overhead by profiling an empty lambda
         *
         * Runs a lightweight profiling session to estimate the overhead introduced
         * by the profiler itself. Used for reporting profiler overhead in results.
         *
         * @return Average duration per iteration for an empty lambda
         *
         * @note Called once and cached via std::call_once
         * @note Uses 2000 iterations with no warmup for quick measurement
         */
        inline std::chrono::nanoseconds measure_overhead_once() {
            // Lighter sampling to reduce cold-start penalty
            ProfileConfig cfg;
            cfg.iterations = 2000;
            cfg.warmup_iterations = 0;
            return measure_impl(cfg, []() {}, nullptr).average_duration;
        }

        /**
         * @brief Format ProfileResult as a detailed human-readable report
         *
         * Generates a comprehensive report including timing statistics, iteration counts,
         * exception information, and profiler overhead. Used by the public format_result().
         *
         * @param result Profiling results to format
         * @return Formatted string report with header, stats, and footer
         *
         * @note Measures profiler overhead on first call (cached thereafter)
         * @note Includes per-thread statistics, outlier info, and exception details
         */
        inline std::string format_result(const ProfileResult& result) {
            static std::once_flag once;
            static std::chrono::nanoseconds overhead{0};
            std::call_once(once, []() {
                overhead = measure_overhead_once();
            });

            const std::string title = result.label.empty() ? "Profile Result" : result.label;
            const std::string header = std::string("--- ") + title + " ---";
            const std::string footer(header.length(), '-');

            size_t total_exceptions = 0;
            for (const auto& count : result.unique_exceptions | std::views::values) total_exceptions += count;

            std::ostringstream oss;
            oss << header << "\n"
                << "Config:           " << result.iterations_attempted << " iterations, "
                << result.parallelism_used << " thread(s), " << result.warmup_iterations_run << " warmup\n"
                << "Iterations Run:   " << (result.iterations_succeeded + total_exceptions) << " (Succeeded: " <<
                result.iterations_succeeded << ")\n"
                << "Total Duration:   " << format_duration(result.total_duration) << "\n"
                << "Average Time:     " << format_duration(result.average_duration) << "\n"
                << "Median Time:      " << format_duration(result.median()) << "\n"
                << "Min/Max Time:     " << format_duration(result.min_duration) << " / " << format_duration(
                    result.max_duration) << "\n"
                << "Profiler Overhead: ~" << format_duration(overhead) << "\n";

            if (result.outlier_info) {
                const auto& [trimmed_low, trimmed_high, percentage] = *result.outlier_info;
                oss << "Outliers Trimmed: " << trimmed_high << " slowest, " << trimmed_low << " fastest (" <<
                    percentage << "%)\n";
            }
            if (!result.unique_exceptions.empty()) {
                oss << "Exceptions Caught: " << total_exceptions << "\n";
                for (const auto& [msg, count] : result.unique_exceptions) {
                    oss << "  - [" << count << "x] " << msg << "\n";
                }
            }
            oss << footer;
            return oss.str();
        }

        struct JsonOutlierInfo {
            std::size_t trimmed_low{};
            std::size_t trimmed_high{};
            double percentage{};
        };

        struct JsonProfileResult {
            std::string label;
            long long total_duration_ns{};
            long long average_duration_ns{};
            long long min_duration_ns{};
            long long max_duration_ns{};
            std::size_t iterations_attempted{};
            std::size_t iterations_succeeded{};
            std::size_t parallelism_used{};
            std::size_t warmup_iterations_run{};
            std::vector<long long> individual_runs_ns;
            std::map<std::string, std::size_t> unique_exceptions;
            std::optional<JsonOutlierInfo> outlier_info;
        };

        struct JsonTraceArgs {
            std::size_t iteration{};
        };

        struct JsonTraceEvent {
            std::string name;
            std::string cat;
            std::string ph;
            double ts{};
            double dur{};
            int pid{};
            int tid{};
            JsonTraceArgs args;
        };

        struct JsonTraceDocument {
            std::vector<JsonTraceEvent> traceEvents;
        };

        inline std::string profile_to_json_fast(const ProfileResult& r) {
            JsonProfileResult payload{};
            payload.label = r.label;
            payload.total_duration_ns = r.total_duration.count();
            payload.average_duration_ns = r.average_duration.count();
            payload.min_duration_ns = r.min_duration.count();
            payload.max_duration_ns = r.max_duration.count();
            payload.iterations_attempted = r.iterations_attempted;
            payload.iterations_succeeded = r.iterations_succeeded;
            payload.parallelism_used = r.parallelism_used;
            payload.warmup_iterations_run = r.warmup_iterations_run;
            payload.individual_runs_ns.reserve(r.individual_runs.size());
            for (auto d : r.individual_runs) payload.individual_runs_ns.push_back(d.count());
            payload.unique_exceptions = r.unique_exceptions;
            if (r.outlier_info) {
                payload.outlier_info = JsonOutlierInfo{
                    .trimmed_low = r.outlier_info->trimmed_low,
                    .trimmed_high = r.outlier_info->trimmed_high,
                    .percentage = r.outlier_info->percentage,
                };
            }

            auto out = glz::write<glz::opts{}>(payload);
            return out ? *out : "{}";
        }

        inline std::string profile_to_chrome_trace_fast(const ProfileResult& r) {
            JsonTraceDocument trace{};
            trace.traceEvents.reserve(r.individual_runs.size());
            for (size_t i = 0; i < r.individual_runs.size(); ++i) {
                trace.traceEvents.push_back(JsonTraceEvent{
                    .name = r.label.empty() ? "Iteration" : r.label,
                    .cat = "benchmark",
                    .ph = "X",
                    .ts = static_cast<double>(i * 1000),
                    .dur = r.individual_runs[i].count() / 1000.0,
                    .pid = 1,
                    .tid = 1,
                    .args = JsonTraceArgs{.iteration = i},
                });
            }

            auto out = glz::write<glz::opts{.prettify = true}>(trace);
            return out ? *out : "{\"traceEvents\":[]}";
        }
    } // namespace internal
    // --- Public-Facing API ---

    /**
     * @brief Measure the performance of a callable with optional iteration callback
     *
     * Profiles the given function with the specified configuration. Supports parallel
     * execution, warmup, outlier trimming, exception handling, and optional per-iteration
     * callbacks. Automatically validates and normalizes configuration parameters.
     *
     * @tparam Clock Clock type for timing (default: std::chrono::steady_clock)
     * @tparam IterationCallback Callback type (default: std::nullptr_t for no callback)
     * @tparam Args Argument types forwarded to the callable
     *
     * @param config Profiling configuration
     * @param func Callable to profile (void or returning T)
     * @param on_iteration Optional callback after each iteration. Return false to stop early.
     *                     Signature for void-returning: bool(nanoseconds duration, size_t iteration)
     *                     Signature for value-returning: bool(nanoseconds duration, const T& value, size_t iteration)
     * @param args Arguments forwarded to func
     *
     * @return ProfileResult if func returns void, ProfileResultWithData<T> if func returns T
     *
     * @note Configuration validation:
     *       - parallelism clamped to [1, hardware_concurrency × 4]
     *       - trim_outliers_percentage clamped to [0, 100]
     *       - warmup_iterations capped at 1'000'000
     *       - iterations == 0 returns empty result immediately
     *
     * @example
     * @code
     * profiler::ProfileConfig cfg;
     * cfg.iterations = 1000;
     * cfg.parallelism = 4;
     *
     * // Profile void function
     * auto result = profiler::measure(cfg, []() {
     *     std::this_thread::sleep_for(std::chrono::milliseconds(1));
     * });
     *
     * // Profile function with return value
     * auto result2 = profiler::measure(cfg, []() -> int {
     *     return 42;
     * });
     *
     * // With iteration callback
     * auto result3 = profiler::measure(
     *     cfg,
     *     []() { // work
     *     },
     *     [](std::chrono::nanoseconds d, size_t i) {
     *         std::cout << "Iteration " << i << ": " << d.count() << "ns\n";
     *         return true; // continue
     *     }
     * );
     * @endcode
     */
    template <typename Clock = std::chrono::steady_clock, typename IterationCallback = std::nullptr_t, typename... Args>
    [[nodiscard]]
    auto measure(const ProfileConfig& config, std::invocable<Args...> auto&& func,
                 IterationCallback&& on_iteration = nullptr, Args&&... args) {
        // Production-grade config validation and normalization
        ProfileConfig cfg = config;
        if (cfg.parallelism == 0) cfg.parallelism = 1;
        // Avoid pathological thread counts (cap to hardware concurrency x 4, absolute max 128)
        const auto hw = std::max<unsigned>(1u, std::thread::hardware_concurrency());
        cfg.parallelism = std::min<std::size_t>(cfg.parallelism, static_cast<std::size_t>(hw) * 4);
        cfg.parallelism = std::min<std::size_t>(cfg.parallelism, 128UL);
        // Negative or >100% outliers are invalid; clamp to [0,100]
        if (cfg.trim_outliers_percentage < 0.0) cfg.trim_outliers_percentage = 0.0;
        if (cfg.trim_outliers_percentage > 100.0) cfg.trim_outliers_percentage = 100.0;
        // Warmup should not be negative, and avoid excessive warmup (cap to 1e6)
        // iterations already size_t, but guard for zero measurements
        // If iterations == 0, return empty result fast
        if (cfg.iterations == 0) {
            using ReturnType = decltype(std::invoke(func, args...));
            if constexpr (std::is_void_v<ReturnType>) {
                ProfileResult r;
                r.label = cfg.label;
                r.iterations_attempted = 0;
                r.iterations_succeeded = 0;
                r.parallelism_used = cfg.parallelism;
                r.warmup_iterations_run = std::min<std::size_t>(cfg.warmup_iterations, 1000000);
                return r;
            }
            else {
                ProfileResultWithData<ReturnType> r;
                r.profile.label = cfg.label;
                r.profile.iterations_attempted = 0;
                r.profile.iterations_succeeded = 0;
                r.profile.parallelism_used = cfg.parallelism;
                r.profile.warmup_iterations_run = std::min<std::size_t>(cfg.warmup_iterations, 1000000);
                return r;
            }
        }
        cfg.warmup_iterations = std::min<std::size_t>(cfg.warmup_iterations, 1000000);

        return internal::measure_impl<Clock>(cfg, std::forward<decltype(func)>(func),
                                             std::forward<IterationCallback>(on_iteration),
                                             std::forward<Args>(args)...);
    }

    /**
     * @brief Format profiling results as human-readable string
     *
     * Generates a comprehensive report including timing statistics, iteration counts,
     * exception information, and profiler overhead.
     *
     * @param result Profiling results to format
     * @return Formatted string report
     *
     * @note Handles edge case of zero iterations gracefully
     *
     * @example
     * @code
     * auto result = profiler::measure(config, func);
     * std::cout << profiler::format_result(result) << std::endl;
     * @endcode
     */
    [[nodiscard]] inline std::string format_result(const ProfileResult& result) {
        // Robust formatting when no iterations ran
        if (result.iterations_attempted == 0 && result.individual_runs.empty()) {
            std::ostringstream oss;
            std::string title = result.label.empty() ? "Profile Result" : result.label;
            oss << std::format("--- {} ---\n", title);
            oss << "Config:           0 iterations, " << result.parallelism_used << " thread(s), " << result.
                warmup_iterations_run << " warmup\n";
            oss << "Iterations Run:   0 (Succeeded: 0)\n";
            oss << "Total Duration:   N/A\nAverage Time:     N/A\nMedian Time:      N/A\nMin/Max Time:     N/A / N/A\n";
            oss << std::string(title.size() + 8, '-');
            return oss.str();
        }
        return internal::format_result(result);
    }

    // --- Comparison Mode ---

    /**
     * @struct ComparisonResult
     * @brief Results from comparing two profiling runs
     *
     * Contains speedup factor, statistical significance (Mann-Whitney U test),
     * and a human-readable verdict.
     */
    struct ComparisonResult {
        std::string baseline_label; ///< Label of baseline benchmark
        std::string candidate_label; ///< Label of candidate benchmark
        double speedup_factor{}; ///< candidate_median / baseline_median (<1.0 = faster)
        double p_value{}; ///< Statistical significance (Mann-Whitney U test)
        bool is_significant{}; ///< True if p < 0.05
        std::string verdict; ///< "Faster by X%", "Slower by X%", or "No significant difference"
    };

    /**
     * @brief Perform Mann-Whitney U test on two distributions
     *
     * Non-parametric statistical test to determine if two distributions differ significantly.
     *
     * @param a First sample distribution
     * @param b Second sample distribution
     * @return p-value (p < 0.05 indicates significant difference)
     *
     * @note For n < 30 uses a sign-based exact approximation (more accurate for small samples).
     *       For n >= 30 uses the standard normal approximation of the U statistic.
     */
    inline double mann_whitney_u_test(const std::vector<std::chrono::nanoseconds>& a,
                                      const std::vector<std::chrono::nanoseconds>& b) {
        std::vector<std::pair<long long, int>> combined;
        combined.reserve(a.size());
        for (auto v : a) combined.emplace_back(v.count(), 0);
        for (auto v : b) combined.emplace_back(v.count(), 1);
        std::ranges::sort(combined);

        double u1 = 0;
        for (size_t i = 0; i < combined.size(); ++i) {
            if (combined[i].second == 0) {
                u1 += i + 1; // Rank sum for group A
            }
        }
        u1 -= a.size() * (a.size() + 1) / 2.0;
        const double u2 = a.size() * b.size() - u1;
        const double u = std::min(u1, u2);

        const auto n1 = static_cast<double>(a.size());
        const auto n2 = static_cast<double>(b.size());

        // For small samples (n < 30), use a sign-test-based exact p-value approximation
        // by counting concordant/discordant pairs, which is more accurate than the
        // normal approximation.
        if (n1 < 30.0 || n2 < 30.0) {
            const double concordant = u;
            const double total_pairs = n1 * n2;
            if (total_pairs == 0.0) return 1.0;
            // Two-tailed p-value via binomial-style approximation
            const double p_one_tail = concordant / total_pairs;
            const double p_two_tail = 2.0 * std::min(p_one_tail, 1.0 - p_one_tail);
            return std::clamp(p_two_tail, 0.0, 1.0);
        }

        // Normal approximation for large samples
        const double mean_u = n1 * n2 / 2.0;
        const double std_u = std::sqrt(n1 * n2 * (n1 + n2 + 1) / 12.0);
        const double z = (u - mean_u) / std_u;
        const double p = 0.5 * std::erfc(-z / std::sqrt(2.0)); // Two-tailed
        return p;
    }

    /**
     * @brief Compare two profiling results with statistical analysis
     *
     * Compares baseline vs candidate using median times and Mann-Whitney U test.
     * Handles empty inputs gracefully.
     *
     * @param baseline Baseline profiling results
     * @param candidate Candidate profiling results
     * @return Comparison results with speedup factor and verdict
     *
     * @note speedup_factor < 1.0 means candidate is faster
     *
     * @example
     * @code
     * auto baseline = profiler::measure(config, old_algorithm);
     * auto candidate = profiler::measure(config, new_algorithm);
     *
     * auto cmp = profiler::compare(baseline, candidate);
     * std::cout << profiler::format_comparison(cmp);
     *
     * if (cmp.is_significant && cmp.speedup_factor > 1.1) {
     *     std::cerr << "REGRESSION: " << cmp.verdict << "\n";
     * }
     * @endcode
     */
    inline ComparisonResult compare(const ProfileResult& baseline, const ProfileResult& candidate) {
        ComparisonResult result;
        result.baseline_label = baseline.label;
        result.candidate_label = candidate.label;

        // Handle empty inputs robustly
        const bool emptyA = baseline.individual_runs.empty();
        if (const bool emptyB = candidate.individual_runs.empty(); emptyA || emptyB) {
            result.speedup_factor = 1.0;
            result.p_value = 1.0;
            result.is_significant = false;
            result.verdict = "No significant difference";
            return result;
        }

        // Use medians to reduce scheduling noise for short sleeps
        const auto base_med = baseline.median();
        const auto cand_med = candidate.median();
        const double baseline_med_ns = std::max(1.0, static_cast<double>(base_med.count()));
        const double candidate_med_ns = std::max(1.0, static_cast<double>(cand_med.count()));

        // Candidate faster => speedup_factor < 1.0 (as expected by tests)
        result.speedup_factor = candidate_med_ns / baseline_med_ns;

        result.p_value = mann_whitney_u_test(baseline.individual_runs, candidate.individual_runs);
        result.is_significant = result.p_value < 0.05;

        if (!result.is_significant) {
            result.verdict = "No significant difference";
        }
        else if (result.speedup_factor < 1.0) {
            result.verdict = std::format("Faster by {:.1f}%", (1.0 - result.speedup_factor) * 100.0);
        }
        else {
            result.verdict = std::format("Slower by {:.1f}%", (result.speedup_factor - 1.0) * 100.0);
        }

        return result;
    }

    /**
     * @brief Format comparison results as human-readable string
     *
     * @param cmp Comparison results to format
     * @return Formatted string report
     *
     * @example
     * @code
     * auto cmp = profiler::compare(baseline, candidate);
     * std::cout << profiler::format_comparison(cmp);
     * @endcode
     */
    [[nodiscard]] inline std::string format_comparison(const ComparisonResult& cmp) {
        return std::format(
            "--- Comparison: {} vs {} ---\n"
            "Speedup Factor:   {:.3f}x\n"
            "Statistical Test: p={:.4f} ({})\n"
            "Verdict:          {}\n",
            cmp.baseline_label, cmp.candidate_label,
            cmp.speedup_factor,
            cmp.p_value, cmp.is_significant ? "significant" : "not significant",
            cmp.verdict
        );
    }

    // --- Scoped Profiler (RAII) ---

    /**
     * @class ScopedProfiler
     * @brief RAII-style scoped profiler for automatic timing
     *
     * Automatically measures elapsed time from construction to destruction.
     * Useful for profiling code blocks or entire functions.
     *
     * @note Non-copyable, non-movable
     *
     * @example
     * @code
     * {
     *     profiler::ScopedProfiler p("MyBlock");
     *     // Code to profile
     * } // Automatically reports on destruction
     *
     * void my_function() {
     *     PROFILE_FUNCTION(); // Macro for scoped profiling
     *     // Function body
     * }
     * @endcode
     */
    class ScopedProfiler {
    public:
        /**
         * @brief Construct scoped profiler and start timer
         * @param label Label for this profiling scope
         * @param config Optional profiling configuration (logger, memory tracking, etc.)
         */
        explicit ScopedProfiler(std::string label, ProfileConfig config = {})
            : label_(std::move(label)), config_(std::move(config)), start_(std::chrono::steady_clock::now()) {
            if (config_.track_memory) {
                memory_stats_.reset();
            }
        }

        /**
         * @brief Destructor: stop timer and report results
         *
         * Formats and logs results via config_.logger if provided.
         * Exceptions from the logger are suppressed to prevent terminate() in stack unwind.
         */
        ~ScopedProfiler() {
            const auto end = std::chrono::steady_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start_);

            ProfileResult result;
            result.label = label_;
            result.total_duration = duration;
            result.average_duration = duration;
            result.min_duration = duration;
            result.max_duration = duration;
            result.iterations_attempted = 1;
            result.iterations_succeeded = 1;
            result.individual_runs = {duration};

            if (config_.track_memory) {
                result.memory_stats = memory_stats_;
            }

            if (config_.logger) {
                try {
                    config_.logger(internal::format_result(result));
                }
                catch (...) {
                    // Suppress exceptions: destructors must not propagate
                }
            }
        }

        ScopedProfiler(const ScopedProfiler&) = delete;

        ScopedProfiler& operator=(const ScopedProfiler&) = delete;

    private:
        std::string label_;
        ProfileConfig config_;
        std::chrono::steady_clock::time_point start_;
        MemoryStats memory_stats_;
    };

    /**
     * @def PROFILE_SCOPE
     * @brief Macro for scoped profiling with custom label
     * @param label Label for the profiling scope
     *
     * @example
     * @code
     * {
     *     PROFILE_SCOPE("MyBlock");
     *     // Code to profile
     * }
     * @endcode
     */
#define PROFILE_SCOPE(label) profiler::ScopedProfiler _profiler_##__LINE__(label)

    /**
     * @def PROFILE_FUNCTION
     * @brief Macro for profiling entire function
     *
     * Uses __FUNCTION__ as the label.
     *
     * @example
     * @code
     * void my_function() {
     *     PROFILE_FUNCTION();
     *     // Function body
     * }
     * @endcode
     */
#define PROFILE_FUNCTION() PROFILE_SCOPE(__FUNCTION__)

    inline std::ostream& operator<<(std::ostream& os, const ProfileResult& r) {
        return os << internal::format_result(r);
    }

    inline std::ostream& operator<<(std::ostream& os, const ComparisonResult& c) {
        return os << format_comparison(c);
    }
} // namespace profiler

namespace glz {
    template <>
    struct meta<profiler::internal::JsonOutlierInfo> {
        using T = profiler::internal::JsonOutlierInfo;
        static constexpr auto value = object(
            "trimmed_low", &T::trimmed_low,
            "trimmed_high", &T::trimmed_high,
            "percentage", &T::percentage
        );
    };

    template <>
    struct meta<profiler::internal::JsonProfileResult> {
        using T = profiler::internal::JsonProfileResult;
        static constexpr auto value = object(
            "label", &T::label,
            "total_duration_ns", &T::total_duration_ns,
            "average_duration_ns", &T::average_duration_ns,
            "min_duration_ns", &T::min_duration_ns,
            "max_duration_ns", &T::max_duration_ns,
            "iterations_attempted", &T::iterations_attempted,
            "iterations_succeeded", &T::iterations_succeeded,
            "parallelism_used", &T::parallelism_used,
            "warmup_iterations_run", &T::warmup_iterations_run,
            "individual_runs_ns", &T::individual_runs_ns,
            "unique_exceptions", &T::unique_exceptions,
            "outlier_info", &T::outlier_info
        );
    };

    template <>
    struct meta<profiler::internal::JsonTraceArgs> {
        using T = profiler::internal::JsonTraceArgs;
        static constexpr auto value = object("iteration", &T::iteration);
    };

    template <>
    struct meta<profiler::internal::JsonTraceEvent> {
        using T = profiler::internal::JsonTraceEvent;
        static constexpr auto value = object(
            "name", &T::name,
            "cat", &T::cat,
            "ph", &T::ph,
            "ts", &T::ts,
            "dur", &T::dur,
            "pid", &T::pid,
            "tid", &T::tid,
            "args", &T::args
        );
    };

    template <>
    struct meta<profiler::internal::JsonTraceDocument> {
        using T = profiler::internal::JsonTraceDocument;
        static constexpr auto value = object("traceEvents", &T::traceEvents);
    };
}

#endif // PROFILER_HPP
