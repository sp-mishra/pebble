// ============================================================================
// test_samasa_recovery.cpp — skip_until_sync, delete_unexpected, insert_missing.
// ============================================================================

#include "catch_amalgamated.hpp"
#include "languages/samasa/samasa.hpp"

namespace {
    enum class SK : std::uint8_t { root };

    enum class TK : std::uint8_t { eof, a, b, sync };

    using namespace lang::samasa;

    // sync_set containing TK::sync
    using MySyncSet = sync_set<TK::sync>;

    struct RecCtx {
        token_buffer<TK> buf;
        token_stream<TK> stream;
        event_stream<SK> events;
        lang::collecting_sink<diagnostic> sink;
        lang::parse_tree_stats stats;
        std::optional<parse_context<SK, TK>> ctx;

        parse_context<SK, TK>& operator*() { return *ctx; }

        explicit RecCtx(std::initializer_list<TK> kinds) {
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
} // anonymous namespace

// ============================================================================

TEST_CASE (
"recovery: skip_until_sync skips tokens until sync token"
,
"[samasa][recovery]"
)
 {
    // tokens: a, b, a, sync
    RecCtx rc{TK::a, TK::b, TK::a, TK::sync};
    skip_until_sync<MySyncSet> strategy;
    strategy(*rc.ctx);

    // Cursor must now sit at sync token (pos 3).
    REQUIRE(!rc.ctx->cursor().at_end());
    CHECK(rc.ctx->cursor().peek().kind == TK::sync);
    CHECK(rc.ctx->repairs() == 1);
}

TEST_CASE (
"recovery: skip_until_sync emits recover_skipped diagnostic"
,
"[samasa][recovery]"
)
 {
    RecCtx rc{TK::a, TK::b, TK::sync};
    skip_until_sync<MySyncSet> strategy;
    strategy(*rc.ctx);

    CHECK(rc.sink.has_errors());
    bool found = false;
    for (const auto& d : rc.sink.entries)
        if (d.kind == samasa_diag_code::recover_skipped) found = true;
    CHECK(found);
}

TEST_CASE (
"recovery: skip_until_sync emits error event into event_stream"
,
"[samasa][recovery]"
)
 {
    RecCtx rc{TK::a, TK::sync};
    const auto before = rc.events.event_count();
    skip_until_sync<MySyncSet> strategy;
    strategy(*rc.ctx);
    CHECK(rc.events.event_count() > before);
}

TEST_CASE (
"recovery: skip_until_sync stops at eof when no sync token"
,
"[samasa][recovery]"
)
 {
    RecCtx rc{TK::a, TK::b};
    skip_until_sync<MySyncSet> strategy;
    strategy(*rc.ctx);
    CHECK(rc.ctx->cursor().at_end());
}

TEST_CASE (
"recovery: skip_until_sync is no-op when already at sync"
,
"[samasa][recovery]"
)
 {
    RecCtx rc{TK::sync};
    const auto repairs_before = rc.ctx->repairs();
    skip_until_sync<MySyncSet> strategy;
    strategy(*rc.ctx);
    // No tokens skipped — no repair incremented.
    CHECK(rc.ctx->repairs() == repairs_before);
    CHECK(rc.ctx->cursor().pos == 0);
}

TEST_CASE (
"recovery: delete_unexpected consumes one token"
,
"[samasa][recovery]"
)
 {
    RecCtx rc{TK::a, TK::b};
    const auto pos_before = rc.ctx->cursor().pos;
    delete_unexpected du;
    du(*rc.ctx);

    CHECK(rc.ctx->cursor().pos == pos_before + 1);
    CHECK(rc.ctx->repairs() == 1);
}

TEST_CASE (
"recovery: delete_unexpected emits recover_deleted diagnostic"
,
"[samasa][recovery]"
)
 {
    RecCtx rc{TK::a};
    delete_unexpected du;
    du(*rc.ctx);

    CHECK(rc.sink.has_errors());
    bool found = false;
    for (const auto& d : rc.sink.entries)
        if (d.kind == samasa_diag_code::recover_deleted) found = true;
    CHECK(found);
}

TEST_CASE (
"recovery: delete_unexpected is no-op at end-of-stream"
,
"[samasa][recovery]"
)
 {
    RecCtx rc{};           // only eof token
    rc.ctx->set_cursor(rc.ctx->cursor().advance()); // force at_end
    const auto repairs_before = rc.ctx->repairs();
    delete_unexpected du;
    du(*rc.ctx);
    CHECK(rc.ctx->repairs() == repairs_before);
}

TEST_CASE (
"recovery: insert_missing increments repair count"
,
"[samasa][recovery]"
)
 {
    RecCtx rc{TK::b};
    insert_missing<TK> im{TK::a};
    im(*rc.ctx);

    CHECK(rc.ctx->repairs() == 1);
    CHECK(rc.sink.has_errors());
}

TEST_CASE (
"recovery: insert_missing emits recover_inserted diagnostic"
,
"[samasa][recovery]"
)
 {
    RecCtx rc{TK::b};
    insert_missing<TK> im{TK::a};
    im(*rc.ctx);

    bool found = false;
    for (const auto& d : rc.sink.entries)
        if (d.kind == samasa_diag_code::recover_inserted) found = true;
    CHECK(found);
}

TEST_CASE (
"recovery: insert_missing is no-op when over repair limit"
,
"[samasa][recovery]"
)
 {
    limits budget;
    budget.max_repairs = 0;
    RecCtx rc{TK::b};
    // Reconstruct ctx with restrictive budget
    parse_context<SK, TK> ctx2(rc.stream, {}, rc.events, rc.sink, rc.stats, budget);
    insert_missing<TK> im{TK::a};
    im(ctx2);
    CHECK(ctx2.repairs() == 0);
}

// ============================================================================
// New tests [R16]: declarative recover_with combinator
// ============================================================================

namespace {
    // A pattern that always hard-fails (simulates an unrecoverable rule body).
    struct always_hard_fail {
        template <class Ctx>
        [[nodiscard]] auto match(Ctx& ctx) const {
            using R = parse_result<typename Ctx::stream_type>;
            return R::hard_failure(ctx.cursor());
        }
    };

