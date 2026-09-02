#pragma once

#include "observability/nadi.hpp"

#include <cstdint>
#include <atomic>
#include <memory>
#include <ostream>
#include <string_view>
#include <tuple>
#include <type_traits>

namespace utils::nadi {
    // ---------------------------------------------------------------------------
    // ChromeTraceSink  [DIAGNOSTIC SINK — not for production hot paths]
    //
    // Serializes any Pulse to Chrome Trace Event Format JSON:
    //   {"name":"<category>","ph":"B","ts":<us>,"pid":0,"tid":0,
    //    "id":<id>,"args":{"field_name":value,...}}
    //
    // Phase mapping:  Begin→"B", End→"E", Instant→"I", Duration→"X", Error→"M"
    // Timestamp unit: microseconds (Chrome Trace convention).
    // Output stream:  caller-supplied std::ostream (e.g. std::cout, ostringstream).
    //
    // Thread safety:  A std::atomic_flag spinlock serialises concurrent writes,
    //   making emit() safe to call from multiple threads simultaneously.
    //   NOTE: std::ostream is not real-time safe — do not use this sink in
    //   audio, physics, or other latency-sensitive threads. Use it for
    //   development, testing, and offline diagnostic traces only.
    //
    // Performance note:
    //   std::ostream + spinlock is appropriate for debugging and test harnesses,
    //   not for high-frequency production instrumentation. For hot paths, use a
    //   buffered binary sink and flush asynchronously.
    // ---------------------------------------------------------------------------

    struct ChromeTraceSink {
        static constexpr bool enabled = true;
        static constexpr Lossless flow_control = {};

        // The ostream to write to. Must be set before first use.
        // Thread-safe: wrapped in atomic to protect concurrent assignments.
        inline static std::atomic<std::ostream*> out = nullptr;

    private:
        inline static std::atomic_flag write_lock_ = ATOMIC_FLAG_INIT;

        struct SpinLockGuard {
            std::atomic_flag& flag_;

            explicit SpinLockGuard(std::atomic_flag& f) noexcept : flag_(f) {
                while (flag_.test_and_set(std::memory_order_acquire))
#if defined(__x86_64__) || defined(_M_X64)
                __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm64__)
                asm volatile("yield" ::: "memory");
#else
                    ;
#endif
            }

            ~SpinLockGuard() noexcept { flag_.clear(std::memory_order_release); }

            SpinLockGuard(const SpinLockGuard&) = delete;

            SpinLockGuard& operator=(const SpinLockGuard&) = delete;
        };

    public:
        inline static std::atomic<std::size_t> write_errors{0};

        static void emit(const auto& pulse) noexcept {
            auto* out_ptr = out.load(std::memory_order_acquire);
            if (!out_ptr) return;
            SpinLockGuard guard(write_lock_);
            try {
                write_event(*out_ptr, pulse);
                if (out_ptr->fail()) {
                    write_errors.fetch_add(1, std::memory_order_relaxed);
                }
            } catch (...) {
                write_errors.fetch_add(1, std::memory_order_relaxed);
            }
        }

    private:
        static constexpr std::string_view phase_char(const PulsePhase p) noexcept {
            switch (p) {
            case PulsePhase::Begin: return "B";
            case PulsePhase::End: return "E";
            case PulsePhase::Instant: return "I";
            case PulsePhase::Duration: return "X";
            case PulsePhase::Error: return "M";
            }
            return "I";
        }

        static void write_json_string(std::ostream& os, const std::string_view sv) {
            os << '"';
            for (const unsigned char c : sv) {
                switch (c) {
                case '"': os << "\\\"";
                    break;
                case '\\': os << "\\\\";
                    break;
                case '\n': os << "\\n";
                    break;
                case '\r': os << "\\r";
                    break;
                case '\t': os << "\\t";
                    break;
                default:
                    if (c < 0x20) {
                        // Hex-escape remaining ASCII control characters.
                        constexpr char hex[] = "0123456789abcdef";
                        os << "\\u00" << hex[c >> 4] << hex[c & 0xf];
                    }
                    else {
                        os << static_cast<char>(c);
                    }
                }
            }
            os << '"';
        }

        template <FixedString Name, typename T>
        static void write_field(std::ostream& os, const Field<Name, T>& f, const bool first) {
            if (!first) os << ',';
            write_json_string(os, Name.view());
            os << ':';
            write_value(os, f.value);
        }

        template <typename T>
        static void write_value(std::ostream& os, const T& v) {
            if constexpr (std::is_same_v<T, bool>) {
                os << (v ? "true" : "false");
            }
            else if constexpr (std::is_integral_v<T>) {
                os << v;
            }
            else if constexpr (std::is_floating_point_v<T>) {
                os << v;
            }
            else if constexpr (std::is_same_v<T, const char*>) {
                if (v) write_json_string(os, v);
                else os << "null";
            }
            else if constexpr (std::is_same_v<T, std::string_view>) {
                write_json_string(os, v);
            }
            else if constexpr (std::is_same_v<T, SourceLocation>) {
                os << R"({"file":)";
                if (v.file) write_json_string(os, v.file);
                else os << "null";
                os << R"(,"line":)" << v.line << '}';
            }
            else {
                os << static_cast<std::uintptr_t>(
                    reinterpret_cast<std::uintptr_t>(std::addressof(v)));
            }
        }

        template <typename PulseType>
        static void write_event(std::ostream& os, const PulseType& pulse) {
            os << R"({"name":)";
            write_json_string(os, PulseType::category.view());
            os << R"(,"ph":")" << phase_char(pulse.phase) << '"';
            os << R"(,"ts":)" << pulse.timestamp_ns / 1000u;
            os << R"(,"pid":0,"tid":0)";
            os << R"(,"id":)" << pulse.id.value;

            // Skip the args block entirely at compile time when the payload is empty
            // (e.g. the bare EndPulse emitted by PulseScope).
            if constexpr (std::tuple_size_v<decltype(pulse.payload)> > 0) {
                os << R"(,"args":{)";
                bool first = true;
                std::apply([&](const auto&... fields) {
                    ([&](const auto& f) {
                        write_field(os, f, first);
                        first = false;
                    }(fields), ...);
                }, pulse.payload);
                os << '}';
            }
            else {
                os << R"(,"args":{})";
            }

            os << "}\n";
        }
    };
} // namespace utils::nadi
