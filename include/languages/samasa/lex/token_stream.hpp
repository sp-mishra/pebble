#pragma once

// samasa/lex/token_stream.hpp — Non-owning span view over a token array.
//
// C++23, header-only, no virtual, no macros. Namespace: lang::samasa
//
// token_stream<TK> — span-backed view; satisfies the Stream concept required by cursor<>.

#include <cstdint>
#include <span>
#include <vector>
#include "token.hpp"

namespace lang::samasa {

    template <class TokenKind>
    struct token_stream {
        std::span<const token<TokenKind>> tokens;

        [[nodiscard]] constexpr std::uint32_t size() const noexcept {
            return static_cast<std::uint32_t>(tokens.size());
        }
        [[nodiscard]] constexpr bool empty() const noexcept { return tokens.empty(); }

        [[nodiscard]] constexpr const token<TokenKind>& operator[](std::uint32_t i) const {
            return tokens[i];
        }

        [[nodiscard]] constexpr const token<TokenKind>* begin() const noexcept {
            return tokens.data();
        }
        [[nodiscard]] constexpr const token<TokenKind>* end() const noexcept {
            return tokens.data() + tokens.size();
        }
    };

    // Owning vector — scanner produces this; then a token_stream view is taken.
    template <class TokenKind>
    struct token_buffer {
        std::vector<token<TokenKind>> data;
        std::vector<trivia>           trivia_arena;

        [[nodiscard]] token_stream<TokenKind> view() const noexcept {
            return {std::span<const token<TokenKind>>(data)};
        }
    };

} // namespace lang::samasa
