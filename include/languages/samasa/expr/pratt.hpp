#pragma once

// samasa/expr/pratt.hpp — Generic Pratt expression parser.
//
// C++23, header-only, no virtual, no macros. Namespace: lang::samasa
//
// pratt_expression<Table, PrimaryRule, ActionPolicy>
//   — implements the Pratt algorithm using an operator_table<Ops...>.
//   — delegates prefix/primary parsing to PrimaryRule.
//
// Two built-in action policies:
//
//   flat_pratt_action (default)
//     Emits token events only; no begin/end node wrapping. The CST records tokens
//     in order but does NOT preserve operator precedence as nested syntax nodes.
//     Suitable for simple languages or frontends that re-parse expression structure.
//
//   structured_pratt_action<BinaryKind, PrefixKind, PostfixKind>
//     Wraps each binary/prefix/postfix operation in a begin/end node event so
//     precedence is visible as CST structure. Infix nodes fully wrap their left
//     operand (correct tree shape). Operator token is emitted INSIDE the node
//     (between left operand and right operand subtree). All nodes carry exact hull
//     spans covering their full operand range.
//
//       For "a + b * c":
//         begin binary_expr            ← spans all of "a + b * c"
//           token ident(a)
//           token plus
//           begin binary_expr          ← spans "b * c"
//             token ident(b)
//             token star
//             token ident(c)
//           end binary_expr
//         end binary_expr
//
//       For prefix "-a":
//         begin prefix_expr            ← spans "-a"
//           token minus
//           token ident(a)
//         end prefix_expr
//
//       For postfix "a?":
//         begin postfix_expr           ← spans "a?"
//           token ident(a)
//           token question
//         end postfix_expr
//
//     Language authors supply their SyntaxKind enum values for each fixity.
//
//   cst_pratt_action — alias for flat_pratt_action (backward compatibility).
//
// Action policies customize event emission. AST construction should happen in
// the language frontend after parsing, not inside Samasa.
//
// Infix left-operand wrapping:
//   structured_pratt_action uses event_log::insert_begin_at to retroactively open
//   a binary node before the left operand's already-emitted events. The Pratt loop
//   snapshots the event log before parsing the primary (pre_left), then passes that
//   snapshot to begin_infix so the action can insert the open-node at that position.
//
// Marker threading:
//   structured_pratt_action::begin_infix returns the event_stream marker so the
//   Pratt loop can close the node with the correct span after parsing the RHS.
//   flat_pratt_action::begin_infix returns void; the loop skips the close call.
//   The loop uses `if constexpr (requires { ... })` to handle both cases at zero cost.

#include "operator_table.hpp"
#include "../core/result.hpp"
#include "languages/generic/tree/spans.hpp"
#include <type_traits>

namespace lang::samasa {

    // ---- flat_pratt_action — token events only, no node wrapping -------------
    // Emits token events into the context's event_stream. No begin/end node
    // wrapping — expression structure is NOT preserved as CST nesting.
    // Use for simple languages or when the frontend re-parses expression structure.
    struct flat_pratt_action {
        template <class Ctx, class Op>
        static void begin_infix([[maybe_unused]] Ctx& ctx, [[maybe_unused]] Op op) {}
        template <class Ctx, class Op>
        static void end_infix([[maybe_unused]] Ctx& ctx, [[maybe_unused]] Op op) {}
        template <class Ctx, class Op>
        static auto begin_prefix([[maybe_unused]] Ctx& ctx, [[maybe_unused]] Op op) {}
        template <class Ctx, class Op>
        static void end_prefix([[maybe_unused]] Ctx& ctx, [[maybe_unused]] Op op) {}
        template <class Ctx, class Op>
        static auto begin_postfix([[maybe_unused]] Ctx& ctx, [[maybe_unused]] Op op) {}
        template <class Ctx, class Op>
        static void end_postfix([[maybe_unused]] Ctx& ctx, [[maybe_unused]] Op op) {}
    };

    // cst_pratt_action — alias for flat_pratt_action (backward compatibility).
    using cst_pratt_action = flat_pratt_action;

