#pragma once
// =============================================================================
// medha/diagnostics.hpp — MEDHA-* diagnostic codes via lithe::diag
//
// C++23, header-only, no virtual, no macros.
//
// Reuses lithe::diag (severity/stage/string_code/source_span/sinks).
// Medha registers the "medha" code namespace.
// Zero cost when lithe::diag is absent (__has_include guard).
// =============================================================================

#include <string_view>

#if __has_include("edsl/lithe_diagnostics/lithe_diagnostics.hpp")
#  include "edsl/lithe_diagnostics/lithe_diagnostics.hpp"
#  define MEDHA_HAS_LITHE_DIAG 1
#endif

namespace medha::diag {
    // ============================================================================
    // Diagnostic code constants (string codes, extensible)
    // ============================================================================

    inline constexpr std::string_view kIllegalEffect = "MEDHA-004";
    inline constexpr std::string_view kConflictRetry = "MEDHA-TRY-002";
    inline constexpr std::string_view kSerializableUnavailable = "MEDHA-SER-011";
    inline constexpr std::string_view kPartialCommit = "MEDHA-CMT-020";
    inline constexpr std::string_view kAbaUnsafe = "MEDHA-ABA-030";
    inline constexpr std::string_view kOomAbort = "MEDHA-OOM-040";

    // ============================================================================
    // Simple diagnostic record (usable without lithe::diag)
    // ============================================================================

    enum class severity : std::uint8_t { note, info, warning, error, fatal };

    struct diagnostic {
        severity level = severity::error;
        std::string_view code{};
        std::string_view message{};
        std::string_view hint{};
    };
} // namespace medha::diag
