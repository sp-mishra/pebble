#pragma once

// samasa/tooling/render_markdown.hpp — Markdown renderer for grammar models.
//
// C++23, header-only, no virtual, no macros. Namespace: lang::samasa
//
// render_markdown(grammar_description<G>)          — rule list as Markdown table.
// render_markdown(grammar_railroad_model<G>)        — railroad entries as Markdown table.
// render_markdown(grammar_first_sets<G>)            — FIRST/nullable table as Markdown.
// render_markdown(grammar_follow_sets<G>)           — FOLLOW sets table as Markdown.
// render_markdown(grammar_validation_result<N>)     — validation issues as Markdown table.
//
// All functions return std::string (runtime — renderers are not consteval by design).

#include <string>
#include <string_view>
#include "describe.hpp"
#include "railroad.hpp"
#include "../grammar/expected_sets.hpp"
#include "../grammar/validation.hpp"

namespace lang::samasa {
    // ---- render_markdown(grammar_description<G>) ---------------------------

    template <class G>
    [[nodiscard]] std::string render_markdown(const grammar_description<G>&) {
        std::string out;
        out.reserve(256 + G::rule_count * 64);
        out += "# Grammar: ";
        out += G::root_rule::name_sv;
        out += "\n\n";
        out += "| # | Rule |\n|---|------|\n";
        for (const auto& d : grammar_description<G>::rules) {
            out += "| ";
            out += std::to_string(d.index);
            out += " | `";
            out += d.name;
            out += "` |\n";
        }
        return out;
    }

    // ---- render_markdown(grammar_railroad_model<G>) ------------------------

    template <class G>
    [[nodiscard]] std::string render_markdown(const grammar_railroad_model<G>&) {
        std::string out;
        out.reserve(256 + G::rule_count * 80);
        out += "# Railroad Model: ";
        out += G::root_rule::name_sv;
        out += "\n\n";
        out += "| Rule | Shape | Nullable |\n|------|-------|----------|\n";
        for (const auto& e : grammar_railroad_model<G>::entries) {
            out += "| `";
            out += e.rule_name;
            out += "` | ";
            out += e.shape;
            out += " | ";
            out += e.nullable ? "yes" : "no";
            out += " |\n";
        }
        return out;
    }

    // ---- render_markdown(grammar_first_sets<G>) ----------------------------

    template <class G>
    [[nodiscard]] std::string render_markdown(const grammar_first_sets<G>&) {
        std::string out;
        out.reserve(256 + G::rule_count * 64);
        out += "# FIRST Sets: ";
        out += G::root_rule::name_sv;
        out += "\n\n";
        out += "| Rule | Nullable |\n|------|----------|\n";
        for (const auto& d : grammar_first_sets<G>::descriptors) {
            out += "| `";
            out += d.name;
            out += "` | ";
            out += d.nullable ? "yes" : "no";
            out += " |\n";
        }
        return out;
    }

    // ---- render_markdown(grammar_follow_sets<G>) ---------------------------

    template <class G>
    [[nodiscard]] std::string render_markdown(const grammar_follow_sets<G>&) {
        std::string out;
        out.reserve(256 + G::rule_count * 80);
        out += "# FOLLOW Sets: ";
        out += G::root_rule::name_sv;
        out += "\n\n";
        out += "| Rule | Has EOF | Token Count |\n|------|---------|-------------|\n";
        for (const auto& e : grammar_follow_sets<G>::entries) {
            out += "| `";
            out += e.name;
            out += "` | ";
            out += e.has_eof ? "yes" : "no";
            out += " | ";
            out += std::to_string(e.token_count);
            out += " |\n";
        }
        return out;
    }

    // ---- render_markdown(grammar_validation_result<N>) ---------------------

    namespace rm_detail {
        [[nodiscard]] inline std::string_view severity_str(grammar_issue_severity s) noexcept {
            switch (s) {
            case grammar_issue_severity::error: return "error";
            case grammar_issue_severity::warning: return "warning";
            case grammar_issue_severity::note: return "note";
            }
            return "unknown";
        }

        [[nodiscard]] inline std::string_view diag_code_str(grammar_diag_code c) noexcept {
            switch (c) {
            case grammar_diag_code::empty_many: return "SAMASA-GRAMMAR-EMPTY-MANY";
            case grammar_diag_code::duplicate_rule: return "SAMASA-GRAMMAR-DUPLICATE-RULE";
            case grammar_diag_code::unreachable_rule: return "SAMASA-GRAMMAR-UNREACHABLE-RULE";
            case grammar_diag_code::left_recursion: return "SAMASA-GRAMMAR-LEFT-RECURSION";
            case grammar_diag_code::unknown_ref: return "SAMASA-GRAMMAR-UNKNOWN-REF";
            case grammar_diag_code::duplicate_operator: return "SAMASA-GRAMMAR-DUPLICATE-OP";
            case grammar_diag_code::bad_pratt_table: return "SAMASA-GRAMMAR-BAD-PRATT-TABLE";
            case grammar_diag_code::nullable_root: return "SAMASA-GRAMMAR-NULLABLE-ROOT";
            case grammar_diag_code::choice_shadowing: return "SAMASA-GRAMMAR-CHOICE-SHADOW";
            case grammar_diag_code::empty_separator: return "SAMASA-GRAMMAR-EMPTY-SEP";
            case grammar_diag_code::choice_first_overlap: return "SAMASA-GRAMMAR-CHOICE-OVERLAP";
            case grammar_diag_code::recovery_no_progress: return "SAMASA-GRAMMAR-RECOVERY-NO-PROGRESS";
            }
            return "SAMASA-GRAMMAR-UNKNOWN";
        }
    }

    template <std::size_t N>
    [[nodiscard]] std::string render_markdown(const grammar_validation_result<N>& result) {
        std::string out;
        out.reserve(128 + N * 96);
        out += "# Grammar Validation\n\n";
        if constexpr (N == 0) {
            out += "_No issues found._\n";
            return out;
        }
        out += "| Code | Severity | Rule |\n|------|----------|------|\n";
        for (const auto& issue : result.issues) {
            out += "| `";
            out += rm_detail::diag_code_str(issue.code);
            out += "` | ";
            out += rm_detail::severity_str(issue.severity);
            out += " | `";
            out += issue.rule;
            out += "` |\n";
        }
        out += '\n';
        out += result.ok() ? "_Validation passed._\n" : "_Validation failed._\n";
        return out;
    }
} // namespace lang::samasa
