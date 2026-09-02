// ============================================================================
// test_samasa_combinators.cpp — seq/choice/opt/many/sep_by, cut, backtracking.
// ============================================================================

#include "catch_amalgamated.hpp"
#include "languages/samasa/samasa.hpp"

namespace {
    enum class SK : std::uint8_t { root };

    enum class TK : std::uint8_t { eof, a, b, c };

    using namespace lang::samasa;

    // Build a minimal context from a list of token kinds.
    struct TestCtx {
        token_buffer<TK> buf;
        token_stream<TK> stream;
        event_stream<SK> events;
        lang::collecting_sink<diagnostic> sink;
        lang::parse_tree_stats stats;
        std::optional<parse_context<SK, TK>> ctx;

        parse_context<SK, TK>& operator*() { return *ctx; }

        explicit TestCtx(std::initializer_list<TK> kinds) {
            std::uint32_t off = 0;
            for (TK k : kinds) {
                buf.data.push_back({k, off, 1, 0, 0});
                ++off;
            }
            buf.data.push_back({TK::eof, off, 0, 0, 0});
            stream = buf.view();
            ctx.emplace(stream, std::string_view{}, events, sink, stats);
        }
    };

    // Matcher that emits one diagnostic into ctx then soft-fails.
    struct emit_then_soft_fail {
        template <class Ctx>
        [[nodiscard]] auto match(Ctx& ctx) const {
            ctx.emit({
                samasa_diag_code::recover_skipped, {},
                "test: pre-fail diagnostic", ::lang::severity::error
            });
            using R = parse_result<typename Ctx::stream_type>;
            return R::soft_failure(ctx.cursor());
        }
    };

    // Matcher that bumps repair count then soft-fails.
    struct inc_repair_then_fail {
        template <class Ctx>
        [[nodiscard]] auto match(Ctx& ctx) const {
            ctx.inc_repairs();
            using R = parse_result<typename Ctx::stream_type>;
            return R::soft_failure(ctx.cursor());
        }
    };
} // anonymous namespace

// ============================================================================

TEST_CASE (
"combinators: seq matches in order"
,
"[samasa][combinators]"
)
 {
    TestCtx tc{TK::a, TK::b, TK::c};
    auto r = seq(tok<TK::a>{}, tok<TK::b>{}, tok<TK::c>{}).match(*tc.ctx);
    CHECK(r.ok());
    CHECK(tc.ctx->cursor().pos == 3);
}

TEST_CASE (
"combinators: seq rolls back on failure"
,
"[samasa][combinators]"
)
 {
    TestCtx tc{TK::a, TK::c}; // no b in between
    auto saved = tc.ctx->cursor();
    auto r = seq(tok<TK::a>{}, tok<TK::b>{}).match(*tc.ctx);
    CHECK(r.soft_fail());
    CHECK(tc.ctx->cursor().pos == saved.pos);
}

TEST_CASE (
"combinators: choice picks first match"
,
"[samasa][combinators]"
)
 {
    TestCtx tc{TK::b};
    auto r = choice(tok<TK::a>{}, tok<TK::b>{}, tok<TK::c>{}).match(*tc.ctx);
    CHECK(r.ok());
    CHECK(tc.ctx->cursor().pos == 1);
}

TEST_CASE (
"combinators: choice fails when all alternatives fail"
,
"[samasa][combinators]"
)
 {
    TestCtx tc{TK::c};
    auto r = choice(tok<TK::a>{}, tok<TK::b>{}).match(*tc.ctx);
    CHECK(r.soft_fail());
    CHECK(tc.ctx->cursor().pos == 0);
}

TEST_CASE (
"combinators: opt succeeds even when inner fails"
,
"[samasa][combinators]"
)
 {
    TestCtx tc{TK::c};
    auto r = opt(tok<TK::a>{}).match(*tc.ctx);
    CHECK(r.ok());
    CHECK(tc.ctx->cursor().pos == 0); // nothing consumed
}

TEST_CASE (
"combinators: opt succeeds and consumes when inner matches"
,
"[samasa][combinators]"
)
 {
    TestCtx tc{TK::a};
    auto r = opt(tok<TK::a>{}).match(*tc.ctx);
    CHECK(r.ok());
    CHECK(tc.ctx->cursor().pos == 1);
}