    // ---- structured_pratt_action<BinaryKind, PrefixKind, PostfixKind> --------
    // Wraps each binary, prefix, and postfix operation in begin/end node events
    // so operator precedence is preserved as CST structure with correct tree shape.
    //
    // Infix nodes fully wrap their left operand via retroactive insertion:
    //   begin_infix receives a pre_left snapshot and calls insert_begin_at so
    //   the open-node event is placed before the left operand's tokens.
    //
    // All nodes carry exact hull spans (end_* receive operand spans).
    //
    // Event order contract (fully locked):
    //
    //   Infix (e.g. a + b):
    //     begin_infix(ctx, op, pre_left) → inserts begin_node(BinaryKind) at pre_left,
    //                                      returns marker pointing to that event
    //     [operator token emitted by caller]
    //     [RHS parsed recursively]
    //     end_infix(ctx, op, marker, hull_span) → emits end_node(BinaryKind, hull_span)
    //     Result: begin_node wraps left operand + operator + right operand.
    //
    //   Prefix (e.g. -a):
    //     begin_prefix(ctx, op) → emits begin_node(PrefixKind), returns marker
    //     [operator token emitted by caller]
    //     [operand parsed recursively]
    //     end_prefix(ctx, op, marker, hull_span) → emits end_node
    //
    //   Postfix (e.g. a?):
    //     [operand tokens already emitted; pre_left snapshot held by loop]
    //     begin_postfix(ctx, op, pre_left) → inserts begin_node(PostfixKind) at pre_left,
    //                                        returns marker
    //     [operator token emitted by caller]
    //     end_postfix(ctx, op, marker, hull_span) → emits end_node

    template <auto BinaryKind, auto PrefixKind = BinaryKind, auto PostfixKind = BinaryKind>
    struct structured_pratt_action {
        // begin_infix — retroactively inserts the binary open-node before the left operand.
        // pre_left is the event-log snapshot taken before the primary was parsed.
        template <class Ctx, class Op, class Marker>
        static auto begin_infix(Ctx& ctx, [[maybe_unused]] Op op, Marker pre_left) {
            return ctx.events().insert_begin_at(
                pre_left, static_cast<typename Ctx::syntax_kind>(BinaryKind));
        }

        template <class Ctx, class Op, class Marker>
        static void end_infix(Ctx& ctx, [[maybe_unused]] Op op, Marker m, lang::byte_span span) {
            ctx.events().end(m, span);
        }

        template <class Ctx, class Op>
        static auto begin_prefix(Ctx& ctx, [[maybe_unused]] Op op) {
            return ctx.events().begin(static_cast<typename Ctx::syntax_kind>(PrefixKind));
        }

        template <class Ctx, class Op, class Marker>
        static void end_prefix(Ctx& ctx, [[maybe_unused]] Op op, Marker m, lang::byte_span span) {
            ctx.events().end(m, span);
        }

        // begin_postfix — retroactively inserts the postfix open-node before the operand.
        template <class Ctx, class Op, class Marker>
        static auto begin_postfix(Ctx& ctx, [[maybe_unused]] Op op, Marker pre_left) {
            return ctx.events().insert_begin_at(
                pre_left, static_cast<typename Ctx::syntax_kind>(PostfixKind));
        }

        template <class Ctx, class Op, class Marker>
        static void end_postfix(Ctx& ctx, [[maybe_unused]] Op op, Marker m, lang::byte_span span) {
            ctx.events().end(m, span);
        }
    };

    // ---- pratt_expression<Table, PrimaryRule, Action> -----------------------
    // Internal helpers to detect structured vs flat action variants.
    namespace detail {
        // Detects structured begin_infix (3-arg: ctx, op, pre_left marker).
        template <class Action, class Ctx, class Op, class Marker>
        concept infix_is_structured = requires(Ctx& ctx, Op op, Marker m) {
            { Action::begin_infix(ctx, op, m) };
        };

        template <class Action, class Ctx, class Op>
        concept prefix_returns_marker = requires(Ctx& ctx, Op op) {
            { Action::begin_prefix(ctx, op) } -> std::same_as<void>;
        } == false;
    }

    template <class Table, class PrimaryRule, class Action = flat_pratt_action>
    struct pratt_expression {
        PrimaryRule primary;

