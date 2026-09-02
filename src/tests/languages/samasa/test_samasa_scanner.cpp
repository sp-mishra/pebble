// ============================================================================
// test_samasa_scanner.cpp — Scanner: keyword/ident, operator longest-match,
//   trivia retention, unterminated string diag, unknown char diag.
// ============================================================================

#include "catch_amalgamated.hpp"
#include "languages/samasa/samasa.hpp"

namespace {
    enum class TK : std::uint8_t {
        eof,
        ident,
        kw_let,
        op_eq,
        op_eqeq,
        op_arrow,
        int_lit,
        str_lit,
        unknown_,
    };

    using MyKW = lang::samasa::keyword_table<
        lang::samasa::keyword<"let", TK::kw_let>
    >;

    using MyOps = lang::samasa::operator_trie<
        lang::samasa::operator_token < "=>", TK::op_arrow>
    ,
    lang::samasa::operator_token<"==", TK::op_eqeq>
    ,
    lang::samasa::operator_token<"=", TK::op_eq>
    >;

    using lang::samasa::no_line_sensitivity;
    using lang::samasa::scan;
    using lang::samasa::scan_token_kinds;

    constexpr scan_token_kinds<TK> kinds{
        TK::eof, TK::ident, TK::int_lit, TK::eof /*float — unused*/, TK::str_lit, TK::unknown_
    };

    auto do_scan(std::string_view src) {
        lang::collecting_sink<lang::samasa::diagnostic> sink;
        no_line_sensitivity<TK> lp;
        auto buf = scan<MyKW, MyOps, no_line_sensitivity<TK>, TK>(src, kinds, lp, sink);
        return std::make_pair(std::move(buf), std::move(sink));
    }
} // anonymous namespace

// ============================================================================

TEST_CASE (
"scanner: identifier vs keyword"
,
"[samasa][scanner]"
)
 {
    auto [buf, sink] = do_scan("let foo");
    REQUIRE(sink.entries.empty());
    // tokens: kw_let, ident, eof
    REQUIRE(buf.data.size() == 3);
    CHECK(buf.data[0].kind == TK::kw_let);
    CHECK(buf.data[1].kind == TK::ident);
    CHECK(buf.data[2].kind == TK::eof);
}

TEST_CASE (
"scanner: identifier text span correct"
,
"[samasa][scanner]"
)
 {
    std::string_view src = "myVar";
    auto [buf, sink] = do_scan(src);
    REQUIRE(buf.data.size() >= 2);
    const auto& tok = buf.data[0];
    CHECK(tok.kind == TK::ident);
    CHECK(tok.offset == 0);
    CHECK(tok.length == 5);
    CHECK(src.substr(tok.offset, tok.length) == "myVar");
}

TEST_CASE (
"scanner: operator longest-match — => vs == vs ="
,
"[samasa][scanner]"
)
 {
    auto [buf, _] = do_scan("=> == =");
    REQUIRE(buf.data.size() == 4); // op_arrow, op_eqeq, op_eq, eof
    CHECK(buf.data[0].kind == TK::op_arrow);
    CHECK(buf.data[0].length == 2);
    CHECK(buf.data[1].kind == TK::op_eqeq);
    CHECK(buf.data[1].length == 2);
    CHECK(buf.data[2].kind == TK::op_eq);
    CHECK(buf.data[2].length == 1);
    CHECK(buf.data[3].kind == TK::eof);
}

TEST_CASE (
"scanner: integer literal"
,
"[samasa][scanner]"
)
 {
    auto [buf, sink] = do_scan("42");
    REQUIRE(sink.entries.empty());
    REQUIRE(buf.data.size() == 2);
    CHECK(buf.data[0].kind == TK::int_lit);
    CHECK(buf.data[0].length == 2);
}

