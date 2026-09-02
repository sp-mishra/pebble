#pragma once

// generic/core/source_location.hpp — Language-agnostic source position.
//
// C++23, header-only, no virtual, no macros. Namespace: lang
//
// source_span — POD, trivially copyable byte-offset + line/col carrier.
// decode_span — compute line/col by scanning source bytes (single-pass, no alloc).
//
// Usage:
//   lang::source_span s = lang::decode_span(src_text, offset, length);
//   // s.line, s.col are 1-based; s.offset/length are byte positions.

#include <cstdint>
#include <string_view>

namespace lang {
    // =========================================================================
    // source_span — trivially copyable source position
    // =========================================================================

    struct source_span {
        std::uint32_t offset = 0; // byte offset from source start
        std::uint32_t length = 0; // byte length of the token/node
        std::uint32_t line = 1; // 1-based line number
        std::uint32_t col = 1; // 1-based column (byte offset from line start)
    };

    static_assert(std::is_trivially_copyable_v<source_span>);

    // =========================================================================
    // decode_span — resolve line/col from a raw byte offset
    //
    // Scans [src[0], src[offset]) counting '\n' to fill line and col.
    // Suitable for single-pass builds where spans are decoded once per node.
    // =========================================================================

    [[nodiscard]] inline source_span
    decode_span(std::string_view src,
                std::uint32_t offset,
                std::uint32_t length) noexcept {
        source_span s{offset, length, 1, 1};
        std::uint32_t col = 1;
        const auto end = offset < static_cast<std::uint32_t>(src.size())
                             ? offset
                             : static_cast<std::uint32_t>(src.size());
        for (std::uint32_t i = 0; i < end; ++i) {
            if (src[i] == '\n') {
                ++s.line;
                col = 1;
            }
            else { ++col; }
        }
        s.col = col;
        return s;
    }
} // namespace lang
