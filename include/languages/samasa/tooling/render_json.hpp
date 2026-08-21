#pragma once

// samasa/tooling/render_json.hpp — JSON renderer for grammar models.
//
// C++23, header-only, no virtual, no macros. Namespace: lang::samasa
//
// render_json(grammar_description<G>)            — rule list as JSON.
// render_json(grammar_railroad_model<G>)          — railroad entries as JSON.
// render_json(grammar_first_sets<G>)              — FIRST/nullable table as JSON.
// render_json(grammar_follow_sets<G>)             — FOLLOW sets as JSON.
// render_json(grammar_validation_result<N>)       — validation issues as JSON.
//
// All functions return std::string (runtime — renderers are not consteval).
// Output is compact JSON (no trailing newline after closing bracket).

#include <string>
#include "describe.hpp"
#include "railroad.hpp"
#include "../grammar/expected_sets.hpp"
#include "../grammar/validation.hpp"

namespace lang::samasa {

    namespace rj_detail {
        [[nodiscard]] inline std::string json_str(std::string_view s) {
            std::string out;
            out.reserve(s.size() + 2);
            out += '"';
            for (char c : s) {
                if (c == '"') out += "\\\"";
                else if (c == '\\') out += "\\\\";
                else out += c;
            }
            out += '"';
            return out;
        }

        [[nodiscard]] inline std::string_view severity_str(grammar_issue_severity s) noexcept {
            switch (s) {
                case grammar_issue_severity::error:   return "error";
                case grammar_issue_severity::warning: return "warning";
                case grammar_issue_severity::note:    return "note";
            }
            return "unknown";
        }

        [[nodiscard]] inline std::string_view diag_code_str(grammar_diag_code c) noexcept {
            switch (c) {
                case grammar_diag_code::empty_many:             return "SAMASA-GRAMMAR-EMPTY-MANY";
                case grammar_diag_code::duplicate_rule:         return "SAMASA-GRAMMAR-DUPLICATE-RULE";
                case grammar_diag_code::unreachable_rule:       return "SAMASA-GRAMMAR-UNREACHABLE-RULE";
                case grammar_diag_code::left_recursion:         return "SAMASA-GRAMMAR-LEFT-RECURSION";
                case grammar_diag_code::unknown_ref:            return "SAMASA-GRAMMAR-UNKNOWN-REF";
                case grammar_diag_code::duplicate_operator:     return "SAMASA-GRAMMAR-DUPLICATE-OP";
                case grammar_diag_code::bad_pratt_table:        return "SAMASA-GRAMMAR-BAD-PRATT-TABLE";
                case grammar_diag_code::nullable_root:          return "SAMASA-GRAMMAR-NULLABLE-ROOT";
                case grammar_diag_code::choice_shadowing:       return "SAMASA-GRAMMAR-CHOICE-SHADOW";
                case grammar_diag_code::empty_separator:        return "SAMASA-GRAMMAR-EMPTY-SEP";
                case grammar_diag_code::choice_first_overlap:   return "SAMASA-GRAMMAR-CHOICE-OVERLAP";
                case grammar_diag_code::recovery_no_progress:   return "SAMASA-GRAMMAR-RECOVERY-NO-PROGRESS";
            }
            return "SAMASA-GRAMMAR-UNKNOWN";
        }
    }

    // ---- render_json(grammar_description<G>) -------------------------------

    template <class G>
    [[nodiscard]] std::string render_json(const grammar_description<G>&) {
        std::string out;
        out.reserve(128 + G::rule_count * 48);
        out += "{\"grammar\":";
        out += rj_detail::json_str(G::root_rule::name_sv);
        out += ",\"rules\":[";
        bool first = true;
        for (const auto& d : grammar_description<G>::rules) {
            if (!first) out += ',';
            first = false;
            out += "{\"index\":";
            out += std::to_string(d.index);
            out += ",\"name\":";
            out += rj_detail::json_str(d.name);
            out += '}';
        }
        out += "]}";
        return out;
    }

    // ---- render_json(grammar_railroad_model<G>) ----------------------------

    template <class G>
    [[nodiscard]] std::string render_json(const grammar_railroad_model<G>&) {
        std::string out;
        out.reserve(128 + G::rule_count * 80);
        out += "{\"grammar\":";
        out += rj_detail::json_str(G::root_rule::name_sv);
        out += ",\"railroad\":[";
        bool first = true;
        for (const auto& e : grammar_railroad_model<G>::entries) {
            if (!first) out += ',';
            first = false;
            out += "{\"rule\":";
            out += rj_detail::json_str(e.rule_name);
            out += ",\"shape\":";
            out += rj_detail::json_str(e.shape);
            out += ",\"nullable\":";
            out += e.nullable ? "true" : "false";
            out += '}';
        }
        out += "]}";
        return out;
    }

    // ---- render_json(grammar_first_sets<G>) --------------------------------

    template <class G>
    [[nodiscard]] std::string render_json(const grammar_first_sets<G>&) {
        std::string out;
        out.reserve(128 + G::rule_count * 64);
        out += "{\"grammar\":";
        out += rj_detail::json_str(G::root_rule::name_sv);
        out += ",\"first_sets\":[";
        bool first = true;
        for (const auto& d : grammar_first_sets<G>::descriptors) {
            if (!first) out += ',';
            first = false;
            out += "{\"rule\":";
            out += rj_detail::json_str(d.name);
            out += ",\"nullable\":";
            out += d.nullable ? "true" : "false";
            out += '}';
        }
        out += "]}";
        return out;
    }

    // ---- render_json(grammar_follow_sets<G>) -------------------------------

    template <class G>
    [[nodiscard]] std::string render_json(const grammar_follow_sets<G>&) {
        std::string out;
        out.reserve(128 + G::rule_count * 64);
        out += "{\"grammar\":";
        out += rj_detail::json_str(G::root_rule::name_sv);
        out += ",\"follow_sets\":[";
        bool first = true;
        for (const auto& e : grammar_follow_sets<G>::entries) {
            if (!first) out += ',';
            first = false;
            out += "{\"rule\":";
            out += rj_detail::json_str(e.name);
            out += ",\"has_eof\":";
            out += e.has_eof ? "true" : "false";
            out += ",\"token_count\":";
            out += std::to_string(e.token_count);
            out += '}';
        }
        out += "]}";
        return out;
    }

    // ---- render_json(grammar_validation_result<N>) -------------------------

    template <std::size_t N>
    [[nodiscard]] std::string render_json(const grammar_validation_result<N>& result) {
        std::string out;
        out.reserve(64 + N * 96);
        out += "{\"ok\":";
        out += result.ok() ? "true" : "false";
        out += ",\"issues\":[";
        if constexpr (N > 0) {
            bool first = true;
            for (const auto& issue : result.issues) {
                if (!first) out += ',';
                first = false;
                out += "{\"code\":";
                out += rj_detail::json_str(rj_detail::diag_code_str(issue.code));
                out += ",\"severity\":";
                out += rj_detail::json_str(rj_detail::severity_str(issue.severity));
                out += ",\"rule\":";
                out += rj_detail::json_str(issue.rule);
                out += '}';
            }
        }
        out += "]}";
        return out;
    }

} // namespace lang::samasa