TEST_CASE (
"combinators: many consumes zero tokens on no match"
,
"[samasa][combinators]"
)
 {
    TestCtx tc{TK::b};
    auto r = many(tok<TK::a>{}).match(*tc.ctx);
    CHECK(r.ok());
    CHECK(tc.ctx->cursor().pos == 0);
}

TEST_CASE (
"combinators: many consumes all matching tokens"
,
"[samasa][combinators]"
)
 {
    TestCtx tc{TK::a, TK::a, TK::a, TK::b};
    auto r = many(tok<TK::a>{}).match(*tc.ctx);
    CHECK(r.ok());
    CHECK(tc.ctx->cursor().pos == 3);
}

TEST_CASE (
"combinators: sep_by matches list"
,
"[samasa][combinators]"
)
 {
    // a , a , a  where b acts as separator
    TestCtx tc{TK::a, TK::b, TK::a, TK::b, TK::a};
    auto r = sep_by(tok<TK::a>{}, tok<TK::b>{}).match(*tc.ctx);
    CHECK(r.ok());
    CHECK(tc.ctx->cursor().pos == 5);
}

TEST_CASE (
"combinators: sep_by succeeds on empty input (zero elements)"
,
"[samasa][combinators]"
)
 {
    TestCtx tc{TK::c};
    auto r = sep_by(tok<TK::a>{}, tok<TK::b>{}).match(*tc.ctx);
    CHECK(r.ok());
    CHECK(tc.ctx->cursor().pos == 0);
}

TEST_CASE (
"combinators: cut upgrades soft_fail to hard_fail in seq"
,
"[samasa][combinators]"
)
 {
    // seq(a, cut, b): after matching a and cut, b not found → hard_fail
    TestCtx tc{TK::a, TK::c};
    auto r = seq(tok<TK::a>{}, cut{}, tok<TK::b>{}).match(*tc.ctx);
    CHECK(r.hard_fail());
}

TEST_CASE (
"combinators: choice does not rewind on hard_fail"
,
"[samasa][combinators]"
)
 {
    // First branch: seq(a, cut, b) → hard_fail after consuming a
    // choice should propagate hard_fail, not try next branch
    TestCtx tc{TK::a, TK::c};
    auto r = choice(
        seq(tok<TK::a>{}, cut{}, tok<TK::b>{}),
        tok<TK::c>{}
    ).match(*tc.ctx);
    CHECK(r.hard_fail());
}

TEST_CASE (
"combinators: backtracking restores event_stream snapshot"
,
"[samasa][combinators]"
)
 {
    TestCtx tc{TK::b};
    const auto ev_before = tc.events.event_count();
    auto r = seq(tok<TK::a>{}, tok<TK::b>{}).match(*tc.ctx);
    CHECK(r.soft_fail());
    CHECK(tc.events.event_count() == ev_before); // rolled back
}

// ============================================================================
// New tests [R7]: diagnostic and repair rollback on backtracking
// ============================================================================

TEST_CASE (
"combinators: choice rolls back diagnostics emitted by failing first branch"
,
"[samasa][combinators]"
)
 {
    // choice(emit_then_fail, tok<b>) — first branch emits a diag then soft-fails;
    // second branch succeeds. Final sink must be empty.
    TestCtx tc{TK::b};
    const auto diag_before = tc.sink.size();
    auto r = choice(emit_then_soft_fail{}, tok<TK::b>{}).match(*tc.ctx);
    CHECK(r.ok());
    CHECK(tc.sink.size() == diag_before); // leaked diag rolled back
}

TEST_CASE (
"combinators: choice rolls back repairs emitted by failing first branch"
,
"[samasa][combinators]"
)
 {
    TestCtx tc{TK::b};
    const auto repairs_before = tc.ctx->repairs();
    auto r = choice(inc_repair_then_fail{}, tok<TK::b>{}).match(*tc.ctx);
    CHECK(r.ok());
    CHECK(tc.ctx->repairs() == repairs_before); // repair count rolled back
}

// ============================================================================
// New tests [R7]: cut is seq-local
// ============================================================================

