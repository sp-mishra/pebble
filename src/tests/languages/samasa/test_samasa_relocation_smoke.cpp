// ============================================================================
// test_samasa_relocation_smoke.cpp — Verify canonical include path works.
// ============================================================================

#include "catch_amalgamated.hpp"
#include "languages/samasa/samasa.hpp"  // new canonical path

namespace {

enum class TK : std::uint8_t { eof, ident, kw_x, op_eq, int_lit, str_lit, unknown_ };
enum class SK : std::uint8_t { file };

using KW  = lang::samasa::keyword_table<lang::samasa::keyword<"x", TK::kw_x>>;
using Ops = lang::samasa::operator_trie<lang::samasa::operator_token<"=", TK::op_eq>>;

using file_rule  = lang::samasa::rule<"file",  lang::samasa::many_t<lang::samasa::tok<TK::ident>>>;
using TinyGrammar = lang::samasa::grammar<SK, TK, file_rule, file_rule>;

} // namespace

TEST_CASE("samasa canonical path smoke", "[samasa][relocation]") {
    lang::samasa::scan_token_kinds<TK> tk{ TK::eof, TK::ident, TK::int_lit, TK::str_lit, TK::unknown_ };
    auto out = lang::samasa::parse<TinyGrammar, KW, Ops>("hello world", {}, tk);
    REQUIRE(out.success);
}
