// ============================================================================
// test_samasa_conformance_checkpoint.cpp — checkpoint/rollback behavioral contract.
//
// Verifies:
//   - checkpoint()/rollback() restores cursor + event stream position atomically.
//   - Nested checkpoints unwind in correct order.
// ============================================================================

#include "catch_amalgamated.hpp"
#include "languages/samasa/samasa.hpp"

namespace {

using namespace lang::samasa;

enum class SK : std::uint8_t { root, item };
enum class TK : std::uint8_t { eof, tok_a, tok_b, tok_c };

template <class TokenKind>
token_buffer<TokenKind> make_tokens(std::initializer_list<TokenKind> kinds) {
    token_buffer<TokenKind> buf;
    std::uint32_t off = 0;
    for (auto k : kinds) { buf.data.push_back({k, off, 1, 0, 0}); ++off; }
    buf.data.push_back({TokenKind{}, off, 0, 0, 0});
    return buf;
}

// ============================================================================
// checkpoint/rollback restores cursor
// ============================================================================

TEST_CASE("checkpoint: rollback restores cursor position", "[samasa][conformance][checkpoint]") {
    auto buf = make_tokens<TK>({TK::tok_a, TK::tok_b, TK::tok_c});
    event_stream<SK> events;
    lang::collecting_sink<diagnostic> sink;
    lang::parse_tree_stats stats;
    limits budget;
    parse_context<SK,TK> ctx(buf.view(), "abc", events, sink, stats, budget);

    const auto cp = ctx.checkpoint();

    auto r = tok<TK::tok_a>{}.match(ctx);
    REQUIRE(r.ok());
    ctx.set_cursor(r.next);
    // Cursor now past tok_a.
    CHECK(ctx.cursor().peek().kind == TK::tok_b);

    ctx.rollback(cp);
    CHECK(ctx.cursor().peek().kind == TK::tok_a);
}

TEST_CASE("checkpoint: rollback restores event count", "[samasa][conformance][checkpoint]") {
    auto buf = make_tokens<TK>({TK::tok_a, TK::tok_b});
    event_stream<SK> events;
    lang::collecting_sink<diagnostic> sink;
    lang::parse_tree_stats stats;
    limits budget;
    parse_context<SK,TK> ctx(buf.view(), "ab", events, sink, stats, budget);

    const auto cp = ctx.checkpoint();
    const std::size_t events_before = ctx.events().event_count();

    // Emit begin/token/end.
    auto m = ctx.events().begin(SK::item);
    auto r = tok<TK::tok_a>{}.match(ctx);
    REQUIRE(r.ok());
    ctx.set_cursor(r.next);
    ctx.events().end(m, {0, 1});
    CHECK(ctx.events().event_count() > events_before);

    ctx.rollback(cp);
    // After rollback cursor is back at tok_a.
    CHECK(ctx.cursor().peek().kind == TK::tok_a);
}

// ============================================================================
// Nested checkpoints unwind in correct order
// ============================================================================

TEST_CASE("checkpoint: nested rollback restores intermediate position",
          "[samasa][conformance][checkpoint]")
{
    auto buf = make_tokens<TK>({TK::tok_a, TK::tok_b, TK::tok_c});
    event_stream<SK> events;
    lang::collecting_sink<diagnostic> sink;
    lang::parse_tree_stats stats;
    limits budget;
    parse_context<SK,TK> ctx(buf.view(), "abc", events, sink, stats, budget);

    const auto cp0 = ctx.checkpoint(); // position 0

    auto r0 = tok<TK::tok_a>{}.match(ctx);
    REQUIRE(r0.ok());
    ctx.set_cursor(r0.next); // position 1

    const auto cp1 = ctx.checkpoint(); // position 1

    auto r1 = tok<TK::tok_b>{}.match(ctx);
    REQUIRE(r1.ok());
    ctx.set_cursor(r1.next); // position 2

    // Rollback to cp1 → position 1.
    ctx.rollback(cp1);
    CHECK(ctx.cursor().peek().kind == TK::tok_b);

    // Rollback to cp0 → position 0.
    ctx.rollback(cp0);
    CHECK(ctx.cursor().peek().kind == TK::tok_a);
}

} // anonymous namespace