TEST_CASE (
"combinators: cut inside inner seq causes hard-fail; outer choice does not try next branch"
,
"[samasa][combinators]"
)
 {
    // choice(seq(a, cut, b), c): inner seq matches a, commits (cut), then b fails → hard_fail.
    // choice must propagate hard_fail — c is NOT tried.
    TestCtx tc{TK::a, TK::c};
    auto r = choice(
        seq(tok<TK::a>{}, cut{}, tok<TK::b>{}),
        tok<TK::c>{}
    ).match(*tc.ctx);
    CHECK(r.hard_fail());
    // c was not consumed (cursor at pos 1 — only a consumed before hard-fail).
    CHECK(tc.ctx->cursor().pos == 1);
}

TEST_CASE (
"combinators: seq without cut still allows choice to try next branch on failure"
,
"[samasa][combinators]"
)
 {
    // choice(seq(a, b), c): seq(a,b) soft-fails (no b at pos 1), c matches.
    TestCtx tc{TK::a, TK::c};
    auto r = choice(
        seq(tok<TK::a>{}, tok<TK::b>{}),
        tok<TK::c>{}
    ).match(*tc.ctx);
    // choice should soft-fail because seq consumed a then failed — rollback puts us back at 0.
    // Then tok<c> at pos 0 is TK::a, not TK::c — so that fails too, soft-fail overall.
    // Actually tok<TK::a> at pos 0 succeeds, seq rolls back, then choice tries tok<TK::c>
    // at pos 0 which is TK::a — also fails. Result: soft_fail.
    // What matters: NO hard_fail.
    CHECK(!r.hard_fail());
}

// ============================================================================
// New tests [design.md]: PEG ordered-choice semantics
// ============================================================================

TEST_CASE (
"combinators: PEG ordered choice — first match wins, second never tried"
,
"[samasa][combinators]"
)
 {
    // choice(tok<a>, tok<b>): stream has [a, b].
    // PEG: tok<a> succeeds at pos 0 → tok<b> is NEVER tried even though it could match at pos 1.
    // After match, cursor must be at 1 (only a consumed).
    TestCtx tc{TK::a, TK::b};
    auto r = choice(tok<TK::a>{}, tok<TK::b>{}).match(*tc.ctx);
    CHECK(r.ok());
    CHECK(tc.ctx->cursor().pos == 1); // only a consumed; b untouched
}

TEST_CASE (
"combinators: PEG ordered choice — second alternative tried only on first soft-fail"
,
"[samasa][combinators]"
)
 {
    // choice(tok<b>, tok<a>): stream has [a]. tok<b> soft-fails, tok<a> is tried and succeeds.
    TestCtx tc{TK::a};
    auto r = choice(tok<TK::b>{}, tok<TK::a>{}).match(*tc.ctx);
    CHECK(r.ok());
    CHECK(tc.ctx->cursor().pos == 1);
}

TEST_CASE (
"combinators: lookahead does not consume tokens"
,
"[samasa][combinators]"
)
 {
    TestCtx tc{TK::a, TK::b};
    auto r = lookahead(tok<TK::a>{}).match(*tc.ctx);
    CHECK(r.ok());
    CHECK(tc.ctx->cursor().pos == 0); // no consumption
}

TEST_CASE (
"combinators: not_followed_by succeeds when inner fails"
,
"[samasa][combinators]"
)
 {
    TestCtx tc{TK::a};
    auto r = not_followed_by(tok<TK::b>{}).match(*tc.ctx);
    CHECK(r.ok());
    CHECK(tc.ctx->cursor().pos == 0);
}

TEST_CASE (
"combinators: not_followed_by fails when inner succeeds"
,
"[samasa][combinators]"
)
 {
    TestCtx tc{TK::a};
    auto r = not_followed_by(tok<TK::a>{}).match(*tc.ctx);
    CHECK(r.soft_fail());
    CHECK(tc.ctx->cursor().pos == 0);
}

TEST_CASE (
"combinators: many1 fails on empty match"
,
"[samasa][combinators]"
)
 {
    TestCtx tc{TK::b};
    auto r = many1(tok<TK::a>{}).match(*tc.ctx);
    CHECK(r.soft_fail());
    CHECK(tc.ctx->cursor().pos == 0);
}

TEST_CASE (
"combinators: sep_by1 fails on empty match"
,
"[samasa][combinators]"
)
 {
    TestCtx tc{TK::b};
    auto r = sep_by1(tok<TK::a>{}, tok<TK::b>{}).match(*tc.ctx);
    CHECK(!r.ok());
    CHECK(tc.ctx->cursor().pos == 0);
}
