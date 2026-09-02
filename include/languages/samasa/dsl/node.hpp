#pragma once

// samasa/dsl/node.hpp — Syntax node emitter combinator.
//
// C++23, header-only, no virtual, no macros. Namespace: lang::samasa
//
// node<Kind, Pattern> — opens an event marker, runs Pattern, closes or rolls back.
//   - success  → events.end(marker, span)
//   - soft_fail → events.rollback(marker)          — O(1) truncate
//   - hard_fail → events emit error node; keep partial as tombstone
//
// Emits events, not allocations; green_tree is built later from the event log.

#include "../core/result.hpp"
#include "../core/diagnostic.hpp"
#include "../core/source_view.hpp"

namespace lang::samasa {
    template <auto Kind, class Pattern>
    struct node_t {
        Pattern pattern;

        template <class Ctx>
        [[nodiscard]] constexpr auto match(Ctx& ctx) const {
            using Stream = typename Ctx::stream_type;
            using R = parse_result<Stream>;

            auto mk = ctx.events().begin(static_cast<typename Ctx::syntax_kind>(Kind));
            if (ctx.over_depth())
                return R::hard_failure(ctx.cursor());

            ctx.push_depth();
            const auto start_offset = ctx.cursor().at_end()
                                          ? 0u
                                          : ctx.cursor().peek().offset;
            auto r = pattern.match(ctx);
            ctx.pop_depth();

            if (r.ok()) {
                const auto end_offset = ctx.cursor().at_end()
                                            ? (ctx.stream().empty()
                                                   ? 0u
                                                   : ctx.stream()[ctx.stream().size() - 1].offset + ctx.stream()[ctx.
                                                       stream().size() - 1].length)
                                            : ctx.cursor().peek().offset;
                byte_span span{start_offset, end_offset >= start_offset ? end_offset - start_offset : 0u};
                ctx.events().end(mk, span);
                ctx.inc_nodes();
                return r;
            }
            if (r.soft_fail()) {
                ctx.events().rollback(mk);
                return r;
            }
            // hard_fail — emit error node covering scanned range
            const std::uint32_t cur_offset = ctx.cursor().at_end()
                                                 ? start_offset
                                                 : ctx.cursor().peek().offset;
            byte_span err_span{start_offset, cur_offset >= start_offset ? cur_offset - start_offset : 0u};
            ctx.events().error(samasa_diag_code::parse_unexpected_token, err_span);
            ctx.events().rollback(mk); // tombstone the begin
            return r;
        }
    };

    template <auto Kind, class Pattern>
    [[nodiscard]] constexpr node_t<Kind, Pattern> node(Pattern p) {
        return {std::move(p)};
    }
} // namespace lang::samasa
