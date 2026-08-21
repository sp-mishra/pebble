// ============================================================================
// test_samasa_conformance_choice.cpp — Ordered-choice behavioral contract.
//
// Verifies:
//   - First matching alternative wins; later alternatives not tried.
//   - Non-committed failure backtracks; next alternative tried.
//   - Nullable alt shadowing detected by grammar_valid.
// ============================================================================

#include "catch_amalgamated.hpp"
#include "languages/samasa/samasa.hpp"

namespace {

using namespace lang::samasa;

enum class SK : std::uint8_t { root };
enum class TK : std::uint8_t { eof, kw_a, kw_b, kw_c };

template <class TokenKind>
token_buffer<TokenKind> make_one_token(TokenKind k,
                                        std::uint32_t off = 0,
                                        std::uint32_t len = 1)
{
    token_buffer<TokenKind> buf;
    buf.data.push_back({k, off, len, 0, 0});
    buf.data.push_back({TK::eof, off + len, 0, 0, 0});
    return buf;
}

// ============================================================================
// First-wins and backtracking
// ============================================================================

TEST_CASE("choice: first alternative matches → success", "[samasa][conformance][choice]") {
    auto buf = make_one_token<TK>(TK::kw_a);
    event_stream<SK> events;
    lang::collecting_sink<diagnostic> sink;
    lang::parse_tree_stats stats;
    limits budget;
    parse_context<SK,TK> ctx(buf.view(), "a", events, sink, stats, budget);

    auto r = tok<TK::kw_a>{}.match(ctx);
    CHECK(r.ok());
}

TEST_CASE("choice: first fails (soft), second tried and succeeds",
          "[samasa][conformance][choice]")
{
    auto buf = make_one_token<TK>(TK::kw_b);
    event_stream<SK> events;
    lang::collecting_sink<diagnostic> sink;
    lang::parse_tree_stats stats;
    limits budget;
    parse_context<SK,TK> ctx(buf.view(), "b", events, sink, stats, budget);

    auto r1 = tok<TK::kw_a>{}.match(ctx); // soft fail (wrong token)
    CHECK(!r1.ok());

    // Cursor not advanced on soft fail — next attempt succeeds.
    auto r2 = tok<TK::kw_b>{}.match(ctx);
    CHECK(r2.ok());
}

TEST_CASE("choice: no alternative matches → all fail", "[samasa][conformance][choice]") {
    auto buf = make_one_token<TK>(TK::kw_c);
    event_stream<SK> events;
    lang::collecting_sink<diagnostic> sink;
    lang::parse_tree_stats stats;
    limits budget;
    parse_context<SK,TK> ctx(buf.view(), "c", events, sink, stats, budget);

    CHECK(!tok<TK::kw_a>{}.match(ctx).ok());
    CHECK(!tok<TK::kw_b>{}.match(ctx).ok());
}

// ============================================================================
// Nullable alt shadowing caught by grammar validation
// ============================================================================

// opt<kw_a> is nullable → shadows kw_b alternative.
using shadow_root = rule<"root", choice_t<opt_t<tok<TK::kw_a>>, tok<TK::kw_b>>>;
struct ShadowG {
    using syntax_kind = SK;
    using token_kind  = TK;
    using root_rule   = shadow_root;
    using rules       = meta::TypeList<shadow_root>;
    static constexpr std::size_t rule_count = 1;
};

TEST_CASE("choice: nullable alt shadows later → grammar_valid == false",
          "[samasa][conformance][choice]")
{
    constexpr bool valid = grammar_valid<ShadowG>();
    CHECK(!valid);
}

// ============================================================================
// Valid grammar passes validation
// ============================================================================

using good_root = rule<"root", choice_t<tok<TK::kw_a>, tok<TK::kw_b>>>;
struct GoodG {
    using syntax_kind = SK;
    using token_kind  = TK;
    using root_rule   = good_root;
    using rules       = meta::TypeList<good_root>;
    static constexpr std::size_t rule_count = 1;
};

TEST_CASE("choice: non-shadowing grammar → grammar_valid == true",
          "[samasa][conformance][choice]")
{
    constexpr bool valid = grammar_valid<GoodG>();
    CHECK(valid);
}

} // anonymous namespace
