#pragma once

// samasa/lex/line_policy.hpp — Line-sensitivity policies for the scanner.
//
// C++23, header-only, no virtual, no macros. Namespace: lang::samasa
//
// line_policy concept — required by scanner:
//   bool line_continues(TokenKind prev) noexcept
//   bool suppress_separator(TokenKind prev, TokenKind next) noexcept
//   TokenKind synthetic_separator() noexcept
//
// Presets:
//   no_line_sensitivity<TK>   — newlines are trivia; no synthetic separators.
//   newline_terminates<TK,Sep> — newlines after certain tokens become Sep.
//   custom_separator<TK,Sep,Pred> — user-supplied predicate.

#include <concepts>

namespace lang::samasa {

    // ---- Concept -------------------------------------------------------

    template <class P, class TokenKind>
    concept line_policy = requires(P p, TokenKind k) {
        { p.line_continues(k) }           -> std::same_as<bool>;
        { p.suppress_separator(k, k) }    -> std::same_as<bool>;
        { p.synthetic_separator() }       -> std::same_as<TokenKind>;
    };

    // ---- no_line_sensitivity -------------------------------------------

    template <class TokenKind>
    struct no_line_sensitivity {
        [[nodiscard]] constexpr bool      line_continues([[maybe_unused]] TokenKind) const noexcept { return true; }
        [[nodiscard]] constexpr bool      suppress_separator([[maybe_unused]] TokenKind, [[maybe_unused]] TokenKind) const noexcept { return true; }
        [[nodiscard]] constexpr TokenKind synthetic_separator() const noexcept { return {}; }
    };

    // ---- newline_terminates<TK, SepKind> --------------------------------
    // Newline after tokens that can end a statement inserts SepKind.
    // Users specialise statement_ending<TK> to define which tokens end statements.

    template <class TokenKind>
    struct statement_ending {
        // Default: no token ends a statement (override per language).
        [[nodiscard]] static constexpr bool value([[maybe_unused]] TokenKind) noexcept { return false; }
    };

    template <class TokenKind, TokenKind SepKind>
    struct newline_terminates {
        [[nodiscard]] constexpr bool line_continues(TokenKind prev) const noexcept {
            return !statement_ending<TokenKind>::value(prev);
        }
        [[nodiscard]] constexpr bool suppress_separator([[maybe_unused]] TokenKind, [[maybe_unused]] TokenKind) const noexcept {
            return false;
        }
        [[nodiscard]] constexpr TokenKind synthetic_separator() const noexcept { return SepKind; }
    };

} // namespace lang::samasa