TEST_CASE (
"scanner: trivia arena populated for whitespace"
,
"[samasa][scanner]"
)
 {
    auto [buf, sink] = do_scan("a  b");
    // trivia arena must have at least one whitespace entry
    REQUIRE(!buf.trivia_arena.empty());
    const auto& tv = buf.trivia_arena[0];
    CHECK(tv.kind == lang::samasa::trivia_kind::whitespace);
}

TEST_CASE (
"scanner: unterminated string emits diagnostic"
,
"[samasa][scanner]"
)
 {
    auto [buf, sink] = do_scan("\"hello");
    REQUIRE(sink.has_errors());
    bool found = false;
    for (const auto& d : sink.entries)
        if (d.kind == lang::samasa::samasa_diag_code::lex_unterminated_string)
            found = true;
    CHECK(found);
    // token is still emitted as str_lit
    REQUIRE(!buf.data.empty());
    CHECK(buf.data[0].kind == TK::str_lit);
}

TEST_CASE (
"scanner: unknown character emits diagnostic"
,
"[samasa][scanner]"
)
 {
    auto [buf, sink] = do_scan("@");
    REQUIRE(sink.has_errors());
    bool found = false;
    for (const auto& d : sink.entries)
        if (d.kind == lang::samasa::samasa_diag_code::lex_unknown_char)
            found = true;
    CHECK(found);
    CHECK(buf.data[0].kind == TK::unknown_);
}

TEST_CASE (
"scanner: line comment is trivia, not token"
,
"[samasa][scanner]"
)
 {
    auto [buf, sink] = do_scan("a // this is a comment\nb");
    REQUIRE(sink.entries.empty());
    // tokens: ident(a), ident(b), eof
    REQUIRE(buf.data.size() == 3);
    CHECK(buf.data[0].kind == TK::ident);
    CHECK(buf.data[1].kind == TK::ident);
    // trivia arena must contain a line_comment entry
    bool has_comment = false;
    for (const auto& tv : buf.trivia_arena)
        if (tv.kind == lang::samasa::trivia_kind::line_comment)
            has_comment = true;
    CHECK(has_comment);
}

TEST_CASE (
"scanner: eof token at end"
,
"[samasa][scanner]"
)
 {
    auto [buf, sink] = do_scan("");
    REQUIRE(buf.data.size() == 1);
    CHECK(buf.data[0].kind == TK::eof);
    CHECK(buf.data[0].offset == 0);
}

// ============================================================================
// New tests [R4]: token layout invariants
// ============================================================================

TEST_CASE (
"token: trivially copyable for uint32_t kind"
,
"[samasa][scanner]"
)
 {
    STATIC_REQUIRE(std::is_trivially_copyable_v<lang::samasa::token<std::uint32_t>>);
}

TEST_CASE (
"token: trivially copyable for TK enum kind"
,
"[samasa][scanner]"
)
 {
    STATIC_REQUIRE(std::is_trivially_copyable_v<lang::samasa::token<TK>>);
}

TEST_CASE (
"token: uint32_t length supports values > 65535 (no overflow)"
,
"[samasa][scanner]"
)
 {
    // length is uint32_t — must hold values well above the old uint16_t max.
    lang::samasa::token<TK> t{};
    t.length = 100000u;
    CHECK(t.length == 100000u);
    STATIC_REQUIRE(std::numeric_limits<std::uint32_t>::max() > 65535u);
}

// ============================================================================
// New tests [R13]: scanner_policy hook
// ============================================================================

// Custom policy: recognises '#' as a special hash_token.
enum class HTK : std::uint8_t { eof, ident, int_lit, str_lit, unknown_, hash };

struct HashPolicy : lang::samasa::scanner_policy<HTK> {
    template <class ScannerView>
    static bool scan_custom_token(ScannerView& sv) {
        if (sv.pos < sv.source.size() && sv.source[sv.pos] == '#') {
            const auto start = sv.pos;
            ++sv.pos;
            sv.emit_tok(HTK::hash, start, 1);
            return true;
        }
        return false;
    }
};

