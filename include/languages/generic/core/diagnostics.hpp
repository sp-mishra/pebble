#pragma once

// generic/diagnostics.hpp — Generic diagnostic framework for language frontends.
//
// C++23, header-only, no virtual, no macros. Namespace: lang
//
// Provides a template-based diagnostic framework that all language frontends
// and the generic layer itself use for error reporting. Zero external dependencies
// (only <cstdint>, <string>, <string_view>, <vector>).
//
// severity    — note/warning/error/fatal ordered enum.
// lang_diagnostic<DiagKind>
//             — generic diagnostic record. DiagKind must provide:
//                 static std::string_view to_code(DiagKind) noexcept;
// collecting_sink<Diag>
//             — accumulates diagnostics; reports has_errors() / has_warnings().
// diagnostic_view<Diag>
//             — lightweight non-owning span over a collected sink's entries.
//
// Usage:
//   enum class my_code { not_found, mismatch };
//   using my_diag = lang::lang_diagnostic<my_code>;
//   lang::collecting_sink<my_diag> sink;
//   sink.on_diagnostic({my_code::not_found, "sym", "not found"});
//   assert(sink.has_errors());

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lang {
    // =========================================================================
    // severity
    // =========================================================================

    enum class severity : std::uint8_t {
        note = 0,
        warning = 1,
        error = 2,
        fatal = 3,
    };

    [[nodiscard]] constexpr std::string_view to_string(severity s) noexcept {
        switch (s) {
        case severity::note: return "note";
        case severity::warning: return "warning";
        case severity::error: return "error";
        case severity::fatal: return "fatal";
        }
        return "unknown";
    }

    // =========================================================================
    // lang_diagnostic<DiagKind>
    //
    // DiagKind contract:
    //   static std::string_view to_code(DiagKind) noexcept;
    // =========================================================================

    template <class DiagKind>
    struct lang_diagnostic {
        DiagKind kind = {};
        std::string symbol; // affected symbol / module name
        std::string message; // human-readable detail
        severity level = severity::error;

        [[nodiscard]] bool is_error() const noexcept { return level >= severity::error; }
        [[nodiscard]] bool is_warning() const noexcept { return level == severity::warning; }
        [[nodiscard]] bool is_fatal() const noexcept { return level == severity::fatal; }

        [[nodiscard]] std::string_view code() const noexcept {
            return DiagKind::to_code(kind);
        }
    };

    // =========================================================================
    // collecting_sink<Diag>
    // =========================================================================

    template <class Diag>
    class collecting_sink {
    public:
        std::vector<Diag> entries;

        void on_diagnostic(Diag d) { entries.push_back(std::move(d)); }

        [[nodiscard]] bool empty() const noexcept { return entries.empty(); }
        [[nodiscard]] std::size_t size() const noexcept { return entries.size(); }

        [[nodiscard]] bool has_errors() const noexcept {
            for (const auto& d : entries)
                if (d.is_error()) return true;
            return false;
        }

        [[nodiscard]] bool has_warnings() const noexcept {
            for (const auto& d : entries)
                if (d.is_warning()) return true;
            return false;
        }

        // Highest severity level across all collected diagnostics.
        // Returns severity::note for an empty sink.
        [[nodiscard]] severity max_severity() const noexcept {
            severity m = severity::note;
            for (const auto& d : entries)
                if (d.level > m) m = d.level;
            return m;
        }

        void clear() noexcept { entries.clear(); }

        // Truncate to n entries — used by parse_context::rollback to undo
        // diagnostics emitted in a failed alternative.
        void truncate(std::size_t n) noexcept {
            if (n < entries.size()) entries.resize(n);
        }
    };

    // =========================================================================
    // diagnostic_view<Diag>
    // Non-owning read-only view over a collecting_sink's entries.
    // =========================================================================

    template <class Diag>
    struct diagnostic_view {
        std::span<const Diag> entries;

        explicit diagnostic_view(const collecting_sink<Diag>& sink) noexcept
            : entries(sink.entries) {}

        [[nodiscard]] bool empty() const noexcept { return entries.empty(); }
        [[nodiscard]] std::size_t size() const noexcept { return entries.size(); }

        [[nodiscard]] bool has_errors() const noexcept {
            for (const auto& d : entries)
                if (d.is_error()) return true;
            return false;
        }

        [[nodiscard]] auto begin() const noexcept { return entries.begin(); }
        [[nodiscard]] auto end() const noexcept { return entries.end(); }
    };
} // namespace lang
