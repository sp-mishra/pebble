#pragma once

// samasa/core/source_view.hpp — Source identity and byte-range types.
//
// C++23, header-only, no virtual, no macros. Namespace: lang::samasa
//
// file_id     — opaque u32; 0 = unknown.
// byte_span   — alias of lang::byte_span (owner: languages/generic/tree/spans.hpp).
// source_view — non-owning {file_id, text} pair.

#include <cstdint>
#include <string_view>
#include "languages/generic/tree/spans.hpp"

namespace lang::samasa {

    using file_id  = std::uint32_t;
    using byte_span = lang::byte_span;     // owner: languages/generic/tree/spans.hpp

    struct source_view {
        file_id          file = 0;
        std::string_view text;

        [[nodiscard]] constexpr bool valid() const noexcept { return !text.empty(); }
    };

} // namespace lang::samasa
