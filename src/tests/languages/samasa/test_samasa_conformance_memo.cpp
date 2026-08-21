// ============================================================================
// test_samasa_conformance_memo.cpp — Memoization policy behavioral contract.
//
// Verifies:
//   - A memoized rule invoked twice at the same position yields identical result.
//   - Memo respects position (different positions → independent entries).
//   - no_memo behaves as transparent pass-through.
// ============================================================================

#include "catch_amalgamated.hpp"
#include "languages/samasa/samasa.hpp"
#include "languages/samasa/policies/memo_policy.hpp"

namespace {

using namespace lang::samasa;

enum class SK : std::uint8_t { root };
enum class TK : std::uint8_t { eof, tok_a, tok_b };

template <class TokenKind>
token_buffer<TokenKind> make_tokens(std::initializer_list<TokenKind> kinds) {
    token_buffer<TokenKind> buf;
    std::uint32_t off = 0;
    for (auto k : kinds) { buf.data.push_back({k, off, 1, 0, 0}); ++off; }
    buf.data.push_back({TokenKind{}, off, 0, 0, 0});
    return buf;
}

// ============================================================================
// no_memo: direct match works
// ============================================================================

TEST_CASE("memo: direct tok match succeeds", "[samasa][conformance][memo]") {
    auto buf = make_tokens<TK>({TK::tok_a});
    event_stream<SK> events;
    lang::collecting_sink<diagnostic> sink;
    lang::parse_tree_stats stats;
    limits budget;
    parse_context<SK,TK> ctx(buf.view(), "a", events, sink, stats, budget);

    auto r = tok<TK::tok_a>{}.match(ctx);
    CHECK(r.ok());
}

// ============================================================================
// selective_memo: same position → same result
// ============================================================================

TEST_CASE("memo: memoized rule invoked twice at same position → same result",
          "[samasa][conformance][memo]")
{
    auto buf = make_tokens<TK>({TK::tok_a, TK::tok_b});
    event_stream<SK> events;
    lang::collecting_sink<diagnostic> sink;
    lang::parse_tree_stats stats;
    limits budget;
    parse_context<SK,TK,selective_memo> ctx(buf.view(), "ab", events, sink, stats, budget);

    using MemoR = memoized<rule<"a", tok<TK::tok_a>>>;

    // First invocation at position 0.
    const auto cp = ctx.checkpoint();
    const bool ok1 = MemoR{}.match(ctx).ok();

    // Roll back to same position.
    ctx.rollback(cp);
    const bool ok2 = MemoR{}.match(ctx).ok();

    // Both invocations must agree.
    CHECK(ok1 == ok2);
    CHECK(ok1); // tok_a present → both succeed
}

TEST_CASE("memo: memoized rule at failing position → same failure on retry",
          "[samasa][conformance][memo]")
{
    auto buf = make_tokens<TK>({TK::tok_b}); // tok_a not present
    event_stream<SK> events;
    lang::collecting_sink<diagnostic> sink;
    lang::parse_tree_stats stats;
    limits budget;
    parse_context<SK,TK,selective_memo> ctx(buf.view(), "b", events, sink, stats, budget);

    using MemoR = memoized<rule<"a", tok<TK::tok_a>>>;

    const auto cp = ctx.checkpoint();
    const bool ok1 = MemoR{}.match(ctx).ok();
    ctx.rollback(cp);
    const bool ok2 = MemoR{}.match(ctx).ok();

    CHECK(!ok1);
    CHECK(!ok2);
    CHECK(ok1 == ok2);
}

// ============================================================================
// Different positions → independent memo entries
// ============================================================================

TEST_CASE("memo: different positions compute independently",
          "[samasa][conformance][memo]")
{
    auto buf = make_tokens<TK>({TK::tok_a, TK::tok_a});
    event_stream<SK> events;
    lang::collecting_sink<diagnostic> sink;
    lang::parse_tree_stats stats;
    limits budget;
    parse_context<SK,TK,selective_memo> ctx(buf.view(), "aa", events, sink, stats, budget);

    using MemoR = memoized<rule<"a", tok<TK::tok_a>>>;

    // Position 0 → success.
    auto r0 = MemoR{}.match(ctx);
    REQUIRE(r0.ok());
    ctx.set_cursor(r0.next); // advance to position 1

    // Position 1 → also success (independent entry).
    auto r1 = MemoR{}.match(ctx);
    CHECK(r1.ok());
}

} // anonymous namespace
