#pragma once

// samasa/core/diagnostic.hpp — Samasa diagnostic codes + rich diagnostic types.
//
// C++23, header-only, no virtual, no macros. Namespace: lang::samasa
//
// samasa_diag_code  — u16 enum; each value maps to a SAMASA-*-* string via to_code().
// diagnostic        — alias for lang::lang_diagnostic<samasa_diag_code>.
//
// v2 rich diagnostic types:
//   expected_item     — one entry in an expected set (token_kind, token_text, rule, eof).
//   expected_set<N>   — fixed-capacity compile-time or runtime expected set.
//   parse_diagnostic  — structured parse error with expected set, actual token, repair info.
//
// Grammar-validation failures (SAMASA-GRAMMAR-*) are emitted as static_assert messages,
// not runtime diagnostics; they share the naming scheme for consistency.

#include <cstdint>
#include <string_view>
#include <array>
#include "languages/generic/core/diagnostics.hpp"
#include "source_view.hpp"

namespace lang::samasa {

    enum class samasa_diag_code : std::uint16_t {
        // Lexer
        lex_unknown_char        = 0,
        lex_unterminated_string = 1,
        lex_bad_number          = 2,
        lex_unterminated_comment= 3,

        // Parser
        parse_unexpected_token  = 10,
        parse_expected          = 11,
        parse_missing           = 12,
        parse_depth_exceeded    = 13,
        parse_node_limit        = 14,

        // Recovery
        recover_deleted         = 20,
        recover_inserted        = 21,
        recover_skipped         = 22,
        recover_wrapped         = 23,
        recover_repair_limit    = 24,
        recover_replace         = 25,  // replace unexpected token
        recover_wrap_subtree    = 26,  // wrap malformed subtree in error node

        // Grammar fingerprint
        grammar_fingerprint_mismatch = 30,
    };

    [[nodiscard]] constexpr std::string_view to_code(samasa_diag_code c) noexcept {
        switch (c) {
        case samasa_diag_code::lex_unknown_char:          return "SAMASA-LEX-UNKNOWN-CHAR";
        case samasa_diag_code::lex_unterminated_string:   return "SAMASA-LEX-UNTERMINATED-STRING";
        case samasa_diag_code::lex_bad_number:            return "SAMASA-LEX-BAD-NUMBER";
        case samasa_diag_code::lex_unterminated_comment:  return "SAMASA-LEX-UNTERMINATED-COMMENT";
        case samasa_diag_code::parse_unexpected_token:    return "SAMASA-PARSE-UNEXPECTED-TOKEN";
        case samasa_diag_code::parse_expected:            return "SAMASA-PARSE-EXPECTED";
        case samasa_diag_code::parse_missing:             return "SAMASA-PARSE-MISSING";
        case samasa_diag_code::parse_depth_exceeded:      return "SAMASA-PARSE-DEPTH-EXCEEDED";
        case samasa_diag_code::parse_node_limit:          return "SAMASA-PARSE-NODE-LIMIT";
        case samasa_diag_code::recover_deleted:           return "SAMASA-RECOVER-DELETED";
        case samasa_diag_code::recover_inserted:          return "SAMASA-RECOVER-INSERTED";
        case samasa_diag_code::recover_skipped:           return "SAMASA-RECOVER-SKIPPED";
        case samasa_diag_code::recover_wrapped:           return "SAMASA-RECOVER-WRAPPED";
        case samasa_diag_code::recover_repair_limit:      return "SAMASA-RECOVER-REPAIR-LIMIT";
        case samasa_diag_code::recover_replace:           return "SAMASA-RECOVER-REPLACE";
        case samasa_diag_code::recover_wrap_subtree:      return "SAMASA-RECOVER-WRAP-SUBTREE";
        case samasa_diag_code::grammar_fingerprint_mismatch:
            return "SAMASA-GRAMMAR-FINGERPRINT-MISMATCH";
        }
        return "SAMASA-UNKNOWN";
    }

    using diagnostic = lang::lang_diagnostic<samasa_diag_code>;

    // ---- expected_item -----------------------------------------------------
    // One entry describing what the parser expected at a parse failure point.

    struct expected_item {
        enum class kind : std::uint8_t {
            token_kind,    // a specific token kind (e.g. TK::plus)
            token_text,    // a specific text spelling (e.g. "=>")
            rule,          // entry into a named rule
            end_of_file,
        };

        kind             type     = kind::token_kind;
        std::string_view spelling; // token text / rule name; empty for token_kind/eof
        std::uint32_t    token_id = 0; // underlying token kind integer (for token_kind)
    };

    // ---- repair_kind -------------------------------------------------------
    // Scoring weights per design.md v2.

    enum class repair_kind : std::uint8_t {
        none            = 0,
        insert_missing  = 1,  // cost 1
        delete_token    = 2,  // cost 1
        replace_token   = 3,  // cost 2
        skip_tokens     = 4,  // cost N (tokens skipped)
        wrap_subtree    = 5,  // cost 4
        abort_rule      = 6,  // high cost
    };

    [[nodiscard]] constexpr std::uint8_t repair_cost(repair_kind k) noexcept {
        switch (k) {
        case repair_kind::insert_missing: return 1;
        case repair_kind::delete_token:   return 1;
        case repair_kind::replace_token:  return 2;
        case repair_kind::wrap_subtree:   return 4;
        case repair_kind::abort_rule:     return 255;
        default:                          return 0;
        }
    }

    // ---- repair_info -------------------------------------------------------

    struct repair_info {
        repair_kind      kind       = repair_kind::none;
        std::uint8_t     cost       = 0;
        std::string_view token_text; // inserted/replaced token text if applicable
    };

    // ---- parse_diagnostic --------------------------------------------------
    // Rich structured diagnostic produced at a parse failure. Contains the
    // expected set, actual token info, and optional repair that was applied.

    template <std::size_t MaxExpected = 16>
    struct parse_diagnostic {
        samasa_diag_code                    code     = samasa_diag_code::parse_unexpected_token;
        ::lang::severity                    level    = ::lang::severity::error;
        byte_span                           span;

        // Expected vs actual.
        std::array<expected_item, MaxExpected> expected{};
        std::uint8_t                           expected_count = 0;
        std::uint32_t                          actual_token_pos = 0;

        // Furthest error position (Earley-style "where did we get furthest").
        std::uint32_t furthest_offset = 0;

        // Repair applied (if any).
        repair_info repair;

        // Human-readable message.
        std::string_view message;

        void add_expected(expected_item item) noexcept {
            if (expected_count < MaxExpected)
                expected[expected_count++] = item;
        }
    };

} // namespace lang::samasa