        // parse_expr(ctx, min_bp) — the Pratt loop.
        template <class Ctx>
        [[nodiscard]] auto parse_expr(Ctx& ctx, std::uint8_t min_bp = 0) const
            -> parse_result<typename Ctx::stream_type>
        {
            using Stream = typename Ctx::stream_type;
            using R      = parse_result<Stream>;
            using TK     = typename Ctx::token_kind;
            using Marker = typename std::remove_reference_t<decltype(ctx.events())>::marker;

            // Snapshot before prefix operator or primary — needed for retroactive
            // begin_node insertion when a postfix or infix operator follows.
            auto pre_left            = ctx.events().snapshot();
            std::uint32_t pre_left_pos = ctx.cursor().pos; // stream index at pre_left time

            // Parse prefix operator or fall through to primary.
            if (!ctx.cursor().at_end()) {
                const TK peak_k = ctx.cursor().peek().kind;
                const auto pbp = Table::prefix_bp(peak_k);
                if (pbp) {
                    if constexpr (detail::prefix_returns_marker<Action, Ctx, TK>) {
                        auto marker = Action::begin_prefix(ctx, peak_k);
                        const auto op_span = ctx.stream()[ctx.cursor().pos].span();
                        ctx.set_cursor(ctx.cursor().advance());
                        ctx.events().token(ctx.cursor().pos - 1);
                        auto rhs = parse_expr(ctx, *pbp);
                        if (!rhs.ok()) return rhs;
                        ctx.set_cursor(rhs.next);
                        // Hull: op_span through last token of RHS.
                        // Derive RHS span from the snapshot: find the token just before current pos.
                        const auto rhs_end_pos = ctx.cursor().pos;
                        lang::byte_span operand_span{};
                        if (rhs_end_pos > 0)
                            operand_span = ctx.stream()[rhs_end_pos - 1].span();
                        Action::end_prefix(ctx, peak_k, marker,
                                           lang::byte_span::hull(op_span, operand_span));
                    } else {
                        const auto tok_pos = ctx.cursor().pos;
                        ctx.set_cursor(ctx.cursor().advance());
                        ctx.events().token(tok_pos);
                        auto rhs = parse_expr(ctx, *pbp);
                        if (!rhs.ok()) return rhs;
                        ctx.set_cursor(rhs.next);
                    }
                    return R::success_at(ctx.cursor());
                }
            }

            // Primary rule.
            auto r0 = primary.match(ctx);
            if (!r0.ok()) return r0;
            ctx.set_cursor(r0.next);

            // Infix / postfix loop.
            while (!ctx.cursor().at_end()) {
                const TK k = ctx.cursor().peek().kind;

                // Postfix?
                const auto post_bp = Table::postfix_bp(k);
                if (post_bp && *post_bp >= min_bp) {
                    if constexpr (detail::infix_is_structured<Action, Ctx, TK, Marker>) {
                        // Hull spans from first token of operand to last (the postfix op token).
                        const auto operand_start_span = ctx.stream()[pre_left_pos].span();
                        auto marker = Action::begin_postfix(ctx, k, pre_left);
                        const auto op_span = ctx.stream()[ctx.cursor().pos].span();
                        ctx.set_cursor(ctx.cursor().advance());
                        ctx.events().token(ctx.cursor().pos - 1);
                        Action::end_postfix(ctx, k, marker,
                                            lang::byte_span::hull(operand_start_span, op_span));
                    } else {
                        const auto tok_pos = ctx.cursor().pos;
                        ctx.set_cursor(ctx.cursor().advance());
                        ctx.events().token(tok_pos);
                    }
                    // After postfix, update pre_left for any chained operator.
                    pre_left     = ctx.events().snapshot();
                    pre_left_pos = ctx.cursor().pos;
                    continue;
                }

                // Infix?
                const auto ibp = Table::infix_bp(k);
                if (!ibp || ibp->first < min_bp) break;

                if constexpr (detail::infix_is_structured<Action, Ctx, TK, Marker>) {
                    // Hull spans from first token of left operand to last token of RHS.
                    const auto left_start_span = ctx.stream()[pre_left_pos].span();
                    auto marker = Action::begin_infix(ctx, k, pre_left);
                    const auto tok_pos = ctx.cursor().pos;
                    ctx.set_cursor(ctx.cursor().advance());
                    ctx.events().token(tok_pos);
                    auto rhs = parse_expr(ctx, ibp->second);
                    if (!rhs.ok()) return rhs;
                    ctx.set_cursor(rhs.next);
                    const auto right_end_span = ctx.stream()[ctx.cursor().pos - 1].span();
                    Action::end_infix(ctx, k, marker,
                                      lang::byte_span::hull(left_start_span, right_end_span));
                } else {
                    const auto tok_pos = ctx.cursor().pos;
                    ctx.set_cursor(ctx.cursor().advance());
                    ctx.events().token(tok_pos);
                    auto rhs = parse_expr(ctx, ibp->second);
                    if (!rhs.ok()) return rhs;
                    ctx.set_cursor(rhs.next);
                }

                // Update pre_left: after a complete infix expression, a new infix might follow.
                pre_left     = ctx.events().snapshot();
                pre_left_pos = ctx.cursor().pos;
            }

            return R::success_at(ctx.cursor());
        }

        template <class Ctx>
        [[nodiscard]] auto match(Ctx& ctx) const
            -> parse_result<typename Ctx::stream_type>
        {
            return parse_expr(ctx, 0);
        }
    };

    template <class Table, class Primary, class Action = flat_pratt_action>
    [[nodiscard]] constexpr pratt_expression<Table,Primary,Action> pratt(Primary p) {
        return {std::move(p)};
    }

} // namespace lang::samasa
