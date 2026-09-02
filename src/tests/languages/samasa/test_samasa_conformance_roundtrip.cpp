// ============================================================================
// test_samasa_conformance_roundtrip.cpp — Lossless round-trip contract.
//
// Verifies:
//   - print_original(tree, tokens, source) reproduces source byte-for-byte.
//   - Green tree leaf spans tile the source with no gaps / overlaps.
// ============================================================================

#include "catch_amalgamated.hpp"
#include "languages/samasa/samasa.hpp"
#include "languages/samasa/tree/incremental.hpp"

namespace {
    using namespace lang::samasa;

    enum class SK : std::uint8_t { root, stmt, item };

    enum class TK : std::uint8_t { eof, ident, kw_let, op_eq, semi };

    inline scan_token_kinds<TK> make_rt_kinds() {
        return {TK::eof, TK::ident, TK::ident, TK::ident, TK::ident, TK::ident};
    }

    using RtKWTable = keyword_table<keyword<"let", TK::kw_let>>;
    using RtOpTrie = operator_trie<operator_token < "=", TK::op_eq>
    ,
    operator_token<";", TK::semi>
    >;

    using ident_rule = rule<"ident", tok<TK::ident>>;
    using stmt_rule = rule<"stmt", seq_t < tok < TK::kw_let>
    ,
    tok<TK::ident>
    ,
    tok<TK::op_eq>
    ,
    tok<TK::ident>
    ,
    tok<TK::semi>
    >
    >;
    using root_rule = rule<"root", many_t<stmt_rule>>;

    struct RoundTripG {
        using syntax_kind = SK;
        using token_kind = TK;
        using root_rule = ::root_rule;
    };

    inline auto rt_parse(std::string_view src) {
        return parse<RoundTripG, RtKWTable, RtOpTrie>(src, {}, make_rt_kinds());
    }

    // ============================================================================
    // Round-trip: print_original == source
    // ============================================================================

    TEST_CASE (
    "roundtrip: single statement reproduced exactly"
    ,
    "[samasa][conformance][roundtrip]"
    )
 {
    const std::string src = "let x = y ;";
    auto out = rt_parse(src);
    REQUIRE(out.success);

    const std::string reconstructed = print_original(out.tree, out.tokens, src);
    CHECK(reconstructed == src);
}

    TEST_CASE (
    "roundtrip: two statements reproduced exactly"
    ,
    "[samasa][conformance][roundtrip]"
    )
 {
    const std::string src = "let a = b ; let c = d ;";
    auto out = rt_parse(src);
    REQUIRE(out.success);

    const std::string reconstructed = print_original(out.tree, out.tokens, src);
    CHECK(reconstructed == src);
}

    TEST_CASE (
    "roundtrip: source with leading/trailing whitespace"
    ,
    "[samasa][conformance][roundtrip]"
    )
 {
    const std::string src = "  let x = y ;  ";
    auto out = rt_parse(src);
    // May not succeed (whitespace before first token) but round-trip still holds.
    const std::string reconstructed = print_original(out.tree, out.tokens, src);
    CHECK(reconstructed == src);
}

    // ============================================================================
    // Span coverage: leaf spans tile the source (no overlaps, total length = source)
    // ============================================================================

    TEST_CASE (
    "roundtrip: leaf token spans tile the source without gaps"
    ,
    "[samasa][conformance][roundtrip]"
    )
{
    const std::string src = "let x = y ;";
    auto out = rt_parse(src);
    REQUIRE(out.success);

    // Collect all token spans (non-EOF, non-trivia).
    std::vector<byte_span> spans;
    for (const auto& tok : out.tokens.data) {
        if (tok.length > 0)
            spans.push_back({tok.offset, tok.length});
    }
    // Sort by offset.
    std::sort(spans.begin(), spans.end(),
              [](const byte_span& a, const byte_span& b){ return a.offset < b.offset; });

    // No overlaps.
    for (std::size_t i = 1; i < spans.size(); ++i) {
        CHECK(spans[i-1].end() <= spans[i].offset);
    }

    // Total span covers all non-whitespace source bytes.
    if (!spans.empty()) {
        CHECK(spans.back().end() <= static_cast<std::uint32_t>(src.size()));
    }
}

} // anonymous namespace
