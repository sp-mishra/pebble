#pragma once

// generic/core/rich_diagnostic.hpp — Structured rich diagnostic records.
//
// C++23, header-only, no virtual, no macros. Namespace: lang
//
// Provides a language-agnostic rich diagnostic format with primary/secondary
// source spans, expected/found fields, notes, and help lines. A fluent builder
// (explain) keeps call sites terse. Depends only on source_location.hpp.
//
// Types:
//   diag_severity     — error / warning / note
//   diag_label        — secondary labelled span
//   diag_explanation  — full structured diagnostic record
//   explain           — fluent builder for diag_explanation
//
// Usage:
//   auto e = lang::explain("MY-001", "type mismatch", span)
//                .expected("i32").found("f64")
//                .note("inferred from return type")
//                .help("add explicit cast")
//                .build();
//   sink.on_diagnostic(to_vakya_diag(e));   // language glue not included here

#include "languages/generic/core/source_location.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lang {
    // =========================================================================
    // diag_severity
    // =========================================================================

    enum class diag_severity : std::uint8_t { error, warning, note };

    [[nodiscard]] constexpr std::string_view to_string(diag_severity s) noexcept {
        switch (s) {
        case diag_severity::error: return "error";
        case diag_severity::warning: return "warning";
        case diag_severity::note: return "note";
        }
        return "error";
    }

    // =========================================================================
    // diag_label — secondary span with an explanatory label
    // =========================================================================

    struct diag_label {
        source_span at;
        std::string text;
    };

    // =========================================================================
    // diag_explanation — structured diagnostic record
    //
    // render_message() → "<code>: <summary>" (flat, compatible with legacy sinks).
    // render_full()    → multi-line with expected/found, labels, notes, help.
    // =========================================================================

    struct diag_explanation {
        std::string code; // e.g. "MY-GEN-001"
        std::string summary; // headline (no code prefix)
        source_span primary{};

        std::string expected; // optional ("" = n/a)
        std::string found; // optional ("" = n/a)

        std::vector<diag_label> secondary;
        std::vector<std::string> notes;
        std::vector<std::string> help;

        diag_severity severity = diag_severity::error;

        [[nodiscard]] std::string render_message() const {
            if (summary.empty()) return code;
            if (code.empty()) return summary;
            return code + ": " + summary;
        }

        [[nodiscard]] std::string render_full() const {
            std::string out;
            out += std::string(to_string(severity));
            out += '[';
            out += code;
            out += "]: ";
            out += summary;

            if (!expected.empty() || !found.empty()) {
                out += "\n  expected: ";
                out += expected.empty() ? "<n/a>" : expected;
                out += "\n  found:    ";
                out += found.empty() ? "<n/a>" : found;
            }
            for (const auto& n : notes) {
                out += "\n  note: ";
                out += n;
            }
            for (const auto& l : secondary) {
                out += "\n  --> line ";
                out += std::to_string(l.at.line);
                out += ':';
                out += std::to_string(l.at.col);
                out += ": ";
                out += l.text;
            }
            for (const auto& h : help) {
                out += "\n  help: ";
                out += h;
            }
            return out;
        }
    };

    // =========================================================================
    // explain — fluent builder for diag_explanation
    //
    //   auto e = lang::explain("MY-001", "bad type", span)
    //                .expected("T").found("U")
    //                .note("constraint from line 5")
    //                .help("add impl")
    //                .build();
    // =========================================================================

    struct explain {
        diag_explanation e;

        explain(std::string code, std::string summary, source_span at) {
            e.code = std::move(code);
            e.summary = std::move(summary);
            e.primary = at;
        }

        // lvalue chaining
        explain& expected(std::string v) & {
            e.expected = std::move(v);
            return *this;
        }

        explain& found(std::string v) & {
            e.found = std::move(v);
            return *this;
        }

        explain& note(std::string v) & {
            e.notes.push_back(std::move(v));
            return *this;
        }

        explain& help(std::string v) & {
            e.help.push_back(std::move(v));
            return *this;
        }

        explain& label(source_span at, std::string text) & {
            e.secondary.push_back({at, std::move(text)});
            return *this;
        }

        explain& severity(diag_severity s) & {
            e.severity = s;
            return *this;
        }

        // rvalue chaining (builder used as temporary)
        explain&& expected(std::string v) && {
            e.expected = std::move(v);
            return std::move(*this);
        }

        explain&& found(std::string v) && {
            e.found = std::move(v);
            return std::move(*this);
        }

        explain&& note(std::string v) && {
            e.notes.push_back(std::move(v));
            return std::move(*this);
        }

        explain&& help(std::string v) && {
            e.help.push_back(std::move(v));
            return std::move(*this);
        }

        explain&& label(source_span at, std::string text) && {
            e.secondary.push_back({at, std::move(text)});
            return std::move(*this);
        }

        explain&& severity(diag_severity s) && {
            e.severity = s;
            return std::move(*this);
        }

        [[nodiscard]] diag_explanation build() && { return std::move(e); }
        [[nodiscard]] diag_explanation build() const & { return e; }
    };
} // namespace lang
