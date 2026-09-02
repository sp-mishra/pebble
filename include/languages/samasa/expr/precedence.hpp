#pragma once

// samasa/expr/precedence.hpp — Associativity and fixity types for Pratt parsing.
//
// C++23, header-only, no virtual, no macros. Namespace: lang::samasa

#include <cstdint>

namespace lang::samasa {
    enum class associativity : std::uint8_t {
        left = 0,
        right = 1,
        none = 2, // non-associative: a OP b OP c is a parse error
    };

    enum class fixity : std::uint8_t {
        prefix = 0,
        infix = 1,
        postfix = 2,
    };
} // namespace lang::samasa
