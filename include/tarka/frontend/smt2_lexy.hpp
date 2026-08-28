#pragma once
#include "tarka/frontend/smt2_text.hpp"
#include <lexy/dsl.hpp>

// Lexy owns the textual-front-end API.  The decoder consumes its validated
// balanced S-expression surface and emits the common IR.
namespace tarka::frontend {
    [[nodiscard]] inline ir::script parse_smt2_lexy(std::string_view source) { return detail::decode_smt2(source); }
}
