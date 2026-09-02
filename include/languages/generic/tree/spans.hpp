#pragma once

// languages/generic/tree/spans.hpp — Generic span primitives for language infrastructure.
//
// C++23, header-only, no virtual, no macros. Namespace: lang
//
// byte_span   — [offset, length) in source bytes. hull(a,b) computes minimal covering span.
// token_range — [start, end) index range into a token array.
// text_edit   — source mutation: [offset, offset+removed_length) replaced by inserted_text.
//
// samasa aliases these in Stage 2; other frontends define their own DiagCode and use directly.

#include <cstdint>
#include <string>

namespace lang {
    // ---- byte_span ---------------------------------------------------------

    struct byte_span {
        std::uint32_t offset = 0;
        std::uint32_t length = 0;

        [[nodiscard]] constexpr bool empty() const noexcept { return length == 0; }
        [[nodiscard]] constexpr std::uint32_t end() const noexcept { return offset + length; }

        [[nodiscard]] constexpr bool operator==(const byte_span&) const noexcept = default;

        // Minimal span covering both a and b (empty operands are identity).
        [[nodiscard]] static constexpr byte_span hull(byte_span a, byte_span b) noexcept {
            if (a.empty()) return b;
            if (b.empty()) return a;
            const std::uint32_t lo = a.offset < b.offset ? a.offset : b.offset;
            const std::uint32_t hi = a.end() > b.end() ? a.end() : b.end();
            return {lo, hi - lo};
        }
    };

    // ---- token_range -------------------------------------------------------
    // Half-open index range [start, end) into any token array.

    struct token_range {
        std::uint32_t start = 0;
        std::uint32_t end = 0;

        [[nodiscard]] constexpr std::uint32_t size() const noexcept { return end - start; }
        [[nodiscard]] constexpr bool empty() const noexcept { return start == end; }

        [[nodiscard]] constexpr bool operator==(const token_range&) const noexcept = default;
    };

    // ---- text_edit ---------------------------------------------------------
    // Describes a source mutation: replace [offset, offset+removed_length) with inserted_text.

    struct text_edit {
        std::uint32_t offset = 0;
        std::uint32_t removed_length = 0;
        std::string inserted_text;
    };
} // namespace lang