    // A pattern that always soft-fails.
    struct always_soft_fail {
        template <class Ctx>
        [[nodiscard]] auto match(Ctx& ctx) const {
            using R = parse_result<typename Ctx::stream_type>;
            return R::soft_failure(ctx.cursor());
        }
    };

    // Recovery: skip_until_sync<MySyncSet> already defined above.
    using MyRecovery = skip_until_sync<MySyncSet>;

    // Full recover_with combinator: always_hard_fail wrapped with MyRecovery.
    using RecoverWithAlwaysFail = recover_with<always_hard_fail, MyRecovery>;
} // anonymous namespace

TEST_CASE (
"recover_with: hard-fail pattern is recovered; parse continues"
,
"[samasa][recovery]"
)
 {
    // tokens: a, b, sync — hard-fail triggers skip_until_sync.
    RecCtx rc{TK::a, TK::b, TK::sync};
    RecoverWithAlwaysFail rw{{}, {}};
    auto r = rw.match(*rc.ctx);
    // Result must be success — recover_with converts hard_fail into success.
    CHECK(r.ok());
}

TEST_CASE (
"recover_with: recovery emits diagnostic into sink"
,
"[samasa][recovery]"
)
 {
    RecCtx rc{TK::a, TK::sync};
    RecoverWithAlwaysFail rw{{}, {}};
    [[maybe_unused]] auto r = rw.match(*rc.ctx);
    CHECK(rc.sink.has_errors());
}

TEST_CASE (
"recover_with: recovery emits error event into event_stream"
,
"[samasa][recovery]"
)
 {
    RecCtx rc{TK::a, TK::sync};
    const auto before = rc.events.event_count();
    RecoverWithAlwaysFail rw{{}, {}};
    [[maybe_unused]] auto r = rw.match(*rc.ctx);
    CHECK(rc.events.event_count() > before);
}

TEST_CASE (
"recover_with: soft-fail from pattern is passed through unchanged"
,
"[samasa][recovery]"
)
 {
    RecCtx rc{TK::a};
    auto rw = make_recover_with(always_soft_fail{}, MyRecovery{});
    auto r  = rw.match(*rc.ctx);
    CHECK(r.soft_fail()); // soft_fail is transparent (not converted to success)
    CHECK(rc.ctx->cursor().pos == 0); // nothing consumed
}
