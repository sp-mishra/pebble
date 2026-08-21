#pragma once

// generic/lexer/digit_sep.hpp — Numeric literal digit-separator utilities.
//
// C++23, header-only, no virtual, no macros. Namespace: lang
//
// strip_digit_sep(sv, sep) — remove separator characters from a numeric
//   literal string view. Default separator is '_' (Go/Rust/Crank style).
//   Returns a new std::string with separators removed.
//
// Usage:
//   auto n = lang::strip_digit_sep("1_000_000"); // → "1000000"
//   auto h = lang::strip_digit_sep("0xff_ee");   // → "0xffee"

#include <string>
#include <string_view>

namespace lang {

    [[nodiscard]] inline std::string
    strip_digit_sep(std::string_view sv, char sep = '_') {
        std::string out;
        out.reserve(sv.size());
        for (char c : sv)
            if (c != sep) out.push_back(c);
        return out;
    }

} // namespace lang
