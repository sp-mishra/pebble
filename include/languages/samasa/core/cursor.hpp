#pragma once

// samasa/core/cursor.hpp — Value-type cursor over an indexed stream.
//
// C++23, header-only, no virtual, no macros. Namespace: lang::samasa
//
// cursor<Stream> — copyable position into any stream exposing:
//   .size()         → std::uint32_t
//   .peek(u32 pos)  → element (const ref or value)
//
// Cheap backtracking: save cursor by copy, restore by assignment.

#include <cstdint>

namespace lang::samasa {

    template <class Stream>
    struct cursor {
        const Stream* stream = nullptr;
        std::uint32_t pos    = 0;

        [[nodiscard]] constexpr bool at_end() const noexcept {
            return !stream || pos >= stream->size();
        }

        // Peek ahead la positions from current (0 = current).
        [[nodiscard]] constexpr decltype(auto) peek(std::uint32_t la = 0) const {
            return (*stream)[pos + la];
        }

        // True when at_end() + la tokens of lookahead exist.
        [[nodiscard]] constexpr bool can_peek(std::uint32_t la) const noexcept {
            return stream && (pos + la) < stream->size();
        }

        [[nodiscard]] constexpr cursor advance(std::uint32_t n = 1) const noexcept {
            return {stream, pos + n};
        }

        [[nodiscard]] constexpr bool operator==(const cursor& o) const noexcept {
            return pos == o.pos && stream == o.stream;
        }
    };

} // namespace lang::samasa
