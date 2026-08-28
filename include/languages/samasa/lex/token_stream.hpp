#pragma once

// samasa/lex/token_stream.hpp — Non-owning span view over a token array.
//
// C++23, header-only, no virtual, no macros. Namespace: lang::samasa
//
// token_stream<TK> — span-backed view; satisfies the Stream concept required by cursor<>.

#include <cstdint>
#include <span>
#include "containers/dynamic/SmallVector.hpp"
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

    // Owning scanner buffer. Typical source files keep both token and trivia
    // storage inline; callers that need a different allocator can use the
    // storage template directly.
    template <class TokenKind>
    struct token_buffer {
        using token_storage  = containers::dynamic::SmallVector<token<TokenKind>, 512>;
        using trivia_storage = containers::dynamic::SmallVector<trivia, 256>;
        token_storage  data;
        trivia_storage trivia_arena;

        [[nodiscard]] token_stream<TokenKind> view() const noexcept {
            return {std::span<const token<TokenKind>>(data)};
        }
    };

} // namespace lang::samasa
