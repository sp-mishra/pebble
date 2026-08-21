#include "catch_amalgamated.hpp"

#include "languages/samasa/samasa.hpp"

namespace {
enum class tk : std::uint8_t { eof, alpha, beta, ident, integer, floating, string, unknown };
enum class sk : std::uint8_t { root };
using choice_rule = lang::samasa::rule<"root", lang::samasa::choice_t<
    lang::samasa::tok<tk::alpha>, lang::samasa::tok<tk::beta>>>;
using choice_grammar = lang::samasa::grammar<sk, tk, choice_rule, choice_rule>;
constexpr lang::samasa::scan_token_kinds<tk> kinds{tk::eof,tk::ident,tk::integer,tk::floating,tk::string,tk::unknown};
using words = lang::samasa::keyword_table<lang::samasa::keyword<"alpha",tk::alpha>,lang::samasa::keyword<"beta",tk::beta>>;
}

TEST_CASE("samasa lightweight: cached metadata and predicted token choice", "[samasa][lightweight]") {
    STATIC_REQUIRE(lang::samasa::grammar_metadata<choice_grammar>::valid);
    auto out = lang::samasa::parse<choice_grammar, words>("beta", {}, kinds);
    REQUIRE(out.success);
}

TEST_CASE("samasa lightweight: direct event parse omits CST materialization", "[samasa][lightweight]") {
    std::size_t events = 0;
    auto sink = [&](const auto&) { ++events; };
    REQUIRE(lang::samasa::parse_events<choice_grammar, decltype(sink)&, words>(
        "alpha", sink, {}, kinds));
    REQUIRE(events >= 3); // synthetic root begin, token, root end
}

TEST_CASE("samasa lightweight: dense memo has no allocation path", "[samasa][lightweight]") {
    lang::samasa::dense_memo<2, 4> memo;
    lang::samasa::memo_value value{}; value.status = lang::samasa::parse_status::success; value.next_pos = 2; value.valid = true;
    memo.store({17, 1}, value);
    lang::samasa::memo_value cached{};
    REQUIRE(memo.lookup({17, 1}, cached));
    REQUIRE(cached.next_pos == 2);
}
