#pragma once

// samasa/dsl/combinators.hpp — PEG combinator set.
//
// C++23, header-only, no virtual, no macros. Namespace: lang::samasa
//
// All combinators are structs — pure type-level composition, minimal storage.
//
// seq(A,B,...)       — sequential: match A then B then ... (rollback on soft_fail)
// choice(A,B,...)    — ordered choice: PEG ordered semantics; full-checkpoint rewind
//                      (cursor + events + diagnostics + repairs) on soft_fail; hard_fail
//                      propagates immediately without trying later alternatives.
// opt(A)             — optional: always succeeds
// many(A)            — zero-or-more; infinite-loop guard built in
// many1(A)           — one-or-more
// sep_by(A,Sep)      — zero-or-more A separated by Sep
// sep_by1(A,Sep)     — one-or-more A separated by Sep
// lookahead(A)       — positive lookahead: no consumption
// not_followed_by(A) — negative lookahead: succeed iff A fails
// cut                — seq-local commit: upgrades subsequent soft_fail in the nearest
//                      enclosing seq_t to hard_fail; does NOT set any global context flag.

#include <cstdint>
#include <optional>
#include <tuple>
#include <utility>
#include "matcher.hpp"
#include "../core/result.hpp"

namespace lang::samasa { namespace detail {
        [[nodiscard]] constexpr std::uint32_t max_fe(std::uint32_t a, std::uint32_t b) noexcept {
            return a > b ? a : b;
        }
    }

    // -------------------------------------------------------------------------
    // cut — forward-declared here as a type tag so seq_t can detect it via
    // if constexpr. Implementation (match()) is at the bottom of this file.
    // -------------------------------------------------------------------------
    struct cut;

    // -------------------------------------------------------------------------
    // seq_t<Ms...>
    //
    // cut semantics (seq-local): when a cut matcher is encountered, a local
    // `committed` flag is raised. Any subsequent soft_fail is upgraded to
    // hard_fail. The flag does not persist outside this seq instance.
    // -------------------------------------------------------------------------

    template <class... Ms>
    struct seq_t {
        std::tuple<Ms...> matchers;

        template <class Ctx>
        [[nodiscard]] constexpr auto match(Ctx& ctx) const {
            using Stream = typename Ctx::stream_type;
            using R = parse_result<Stream>;

            const auto saved_cp = ctx.checkpoint();
            std::uint32_t fe = 0;
            parse_status worst = parse_status::success;
            bool committed = false; // seq-local cut flag

            auto try_one = [&]<std::size_t I>() -> bool {
                if (worst == parse_status::hard_fail) return false;
                // Detect cut via if constexpr — raise committed, do not call match.
                if constexpr (std::is_same_v < std::tuple_element_t < I, std::tuple<Ms...> >, cut >) {
                    committed = true;
                    return true;
                }
                else {
                    auto r = std::get < I > (matchers).match(ctx);
                    fe = detail::max_fe(fe, r.furthest_error);
                    if (r.ok()) {
                        ctx.set_cursor(r.next);
                        return true;
                    }
                    // Upgrade soft_fail to hard_fail when cut has been seen.
                    if (r.hard_fail() || committed) {
                        worst = parse_status::hard_fail;
                    }
                    else {
                        worst = parse_status::soft_fail;
                        ctx.rollback(saved_cp);
                    }
                    return false;
                }
            };

            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
                (try_one.template operator()<Is>() && ...);
            }(std::index_sequence_for < Ms...>{});

