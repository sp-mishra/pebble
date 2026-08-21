#pragma once

// samasa/dsl/matcher.hpp — Matcher concept.
//
// C++23, header-only, no virtual, no macros. Namespace: lang::samasa
//
// matcher<M,Ctx> — concept satisfied by any type M that exposes
//   parse_result<typename Ctx::stream_type> match(Ctx&) const;
//
// All primitives, combinators, rules, and nodes satisfy this concept.

#include <concepts>
#include "../core/result.hpp"

namespace lang::samasa {

    // MatcherContext concept — required by parse_context but not directly imported
    // here to keep this header lean. Users can substitute any type providing:
    //   - events() -> event_stream<SK>&
    //   - emit(diagnostic)
    //   - depth() / push_depth() / pop_depth() / over_depth()
    //   - update_furthest(u32)
    template <class Ctx>
    concept MatcherContext = requires(Ctx& c) {
        typename Ctx::syntax_kind;
        typename Ctx::token_kind;
        c.events();
        c.push_depth();
        c.pop_depth();
    };

    // matcher<M,Ctx> — core concept every combinator satisfies.
    template <class M, class Ctx>
    concept matcher = MatcherContext<Ctx> && requires(const M& m, Ctx& ctx) {
        { m.match(ctx) };
    };

} // namespace lang::samasa
