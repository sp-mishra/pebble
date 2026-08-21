#pragma once

// samasa/dsl/rule.hpp — Named grammar rule wrapper.
//
// C++23, header-only, no virtual, no macros. Namespace: lang::samasa
//
// rule<Name, Pattern> — wraps a Pattern with a compile-time name for diagnostics,
//   expected-set computation, and grammar-IR analysis. Delegates match() to Pattern.
//   When Ctx exposes trace(), emits enter_rule / exit_rule / fail events automatically.
//
// Usage:
//   using decl_rule = rule<"decl", seq_t<tok<Kw::let>, ...>>;

#include <string_view>
#include "meta/akshara.hpp"
#include "../core/result.hpp"

namespace lang::samasa {

    template <akshara::fixed_string Name, class Pattern>
    struct rule {
        static constexpr auto name      = Name;
        static constexpr auto name_sv   = static_cast<std::string_view>(Name);
        using pattern_type              = Pattern;  // used by grammar IR / validation

        Pattern pattern;

        template <class Ctx>
        [[nodiscard]] constexpr auto match(Ctx& ctx) const {
            using Stream = typename Ctx::stream_type;
            if (ctx.over_depth()) {
                return parse_result<Stream>::hard_failure(ctx.cursor());
            }
            ctx.push_depth();

            const std::uint32_t entry_pos = ctx.cursor().pos;
            if constexpr (requires { ctx.trace(); }) {
                ctx.trace().enter(name_sv, entry_pos);
            }

            auto r = pattern.match(ctx);

            if constexpr (requires { ctx.trace(); }) {
                const std::uint32_t exit_pos = r.ok() ? r.next.pos : ctx.cursor().pos;
                if (r.ok())
                    ctx.trace().exit(name_sv, exit_pos);
                else
                    ctx.trace().fail(name_sv, exit_pos, r.hard_fail());
            }

            ctx.pop_depth();
            return r;
        }
    };

    template <akshara::fixed_string Name, class Pattern>
    [[nodiscard]] constexpr rule<Name, Pattern> make_rule(Pattern p) {
        return {std::move(p)};
    }

} // namespace lang::samasa