TEST_CASE (
"scanner_policy: default policy produces same results as no-policy overload"
,
"[samasa][scanner]"
)
 {
    // Scanning "let foo" with explicit default policy must match the standard path.
    using namespace lang::samasa;
    lang::collecting_sink<diagnostic> sink1, sink2;
    no_line_sensitivity<TK> lp;
    constexpr scan_token_kinds<TK> k{TK::eof, TK::ident, TK::int_lit, TK::eof, TK::str_lit, TK::unknown_};
    auto buf1 = scan<MyKW, MyOps, no_line_sensitivity<TK>, TK>(      "let foo", k, lp, sink1);
    auto buf2 = scan<MyKW, MyOps, no_line_sensitivity<TK>, TK, scanner_policy<TK>>("let foo", k, lp, sink2);
    REQUIRE(buf1.data.size() == buf2.data.size());
    for (std::size_t i = 0; i < buf1.data.size(); ++i)
        CHECK(buf1.data[i].kind == buf2.data[i].kind);
}


// ============================================================================
// New tests [design.md]: block comment, newline_terminates line policy
// ============================================================================

TEST_CASE (
"scanner: block comment is trivia, not token"
,
"[samasa][scanner]"
)
 {
    auto [buf, sink] = do_scan("a /* block */ b");
    REQUIRE(sink.entries.empty());
    // tokens: ident(a), ident(b), eof
    REQUIRE(buf.data.size() == 3);
    CHECK(buf.data[0].kind == TK::ident);
    CHECK(buf.data[1].kind == TK::ident);
    // trivia arena must contain a block_comment entry
    bool has_block = false;
    for (const auto& tv : buf.trivia_arena)
        if (tv.kind == lang::samasa::trivia_kind::block_comment)
            has_block = true;
    CHECK(has_block);
}

TEST_CASE (
"scanner: unterminated block comment emits diagnostic"
,
"[samasa][scanner]"
)
 {
    auto [buf, sink] = do_scan("/* unterminated");
    REQUIRE(sink.has_errors());
    bool found = false;
    for (const auto& d : sink.entries)
        if (d.kind == lang::samasa::samasa_diag_code::lex_unterminated_comment)
            found = true;
    CHECK(found);
}

TEST_CASE (
"scanner: newline_terminates — newline after ident emits separator token"
,
"[samasa][scanner]"
)
 {
    // Use enum with a newline_sep token and specialise statement_ending.
    using namespace lang::samasa;
    enum class LTK : std::uint8_t { eof, ident, newline_sep, unknown_ };

    // statement_ending: ident ends a statement
    struct LEndingPolicy : newline_terminates<LTK, LTK::newline_sep> {};

    constexpr scan_token_kinds<LTK> k{
        LTK::eof, LTK::ident, LTK::eof /*int*/, LTK::eof /*float*/, LTK::eof /*str*/, LTK::unknown_
    };
    lang::collecting_sink<diagnostic> sink;

    // Specialise statement_ending for LTK to treat ident as statement-ending.
    struct LP {
        constexpr bool line_continues(LTK prev) const noexcept {
            return prev != LTK::ident; // ident ends statement
        }
        constexpr bool suppress_separator(LTK, LTK) const noexcept { return false; }
        constexpr LTK  synthetic_separator() const noexcept { return LTK::newline_sep; }
    };

    auto buf = scan<keyword_table<>, operator_trie<>, LP, LTK>("a\nb", k, LP{}, sink);

    // Expected: ident(a), newline_sep (synthetic), ident(b), eof
    REQUIRE(buf.data.size() >= 4);
    CHECK(buf.data[0].kind == LTK::ident);
    CHECK(buf.data[1].kind == LTK::newline_sep);
    CHECK(buf.data[2].kind == LTK::ident);
    CHECK(buf.data[3].kind == LTK::eof);
}

