#pragma once

// languages/generic/ir/node.hpp — Generic IR node record.
//
// C++23, header-only, no virtual, no macros. Namespace: lang
//
// ir_node<KindEnum, ExtPayload=monostate> — flat POD-like record with an extension seam.
//   ExtPayload = monostate  → zero extra bytes (CST / untyped AST frontends).
//   ExtPayload = type_ref   → typed AST (e.g. vakya).
//   Users pay only for the ExtPayload they declare.
//
// symbol_id — opaque uint32_t borrowed from InternPool (default 0 = none).

#include <cstdint>
#include <limits>
#include <variant>
#include "../tree/spans.hpp"

namespace lang {

    using ir_node_id = std::uint32_t;
    inline constexpr ir_node_id k_null_ir = std::numeric_limits<std::uint32_t>::max();

    using symbol_id = std::uint32_t;
    inline constexpr symbol_id k_null_symbol = 0;

    template <class KindEnum, class ExtPayload = std::monostate>
    struct ir_node {
        KindEnum      kind            = {};
        byte_span     span            = {};
        ir_node_id    first_child     = k_null_ir;
        std::uint32_t child_count     = 0;
        std::uint64_t structural_hash = 0;
        symbol_id     name            = k_null_symbol;
        ExtPayload    ext             = {};
    };

} // namespace lang
