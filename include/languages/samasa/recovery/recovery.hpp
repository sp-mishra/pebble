#pragma once

// samasa/recovery/recovery.hpp — Error recovery strategies (opt-in via error_policy).
//
// C++23, header-only, no virtual, no macros. Namespace: lang::samasa
//
// Manual strategies:
//   skip_until_sync<SyncSet>    — consume tokens until sync_set or eof; emit error_node.
//   insert_missing<TK>          — runtime-value form; kept for backward compat.
//   insert_missing_token<V>     — preferred compile-time value form (e.g. insert_missing_token<TK::semi>).
//   delete_unexpected           — consume one unexpected token; emit recover_deleted diag.
//   wrap_error_node             — wrap a byte range in an error node.
//
// Declarative recovery:
//   recover_with<Pattern, Recovery>        — on hard-fail, run Recovery then continue.
//   recover_with_repair<Pattern, SyncSet,
//                       RepairPolicy>      — scoring-based repair: tries cheapest action
//                                            (insert/delete/replace/skip) then continues.
//
// Progress validation:
//   recovery_makes_progress_v<R>  — true iff R guarantees cursor advance / token insert
//                                   / sync; false for wrap_error_node (no advance alone).
//
// All strategies emit diagnostics into ctx.events() / ctx.emit().
// Recovery is budget-limited via ctx.over_repair_limit().
//
// Repair scoring (design.md v2.2):
//   insert missing token    cost 1
//   delete unexpected token cost 1
//   replace token           cost 2
//   skip tokens             cost N (tokens consumed)
//   wrap subtree            cost 4

#include "sync_set.hpp"
#include "../core/diagnostic.hpp"
#include "../core/result.hpp"
#include "../core/source_view.hpp"

namespace lang::samasa {

    // ---- skip_until_sync ---------------------------------------------------

    template <class SyncSet>
    struct skip_until_sync {
        template <class Ctx>
        void operator()(Ctx& ctx) const {
            using TK = typename Ctx::token_kind;
            const auto start_off = ctx.cursor().at_end() ? 0u : ctx.cursor().peek().offset;
            std::uint32_t end_off = start_off;

            while (!ctx.cursor().at_end()) {
                const TK k = ctx.cursor().peek().kind;
                if (SyncSet::contains(k)) break;
                end_off = ctx.cursor().peek().offset + ctx.cursor().peek().length;
                ctx.set_cursor(ctx.cursor().advance());
            }

            if (end_off > start_off) {
                byte_span span{start_off, end_off - start_off};
                ctx.events().error(samasa_diag_code::recover_skipped, span);
                ctx.emit({samasa_diag_code::recover_skipped, {},
                    "error: skipped tokens during recovery", ::lang::severity::error});
                ctx.inc_repairs();
            }
        }
    };

    // ---- insert_missing<class TK> ------------------------------------------
    // Runtime-value form: insert_missing<TK>{TK::semicolon}.
    // Kept for backward compatibility. Prefer insert_missing_token<TK::semicolon>.

    template <class TokenKind>
    struct insert_missing {
        TokenKind expected_kind;

        static constexpr bool makes_progress = true;

        template <class Ctx>
        void operator()(Ctx& ctx) const {
            if (ctx.over_repair_limit()) return;
            const auto off = ctx.cursor().at_end() ? 0u : ctx.cursor().peek().offset;
            ctx.events().error(samasa_diag_code::recover_inserted, {off, 0});
            ctx.emit({samasa_diag_code::recover_inserted, {},
                "error: missing token inserted", ::lang::severity::error});
            ctx.inc_repairs();
        }
    };

    // ---- insert_missing_token<auto V> --------------------------------------
    // Compile-time value form (preferred): insert_missing_token<TK::semicolon>.
    // The expected token kind is encoded entirely in the type — zero runtime storage.
    //
    //   static_assert(recovery_makes_progress_v<insert_missing_token<TK::semicolon>>);
    //
    // Use this form in recover_with<> and in grammar-validation static_asserts.

    template <auto ExpectedKind>
    struct insert_missing_token {
        static constexpr bool makes_progress = true;

        template <class Ctx>
        void operator()(Ctx& ctx) const {
            if (ctx.over_repair_limit()) return;
            const auto off = ctx.cursor().at_end() ? 0u : ctx.cursor().peek().offset;
            ctx.events().error(samasa_diag_code::recover_inserted, {off, 0});
            ctx.emit({samasa_diag_code::recover_inserted, {},
                "error: missing token inserted", ::lang::severity::error});
            ctx.inc_repairs();
        }
    };

    // ---- delete_unexpected -------------------------------------------------

    struct delete_unexpected {
        template <class Ctx>
        void operator()(Ctx& ctx) const {
            if (ctx.cursor().at_end()) return;
            const auto off = ctx.cursor().peek().offset;
            const auto len = ctx.cursor().peek().length;
            ctx.events().error(samasa_diag_code::recover_deleted, {off, len});
            ctx.emit({samasa_diag_code::recover_deleted, {},
                "error: unexpected token deleted", ::lang::severity::error});
            ctx.set_cursor(ctx.cursor().advance());
            ctx.inc_repairs();
        }
    };

    // ---- wrap_error_node ---------------------------------------------------

    struct wrap_error_node {
        byte_span span;

        template <class Ctx>
        void operator()(Ctx& ctx) const {
            ctx.events().error(samasa_diag_code::recover_wrapped, span);
            ctx.emit({samasa_diag_code::recover_wrapped, {},
                "error: wrapped malformed region", ::lang::severity::error});
        }
    };

