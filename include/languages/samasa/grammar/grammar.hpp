#pragma once

// samasa/grammar/grammar.hpp — Grammar type bundle.
//
// C++23, header-only, no virtual, no macros. Namespace: lang::samasa
//
// grammar<SK,TK,Root,Rules...> — bundles enums + root + rule list.
//   Drives all compile-time introspection: validation, fingerprint, IR analysis.
//
// SK        — SyntaxKind enum (CST node kinds); must be an enum type.
// TK        — TokenKind enum (lexer token kinds); must be an enum type.
// Root      — root rule type (one of Rules...)
// Rules...  — all rule<Name,Pattern> types in the grammar
//
// EOF token kind is supplied through scan_token_kinds<TK>{ .eof = TK::eof, ... }.
// Samasa does not require any specific enumerator value for EOF.

#include <type_traits>
#include "meta/meta.hpp"

namespace lang::samasa {

    template <class SyntaxKindT, class TokenKindT,
              class RootRule, class... Rules>
    struct grammar {
        static_assert(std::is_enum_v<SyntaxKindT>,
            "grammar<>: SyntaxKind must be an enum type.");
        static_assert(std::is_enum_v<TokenKindT>,
            "grammar<>: TokenKind must be an enum type.");

        using syntax_kind = SyntaxKindT;
        using token_kind  = TokenKindT;
        using root_rule   = RootRule;
        using rules       = meta::TypeList<Rules...>;

        static constexpr std::size_t rule_count = sizeof...(Rules);
    };

} // namespace lang::samasa
