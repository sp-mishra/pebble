#pragma once

// samasa/tooling/highlight.hpp — Compile-time syntax highlight table.
//
// C++23, header-only, no virtual, no macros. Namespace: lang::samasa
//
// syntax_highlight_table<G>() — consteval: returns a fixed mapping of
//   TokenKind values to highlight_class values for editor integration.

#include <cstdint>

namespace lang::samasa {

    enum class highlight_class : std::uint8_t {
        none       = 0,
        keyword    = 1,
        identifier = 2,
        literal    = 3,
        operator_  = 4,
        comment    = 5,
        punctuation= 6,
        type_name  = 7,
        builtin    = 8,
    };

    // Placeholder: language-specific specialisations return a consteval
    // mapping from TokenKind → highlight_class.
    template <class G>
    [[nodiscard]] consteval highlight_class highlight_for(typename G::token_kind) {
        return highlight_class::none;
    }

} // namespace lang::samasa
