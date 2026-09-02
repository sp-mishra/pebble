#pragma once

// samasa/lex/token.hpp — Token and trivia types.
//
// C++23, header-only, no virtual, no macros. Namespace: lang::samasa
//
// trivia_kind — whitespace / comment / synthetic categories.
// trivia      — out-of-band trivia record (kind + span).
// token<TK>   — compact, trivially copyable token. Size is not ABI-stable
//               unless SAMASA_STABLE_TOKEN_ABI is defined.
//               Fields: kind, offset, length (uint32), trivia_start, trivia_count, flags.

#include <cstdint>
#include <type_traits>
#include "../core/source_view.hpp"

namespace lang::samasa {
    enum class trivia_kind : std::uint8_t {
        whitespace = 0,
        newline = 1,
        line_comment = 2,
        block_comment = 3,
        synthetic_separator = 4, // inserted by line_policy
        skipped = 5, // recovery-skipped chars
    };

    struct trivia {
        trivia_kind kind = trivia_kind::whitespace;
        byte_span span;
    };

    // token<TK> — compact, trivially copyable.
    // trivia_start: first index in the token_buffer::trivia_arena for this token.
    // trivia_count: how many consecutive trivia entries belong to this token.
    // flags: reserved for future language-specific bits (doc-comment, synthetic, etc.).
    template <class TokenKind>
    struct token {
        TokenKind kind = {};
        std::uint32_t offset = 0;
        std::uint32_t length = 0; // uint32 — supports large literals/blobs
        std::uint32_t trivia_start = 0; // index into trivia arena; 0 = no trivia
        std::uint16_t trivia_count = 0;
        std::uint16_t flags = 0;

        [[nodiscard]] constexpr byte_span span() const noexcept { return {offset, length}; }
        [[nodiscard]] constexpr bool empty() const noexcept { return length == 0; }
    };

    static_assert(std::is_trivially_copyable_v<token<std::uint32_t>>);
} // namespace lang::samasa
