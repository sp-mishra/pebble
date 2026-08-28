#pragma once

// samasa/grammar/grammar_ir.hpp — Compile-time grammar analysis: nullable, FIRST, rule graph.
//
// C++23, header-only, no virtual, no macros. Namespace: lang::samasa
//
// grammar_ir<G> — computed via meta::TypeList introspection over G::rules.
//   nullable<Rule>    — true if Rule can match the empty string.
//   first_set<Rule>   — set of TokenKind values that can start Rule (as ct_char_set analogue).
//   rule_count        — number of rules in the grammar.
//
// All static constexpr — pure compile-time evaluation.
//
// Note: full FIRST/FOLLOW for general PEG grammars requires structural recursion over
// the matcher type tree. This header provides the nullable + type-level machinery;
// concrete FIRST values are derived by validation.hpp sub-checks.

#include "grammar.hpp"
#include "meta/meta.hpp"
#include "../dsl/primitive.hpp"
#include "../dsl/combinators.hpp"
#include "../dsl/rule.hpp"
#include "../dsl/node.hpp"
#include "../recovery/recovery.hpp"

namespace lang::samasa {

    // -------------------------------------------------------------------------
    // Nullable predicate — a rule R is nullable iff its pattern can match ε.
    // Structural evaluation over the pattern type hierarchy.
    // -------------------------------------------------------------------------

    namespace detail {

        // Forward declaration for mutual recursion.
        template <class Pattern> struct is_nullable;

        // Terminal matchers are NOT nullable.
        template <auto Kind>
        struct is_nullable<tok<Kind>> : std::false_type {};

        template <akshara::fixed_string S>
        struct is_nullable<char_lit<S>> : std::bool_constant<S.length == 0> {};

        template <akshara::fixed_string S>
        struct is_nullable<token_text<S>> : std::false_type {};

        template <akshara::ct_char_set Set>
        struct is_nullable<char_in<Set>> : std::false_type {};

        template <akshara::fixed_string W>
        struct is_nullable<contextual_keyword<W>> : std::false_type {};

        // seq is nullable iff ALL components are nullable.
        template <class... Ms>
        struct is_nullable<seq_t<Ms...>> : std::bool_constant<(is_nullable<Ms>::value && ...)> {};

        // choice is nullable iff ANY component is nullable.
        template <class... Ms>
        struct is_nullable<choice_t<Ms...>> : std::bool_constant<(is_nullable<Ms>::value || ...)> {};

        // opt is always nullable.
        template <class M>
        struct is_nullable<opt_t<M>> : std::true_type {};

        // many is nullable (zero iterations).
        template <class M>
        struct is_nullable<many_t<M>> : std::true_type {};

        // many1 is nullable iff its body is nullable (shouldn't happen; caught by validation).
        template <class M>
        struct is_nullable<many1_t<M>> : is_nullable<M> {};

        // sep_by is nullable (zero iterations).
        template <class A, class Sep>
        struct is_nullable<sep_by_t<A,Sep>> : std::true_type {};

        template <class A, class Sep>
        struct is_nullable<sep_by1_t<A,Sep>> : is_nullable<A> {};

        // lookahead / not_followed_by are nullable (no consumption).
        template <class M>
        struct is_nullable<lookahead_t<M>> : std::true_type {};

        template <class M>
        struct is_nullable<not_followed_by_t<M>> : std::true_type {};

        // cut is nullable (no consumption).
        template <>
        struct is_nullable<cut> : std::true_type {};

        // eof is NOT nullable — matches only at end of stream, not empty.
        template <>
        struct is_nullable<lang::samasa::eof> : std::false_type {};

        // rule delegates to its pattern.
        template <akshara::fixed_string Name, class Pattern>
        struct is_nullable<rule<Name,Pattern>> : is_nullable<Pattern> {};

        // node delegates to its pattern.
        template <auto Kind, class Pattern>
        struct is_nullable<node_t<Kind,Pattern>> : is_nullable<Pattern> {};

        // recover_with is nullable iff Pattern is nullable.
        template <class Pattern, class Recovery>
        struct is_nullable<recover_with<Pattern,Recovery>> : is_nullable<Pattern> {};

    } // namespace detail

    template <class Pattern>
    inline constexpr bool nullable_v = detail::is_nullable<Pattern>::value;

    // -------------------------------------------------------------------------
    // grammar_ir<G>
    // -------------------------------------------------------------------------

    template <class G>
    struct grammar_ir {
        using syntax_kind = typename G::syntax_kind;
        using token_kind  = typename G::token_kind;
        using rules       = typename G::rules;

        static constexpr std::size_t rule_count = G::rule_count;

        template <class Rule>
        static constexpr bool nullable = nullable_v<typename Rule::pattern_type>;
    };

} // namespace lang::samasa