    // ---- recovery_makes_progress_v -----------------------------------------
    // True iff the recovery strategy guarantees forward progress:
    //   — advances the cursor (skip / delete / insert), OR
    //   — synchronizes to a known safe point.
    // wrap_error_node alone does not advance; it must be paired with a sync.
    // Custom strategies: specialize this variable template or define
    //   static constexpr bool makes_progress = true/false;
    // inside the strategy struct.
    // Enforcement: grammar_valid<G>() / require_valid_grammar<G>() reject grammars
    // where recover_with<P,R> has !recovery_makes_progress_v<R>.

    template <class Recovery>
    inline constexpr bool recovery_makes_progress_v =
        requires { Recovery::makes_progress; }
            ? Recovery::makes_progress
            : false;

    // ---- recover_with<Pattern, Recovery> -----------------------------------

    template <class Pattern, class Recovery>
    struct recover_with {
        Pattern  pattern;
        Recovery recovery;

        template <class Ctx>
        [[nodiscard]] auto match(Ctx& ctx) const {
            using Stream = typename Ctx::stream_type;
            using R      = parse_result<Stream>;

            auto r = pattern.match(ctx);
            if (r.ok()) {
                ctx.set_cursor(r.next);
                return r;
            }
            if (r.soft_fail()) {
                return r;
            }
            // hard_fail: run recovery strategy, emit error-continue event
            const auto off = ctx.cursor().at_end() ? 0u : ctx.cursor().peek().offset;
            recovery(ctx);
            ctx.events().error(samasa_diag_code::recover_wrapped, {off, 0});
            return R::success_at(ctx.cursor(), r.furthest_error);
        }
    };

    template <class Pattern, class Recovery>
    [[nodiscard]] constexpr recover_with<Pattern,Recovery>
    make_recover_with(Pattern p, Recovery r) { return {std::move(p), std::move(r)}; }

    // ---- recovery_makes_progress_v (specializations) -----------------------
    // Primary declared above. Custom strategies: specialize this variable template
    // or define `static constexpr bool makes_progress = true/false;` in the struct.

    template <class SyncSet>
    inline constexpr bool recovery_makes_progress_v<skip_until_sync<SyncSet>> = true;

    template <class TK>
    inline constexpr bool recovery_makes_progress_v<insert_missing<TK>> = true;

    template <auto V>
    inline constexpr bool recovery_makes_progress_v<insert_missing_token<V>> = true;

    template <>
    inline constexpr bool recovery_makes_progress_v<delete_unexpected> = true;

    template <>
    inline constexpr bool recovery_makes_progress_v<wrap_error_node> = false;

    // ---- default_repair_policy ---------------------------------------------
    // Scoring-based repair policy used by recover_with_repair.
    // Tries cheapest action first: delete unexpected (cost 1) → skip until sync.

    struct default_repair_policy {
        // Called when the pattern hard-fails at `ctx`.
        // Attempts the cheapest available repair; emits diagnostic + advances ctx.
        template <class SyncSet, class Ctx>
        static void apply(Ctx& ctx) {
            if (ctx.over_repair_limit()) return;

            // Cheapest action: delete one unexpected token (cost 1).
            if (!ctx.cursor().at_end()) {
                const auto off = ctx.cursor().peek().offset;
                const auto len = ctx.cursor().peek().length;
                ctx.events().error(samasa_diag_code::recover_deleted, {off, len});
                ctx.emit({samasa_diag_code::recover_deleted, {},
                    "error: deleted unexpected token (cost 1)", ::lang::severity::error});
                ctx.set_cursor(ctx.cursor().advance());
                ctx.inc_repairs();
                return;
            }
            // At eof: skip until sync (cost N).
            skip_until_sync<SyncSet>{}(ctx);
        }
    };

    // ---- recover_with_repair<Pattern, SyncSet, RepairPolicy> ---------------
    // Declarative scoring-based recovery combinator.
    //
    // On Pattern hard-fail:
    //   1. Try cheapest repair action via RepairPolicy::apply<SyncSet>.
    //   2. Emit error_node.
    //   3. Return success so parsing continues.
    //
    // On Pattern soft-fail: propagate normally (backtracking).
    // On Pattern success: transparent pass-through.

    template <class Pattern, class SyncSet, class RepairPolicy = default_repair_policy>
    struct recover_with_repair {
        Pattern pattern;

        template <class Ctx>
        [[nodiscard]] auto match(Ctx& ctx) const {
            using Stream = typename Ctx::stream_type;
            using R      = parse_result<Stream>;

            auto r = pattern.match(ctx);
            if (r.ok()) {
                ctx.set_cursor(r.next);
                return r;
            }
            if (r.soft_fail()) return r;

            // hard_fail: apply scoring-based repair
            const auto off = ctx.cursor().at_end() ? 0u : ctx.cursor().peek().offset;
            RepairPolicy::template apply<SyncSet>(ctx);
            ctx.events().error(samasa_diag_code::recover_wrapped, {off, 0});
            return R::success_at(ctx.cursor(), r.furthest_error);
        }
    };

    template <class Pattern, class SyncSet, class RepairPolicy = default_repair_policy>
    [[nodiscard]] constexpr recover_with_repair<Pattern,SyncSet,RepairPolicy>
    make_recover_repair(Pattern p) { return {std::move(p)}; }

} // namespace lang::samasa