            ctx.update_furthest(fe);
            if (worst == parse_status::success) return R::success_at(ctx.cursor(), fe);
            if (worst == parse_status::hard_fail) return R::hard_failure(ctx.cursor(), fe);
            return R::soft_failure(saved_cp.cursor, fe);
        }
    };

    template <class... Ms>
    [[nodiscard]] constexpr seq_t<Ms...> seq(Ms... ms) {
        return {std::tuple < Ms...>{std::move(ms)...}};
    }

    // -------------------------------------------------------------------------
    // choice_t<Ms...>
    //
    // PEG ordered choice: tries A first; only tries B if A soft-fails.
    // Full checkpoint rollback (cursor + events + diagnostics + repairs) on
    // soft_fail ensures failed alternatives leave no observable side-effects.
    // hard_fail propagates immediately without trying later alternatives.
    // -------------------------------------------------------------------------

    template <class... Ms>
    struct choice_t {
        std::tuple<Ms...> matchers;

        template <class Ctx>
        [[nodiscard]] constexpr auto match(Ctx& ctx) const {
            using Stream = typename Ctx::stream_type;
            using R = parse_result<Stream>;

            if constexpr ((requires { Ms::token_kind; } && ...)) {
                const auto cur = ctx.cursor();
                if (cur.at_end()) return R::soft_failure(cur);
                std::optional<R> selected;
                auto dispatch = [&]<std::size_t I>() {
                    using M = std::tuple_element_t<I, std::tuple<Ms...>>;
                    if (!selected && cur.peek().kind == M::token_kind)
                        selected = std::get < I > (matchers).match(ctx);
                };
                [&]<std::size_t... Is>(std::index_sequence<Is...>) { (dispatch.template operator()<Is>(), ...); }(
                    std::index_sequence_for < Ms...>{});
                if (selected) {
                    ctx.set_cursor(selected->next);
                    return *selected;
                }
                ctx.update_furthest(cur.peek().offset);
                return R::soft_failure(cur);
            }

            std::uint32_t fe = 0;
            std::optional<R> found;

            auto try_one = [&]<std::size_t I>() -> bool {
                if (found) return true;
                const auto saved_cp = ctx.checkpoint();
                auto r = std::get < I > (matchers).match(ctx);
                fe = detail::max_fe(fe, r.furthest_error);
                if (r.ok()) {
                    found = r;
                    return true;
                }
                if (r.hard_fail()) {
                    found = r;
                    return true;
                }
                // soft_fail: full checkpoint rollback (cursor + events + diags + repairs)
                ctx.rollback(saved_cp);
                return false;
            };

            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
                ((try_one.template operator()<Is>()), ...);
            }(std::index_sequence_for < Ms...>{});

            ctx.update_furthest(fe);
            if (found) {
                if (found->ok()) ctx.set_cursor(found->next);
                return *found;
            }
            return R::soft_failure(ctx.cursor(), fe);
        }
    };

    template <class... Ms>
    [[nodiscard]] constexpr choice_t<Ms...> choice(Ms... ms) {
        return {std::tuple < Ms...>{std::move(ms)...}};
    }

    // -------------------------------------------------------------------------
    // opt_t<M>
    // -------------------------------------------------------------------------

    template <class M>
    struct opt_t {
        M m;

        template <class Ctx>
        [[nodiscard]] constexpr auto match(Ctx& ctx) const {
            using Stream = typename Ctx::stream_type;
            using R = parse_result<Stream>;
            auto r = m.match(ctx);
            if (r.ok()) {
                ctx.set_cursor(r.next);
                return r;
            }
            if (r.hard_fail()) return r;
            // soft_fail → succeed without consuming, cursor unchanged
            return R::success_at(ctx.cursor(), r.furthest_error);
        }
    };

    template <class M>
    [[nodiscard]] constexpr opt_t<M> opt(M m) { return {std::move(m)}; }

    // -------------------------------------------------------------------------
    // many_t<M> (zero-or-more)
    // -------------------------------------------------------------------------

    template <class M>
    struct many_t {
        M m;

        template <class Ctx>
        [[nodiscard]] constexpr auto match(Ctx& ctx) const {
            using Stream = typename Ctx::stream_type;
            using R = parse_result<Stream>;
            std::uint32_t fe = 0;
            while (true) {
                const auto before = ctx.cursor();
                auto r = m.match(ctx);
                fe = detail::max_fe(fe, r.furthest_error);
                if (r.hard_fail()) {
                    ctx.update_furthest(fe);
                    return r;
                }
                if (!r.ok()) break;
                ctx.set_cursor(r.next);
                if (ctx.cursor().pos == before.pos) break; // infinite-loop guard
            }
            ctx.update_furthest(fe);
            return R::success_at(ctx.cursor(), fe);
        }
    };

    template <class M>
    [[nodiscard]] constexpr many_t<M> many(M m) { return {std::move(m)}; }

    // -------------------------------------------------------------------------
    // many1_t<M> (one-or-more)
    // -------------------------------------------------------------------------

    template <class M>
    struct many1_t {
        M m;

        template <class Ctx>
        [[nodiscard]] constexpr auto match(Ctx& ctx) const {
            using Stream = typename Ctx::stream_type;
            using R = parse_result<Stream>;
            auto r0 = m.match(ctx);
            if (!r0.ok()) return r0;
            ctx.set_cursor(r0.next);
            std::uint32_t fe = r0.furthest_error;
            while (true) {
                const auto before = ctx.cursor();
                auto r = m.match(ctx);
                fe = detail::max_fe(fe, r.furthest_error);
                if (r.hard_fail()) {
                    ctx.update_furthest(fe);
                    return r;
                }
                if (!r.ok()) break;
                ctx.set_cursor(r.next);
                if (ctx.cursor().pos == before.pos) break;
            }
            ctx.update_furthest(fe);
            return R::success_at(ctx.cursor(), fe);
        }
    };

    template <class M>
    [[nodiscard]] constexpr many1_t<M> many1(M m) { return {std::move(m)}; }

    // -------------------------------------------------------------------------
    // sep_by_t<A,Sep>
    // -------------------------------------------------------------------------

    template <class A, class Sep>
    struct sep_by_t {
        A a;
        Sep sep;

        template <class Ctx>
        [[nodiscard]] constexpr auto match(Ctx& ctx) const {
            using Stream = typename Ctx::stream_type;
            using R = parse_result<Stream>;
            std::uint32_t fe = 0;
            auto r0 = a.match(ctx);
            fe = detail::max_fe(fe, r0.furthest_error);
            if (r0.hard_fail()) return r0;
            if (!r0.ok()) return R::success_at(ctx.cursor(), fe);
            ctx.set_cursor(r0.next);
            while (true) {
                const auto before_cp = ctx.checkpoint();
                auto rs = sep.match(ctx);
                fe = detail::max_fe(fe, rs.furthest_error);
                if (rs.hard_fail()) return rs;
                if (!rs.ok()) break;
                ctx.set_cursor(rs.next);
                auto ra = a.match(ctx);
                fe = detail::max_fe(fe, ra.furthest_error);
                if (ra.hard_fail()) return ra;
                if (!ra.ok()) {
                    ctx.rollback(before_cp);
                    break;
                }
                ctx.set_cursor(ra.next);
            }
            ctx.update_furthest(fe);
            return R::success_at(ctx.cursor(), fe);
        }
    };

    template <class A, class Sep>
    [[nodiscard]] constexpr sep_by_t<A, Sep> sep_by(A a, Sep sep) { return {std::move(a), std::move(sep)}; }

    // -------------------------------------------------------------------------
    // sep_by1_t<A,Sep>
    // -------------------------------------------------------------------------

    template <class A, class Sep>
    struct sep_by1_t {
        A a;
        Sep sep;

        template <class Ctx>
        [[nodiscard]] constexpr auto match(Ctx& ctx) const {
            using Stream = typename Ctx::stream_type;
            using R = parse_result<Stream>;
            const auto before = ctx.cursor();
            auto r = sep_by_t<A, Sep>{a, sep}.match(ctx);
            if (r.ok() && ctx.cursor().pos == before.pos)
                return R::soft_failure(before, r.furthest_error);
            return r;
        }
    };

    template <class A, class Sep>
    [[nodiscard]] constexpr sep_by1_t<A, Sep> sep_by1(A a, Sep sep) { return {std::move(a), std::move(sep)}; }

    // -------------------------------------------------------------------------
    // lookahead_t<M>
    // -------------------------------------------------------------------------

    template <class M>
    struct lookahead_t {
        M m;

        template <class Ctx>
        [[nodiscard]] constexpr auto match(Ctx& ctx) const {
            using Stream = typename Ctx::stream_type;
            using R = parse_result<Stream>;
            const auto saved_cp = ctx.checkpoint();
            auto r = m.match(ctx);
            ctx.rollback(saved_cp);
            if (r.ok()) return R::success_at(saved_cp.cursor, r.furthest_error);
            return R::soft_failure(saved_cp.cursor, r.furthest_error);
        }
    };

    template <class M>
    [[nodiscard]] constexpr lookahead_t<M> lookahead(M m) { return {std::move(m)}; }

    // -------------------------------------------------------------------------
    // not_followed_by_t<M>
    // -------------------------------------------------------------------------

    template <class M>
    struct not_followed_by_t {
        M m;

        template <class Ctx>
        [[nodiscard]] constexpr auto match(Ctx& ctx) const {
            using Stream = typename Ctx::stream_type;
            using R = parse_result<Stream>;
            const auto saved_cp = ctx.checkpoint();
            auto r = m.match(ctx);
            ctx.rollback(saved_cp);
            if (r.ok()) return R::soft_failure(saved_cp.cursor, r.furthest_error);
            return R::success_at(saved_cp.cursor, r.furthest_error);
        }
    };

    template <class M>
    [[nodiscard]] constexpr not_followed_by_t<M> not_followed_by(M m) { return {std::move(m)}; }

    // -------------------------------------------------------------------------
    // cut — seq-local commit signal.
    // When encountered inside seq_t, seq detects it via if constexpr and raises
    // the local committed flag without calling match(). If used outside seq (e.g.,
    // directly in choice), match() succeeds without consuming — no commit effect.
    // -------------------------------------------------------------------------

    struct cut {
        template <class Ctx>
        [[nodiscard]] constexpr auto match(Ctx& ctx) const {
            using Stream = typename Ctx::stream_type;
            return parse_result<Stream>::success_at(ctx.cursor());
        }
    };
} // namespace lang::samasa
