#pragma once

// samasa/lex/operator_trie.hpp — Compile-time longest-match operator scanner.
//
// C++23, header-only, no virtual, no macros. Namespace: lang::samasa
//
// operator_token<Spelling,Kind> — maps a compile-time string to a TokenKind.
// operator_trie<OTs...>         — tries all operator spellings from the current
//   position; returns the longest match.
//
// Algorithm: for each operator, try to match its full spelling from pos.
// Choose the operator with the longest matching spelling; ties broken by
// declaration order (first wins). No actual trie structure needed for typical
// operator counts (< 64); linear scan over spellings ordered by descending length
// ensures longest-match correctness.

#include <optional>
#include <string_view>
#include "meta/akshara.hpp"

namespace lang::samasa {

    template <akshara::fixed_string Spelling, auto Kind>
    struct operator_token {
        static constexpr auto spelling = Spelling;
        static constexpr auto kind     = Kind;
        static constexpr std::size_t length = Spelling.length;
    };

    template <class... OTs>
    struct operator_trie {

        // Match the longest operator starting at src[pos].
        // Returns the matched TokenKind and consumed length, or nullopt.
        template <class TokenKind>
        [[nodiscard]] static constexpr auto match(std::string_view src, std::size_t pos)
            -> std::optional<std::pair<TokenKind, std::size_t>>
        {
            using result_t = std::pair<TokenKind, std::size_t>;
            std::optional<result_t> best;

            ([&]<class OT>() {
                constexpr std::size_t len = OT::length;
                if (pos + len > src.size()) return;
                if (src.substr(pos, len) != static_cast<std::string_view>(OT::spelling)) return;
                // Keep longest match.
                if (!best || len > best->second)
                    best = result_t{static_cast<TokenKind>(OT::kind), len};
            }.template operator()<OTs>(), ...);

            return best;
        }

        static constexpr std::size_t size = sizeof...(OTs);
    };

} // namespace lang::samasa
