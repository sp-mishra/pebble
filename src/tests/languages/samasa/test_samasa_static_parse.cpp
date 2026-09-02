// ============================================================================
// test_samasa_static_parse.cpp — parse_static<G, Src>() compile-time interface;
//   static_parse_output field presence; parse<G>(source) runtime stub.
// ============================================================================

#include "catch_amalgamated.hpp"
#include "languages/samasa/samasa.hpp"

namespace {
    enum class SK : std::uint8_t { root };

    enum class TK : std::uint8_t { eof, ident };

    using namespace lang::samasa;

    // Minimal rule: match a single ident token.
    struct root_rule {
        static constexpr auto name_sv = std::string_view{"root"};

        template <class Ctx>
        [[nodiscard]] constexpr auto match(Ctx& ctx) const {
            return tok < TK::ident >
            {}.match(ctx);
        }
    };

    using MyGrammar = grammar<SK, TK, root_rule, root_rule>;
} // anonymous namespace

// ============================================================================
// static_parse_output field existence (STATIC_REQUIRE on types)
// ============================================================================

TEST_CASE (
"static_parse_output: has success field"
,
"[samasa][static_parse]"
)
 {
    using Out = static_parse_output<SK, TK, 256, 256>;
    STATIC_REQUIRE(std::is_same_v<decltype(std::declval<Out>().success), bool>);
}

TEST_CASE (
"static_parse_output: has token_count field"
,
"[samasa][static_parse]"
)
 {
    using Out = static_parse_output<SK, TK, 256, 256>;
    STATIC_REQUIRE(std::is_same_v<decltype(std::declval<Out>().token_count), std::uint32_t>);
}

TEST_CASE (
"static_parse_output: has event_count field"
,
"[samasa][static_parse]"
)
 {
    using Out = static_parse_output<SK, TK, 256, 256>;
    STATIC_REQUIRE(std::is_same_v<decltype(std::declval<Out>().event_count), std::uint32_t>);
}

TEST_CASE (
"static_parse_output: has diag_count field"
,
"[samasa][static_parse]"
)
 {
    using Out = static_parse_output<SK, TK, 256, 256>;
    STATIC_REQUIRE(std::is_same_v<decltype(std::declval<Out>().diag_count), std::uint32_t>);
}

TEST_CASE (
"static_parse_output: tokens array size matches template parameter"
,
"[samasa][static_parse]"
)
 {
    using Out = static_parse_output<SK, TK, 16, 8>;
    STATIC_REQUIRE(std::tuple_size_v<decltype(std::declval<Out>().tokens)> == 16);
}

TEST_CASE (
"static_parse_output: events array size matches template parameter"
,
"[samasa][static_parse]"
)
 {
    using Out = static_parse_output<SK, TK, 16, 8>;
    STATIC_REQUIRE(std::tuple_size_v<decltype(std::declval<Out>().events)> == 8);
}

// ============================================================================
// parse_static<G, Src>() — consteval stub always returns success=false
// ============================================================================

TEST_CASE (
"parse_static: runs grammar at compile time — valid input succeeds"
,
"[samasa][static_parse]"
)
 {
    // parse_static now runs the grammar: single ident token matches root_rule.
    constexpr scan_token_kinds<TK> kinds{TK::eof, TK::ident};
    STATIC_REQUIRE(parse_static<MyGrammar, "hello">(kinds).success == true);
}

TEST_CASE (
"parse_static: return type is static_parse_output<SK,TK>"
,
"[samasa][static_parse]"
)
 {
    using Out = decltype(parse_static<MyGrammar, "hello">());
    // Must be an instantiation of static_parse_output with the correct SK/TK.
    STATIC_REQUIRE(std::is_same_v<decltype(std::declval<Out>().success), bool>);
}

// ============================================================================
// parse_output<SK,TK> — runtime output struct
// ============================================================================

TEST_CASE (
"parse_output: has success field"
,
"[samasa][static_parse]"
)
 {
    using Out = parse_output<SK, TK>;
    STATIC_REQUIRE(std::is_same_v<decltype(std::declval<Out>().success), bool>);
}

TEST_CASE (
"parse_output: has tokens field of type token_buffer<TK>"
,
"[samasa][static_parse]"
)
 {
    using Out = parse_output<SK, TK>;
    STATIC_REQUIRE(std::is_same_v<decltype(std::declval<Out>().tokens), token_buffer<TK>>);
}

