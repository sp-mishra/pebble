#pragma once

// generic/core/parse_stats.hpp — Language-agnostic parse-phase statistics.
//
// C++23, header-only, no virtual, no macros. Namespace: lang
//
// Provides POD stat structs for timing and tree metrics that any language
// frontend (lexer + parser) can populate. Numeric literal metrics and
// per-token-kind breakdowns are language-specific and not included here.
//
// phase_timings  — wall-clock nanoseconds per compiler phase.
// parse_tree_stats — node/depth metrics from a parse tree walk.
//
// Usage:
//   lang::phase_timings t;
//   auto t0 = std::chrono::steady_clock::now();
//   // ... parse ...
//   t.lex_and_parse = std::chrono::steady_clock::now() - t0;

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace lang {
    // =========================================================================
    // phase_timings — wall-clock duration per frontend phase
    // =========================================================================

    struct phase_timings {
        std::chrono::nanoseconds lex_and_parse{0}; // lexing + parsing wall time
        std::chrono::nanoseconds ast_build{0}; // AST construction wall time
        std::chrono::nanoseconds total{0}; // sum of all phases
    };

    // =========================================================================
    // parse_tree_stats — metrics collected during a parse tree walk
    // =========================================================================

    struct parse_tree_stats {
        // Source metrics
        std::uint32_t source_bytes{0};
        std::uint32_t source_lines{0};

        // Token metrics (language-agnostic)
        std::uint32_t total_tokens{0};
        std::uint32_t trivia_tokens{0};
        std::uint32_t identifier_count{0};
        std::uint32_t literal_count{0};
        std::uint32_t comment_bytes{0};

        // Parse tree metrics
        std::uint32_t production_nodes{0};
        std::uint32_t max_depth{0};
        std::unordered_map<std::string, std::uint32_t> production_by_name;

        // Error metrics
        std::uint32_t error_count{0};
        std::uint32_t warning_count{0};
        std::uint32_t note_count{0};

        // Timings
        phase_timings timings;
    };
} // namespace lang