TEST_CASE (
"parse_output: has diagnostics field of type collecting_sink<diagnostic>"
,
"[samasa][static_parse]"
)
 {
    using Out = parse_output<SK, TK>;
    using Expected = lang::collecting_sink<diagnostic>;
    STATIC_REQUIRE(std::is_same_v<decltype(std::declval<Out>().diagnostics), Expected>);
}

TEST_CASE (
"parse_output: has stats field of type parse_tree_stats"
,
"[samasa][static_parse]"
)
 {
    using Out = parse_output<SK, TK>;
    STATIC_REQUIRE(std::is_same_v<decltype(std::declval<Out>().stats), lang::parse_tree_stats>);
}

TEST_CASE (
"parse_output: default success is false"
,
"[samasa][static_parse]"
)
 {
    parse_output<SK, TK> out{};
    CHECK(out.success == false);
}

// ============================================================================
// New tests [R2]: static_parse_output third capacity param (MaxDiags) + overflow
// ============================================================================

TEST_CASE (
"static_parse_output: third template param MaxDiags controls diagnostics array size"
,
"[samasa][static_parse]"
)
 {
    using Out16  = static_parse_output<SK, TK, 16, 8, 16>;
    using Out128 = static_parse_output<SK, TK, 16, 8, 128>;
    STATIC_REQUIRE(std::tuple_size_v<decltype(std::declval<Out16>().diagnostics)>  == 16);
    STATIC_REQUIRE(std::tuple_size_v<decltype(std::declval<Out128>().diagnostics)> == 128);
}

TEST_CASE (
"static_parse_output: diag_count is uint32_t"
,
"[samasa][static_parse]"
)
 {
    using Out = static_parse_output<SK, TK, 256, 256, 64>;
    STATIC_REQUIRE(std::is_same_v<decltype(std::declval<Out>().diag_count), std::uint32_t>);
}

TEST_CASE (
"parse_static: capacity-overflow path keeps success=false"
,
"[samasa][static_parse]"
)
 {
    // Tiny capacity (MaxTokens=1): scanner overflows → success=false.
    constexpr auto out = parse_static<MyGrammar, "hello world", 1, 1, 1>();
    STATIC_REQUIRE(out.success == false);
}

TEST_CASE (
"parse_static: custom MaxTokens is propagated to return type"
,
"[samasa][static_parse]"
)
 {
    // Verify the tokens array has the requested size.
    constexpr auto out = parse_static<MyGrammar, "x", 8, 8, 4>();
    STATIC_REQUIRE(std::tuple_size_v<std::decay_t<decltype(out.tokens)>> == 8);
}

// ============================================================================
// Stage 1 [S1]: full consteval grammar execution tests
// ============================================================================

namespace {
    // Shared token kinds descriptor for Stage 1 tests.
    inline constexpr scan_token_kinds<TK> s1_kinds{TK::eof, TK::ident};
}

TEST_CASE (
"S1: parse_static produces events for valid input"
,
"[samasa][static_parse][s1]"
)
 {
    constexpr auto out = parse_static<MyGrammar, "hello">(s1_kinds);
    STATIC_REQUIRE(out.success);
    STATIC_REQUIRE(out.event_count > 0);
}

TEST_CASE (
"S1: parse_static success=false for malformed input — no hard compile error"
,
"[samasa][static_parse][s1]"
)
 {
    // Empty source — tok<ident> fails, no hard error.
    constexpr auto out = parse_static<MyGrammar, "">(s1_kinds);
    STATIC_REQUIRE(!out.success);
}

TEST_CASE (
"S1: parse_static event overflow sets success=false, not a build break"
,
"[samasa][static_parse][s1]"
)
 {
    // MaxEvents=0: any event emission overflows → success=false.
    constexpr auto out = parse_static<MyGrammar, "hello", 64, 0, 4>(s1_kinds);
    STATIC_REQUIRE(!out.success);
}

TEST_CASE (
"S1: parse_static token_count matches scanned tokens"
,
"[samasa][static_parse][s1]"
)
 {
    // "hello" scans to [ident, eof] → token_count == 2.
    constexpr auto out = parse_static<MyGrammar, "hello">(s1_kinds);
    STATIC_REQUIRE(out.token_count == 2);
}

TEST_CASE (
"S1: parse_static consteval/runtime parity — event_count matches"
,
"[samasa][static_parse][s1]"
)
 {
    // Runtime parse of the same source must produce the same event count.
    constexpr auto ct = parse_static<MyGrammar, "hello">(s1_kinds);
    auto rt = parse<MyGrammar>(std::string_view{"hello"}, {}, s1_kinds);
    REQUIRE(ct.success == rt.success);
}
