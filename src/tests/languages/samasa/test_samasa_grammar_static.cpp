// ============================================================================
// test_samasa_grammar_static.cpp — grammar_valid<G>() static assertion.
// ============================================================================

#include "catch_amalgamated.hpp"
#include "languages/samasa/samasa.hpp"

namespace {

// ---- Minimal syntax/token kinds -------------------------------------------

enum class MySK : std::uint8_t { root, expr };
enum class MyTK : std::uint8_t { eof, ident, kw_let, op_eq };

// ---- Rules -----------------------------------------------------------------

using namespace lang::samasa;

// root_rule = tok<kw_let> tok<ident> tok<op_eq> tok<ident>
using root_pattern = seq_t<tok<MyTK::kw_let>, tok<MyTK::ident>,
                           tok<MyTK::op_eq>,  tok<MyTK::ident>>;

using root_rule = rule<"root", root_pattern>;

// ---- Grammar ---------------------------------------------------------------

using MyGrammar = grammar<MySK, MyTK, root_rule, root_rule>;

} // anonymous namespace

// ============================================================================

TEST_CASE("grammar_valid: valid grammar passes static check", "[samasa][grammar]") {
    // The grammar has no many<nullable> patterns — should be valid.
    STATIC_REQUIRE(grammar_valid<MyGrammar>());
}

TEST_CASE("grammar: rule_count correct", "[samasa][grammar]") {
    STATIC_REQUIRE(MyGrammar::rule_count == 1);
}

TEST_CASE("grammar: syntax_kind / token_kind aliases", "[samasa][grammar]") {
    STATIC_REQUIRE(std::is_same_v<MyGrammar::syntax_kind, MySK>);
    STATIC_REQUIRE(std::is_same_v<MyGrammar::token_kind,  MyTK>);
}

TEST_CASE("grammar: root_rule type matches", "[samasa][grammar]") {
    STATIC_REQUIRE(std::is_same_v<MyGrammar::root_rule, root_rule>);
}

TEST_CASE("grammar: rules TypeList has exactly one type", "[samasa][grammar]") {
    STATIC_REQUIRE(meta::TypeList<root_rule>::size == 1);
}

TEST_CASE("grammar: seq of non-nullable matchers is not empty_many", "[samasa][grammar]") {
    // Compile-time check: has_empty_many for our root pattern is false.
    using namespace lang::samasa::detail;
    STATIC_REQUIRE(!has_empty_many<root_pattern>::value);
}

// ============================================================================
// New tests [R8]: structured grammar validation — validate_grammar<G>() result
// ============================================================================

namespace {

// Grammar with many<opt<...>> — opt is nullable → empty_many.
using empty_many_pattern = lang::samasa::many_t<lang::samasa::opt_t<lang::samasa::tok<MyTK::ident>>>;
using root_with_empty_many  = lang::samasa::rule<"root_em", empty_many_pattern>;
using BadManyGrammar = lang::samasa::grammar<MySK, MyTK, root_with_empty_many, root_with_empty_many>;

// Grammar with two rules sharing the same name.
using dup_rule_a = lang::samasa::rule<"dup", lang::samasa::tok<MyTK::ident>>;
using dup_rule_b = lang::samasa::rule<"dup", lang::samasa::tok<MyTK::kw_let>>;
using BadDupGrammar = lang::samasa::grammar<MySK, MyTK, dup_rule_a, dup_rule_a, dup_rule_b>;

} // anonymous namespace

TEST_CASE("grammar: validate_grammar detects empty_many", "[samasa][grammar]") {
    constexpr auto result = lang::samasa::validate_grammar<BadManyGrammar>();
    STATIC_REQUIRE(!result.ok());
    STATIC_REQUIRE(result.issues[0].code == lang::samasa::grammar_diag_code::empty_many);
}

TEST_CASE("grammar: validate_grammar detects duplicate_rule", "[samasa][grammar]") {
    constexpr auto result = lang::samasa::validate_grammar<BadDupGrammar>();
    STATIC_REQUIRE(!result.ok());
    STATIC_REQUIRE(result.issues[0].code == lang::samasa::grammar_diag_code::duplicate_rule);
}

TEST_CASE("grammar: validate_grammar returns ok for valid grammar", "[samasa][grammar]") {
    constexpr auto result = lang::samasa::validate_grammar<MyGrammar>();
    STATIC_REQUIRE(result.ok());
}

TEST_CASE("grammar: grammar_diag_code enum has 7 values", "[samasa][grammar]") {
    using GDC = lang::samasa::grammar_diag_code;
    STATIC_REQUIRE(static_cast<std::uint8_t>(GDC::bad_pratt_table) == 6);
}

// ============================================================================
// New tests [design.md §6]: require_valid_grammar, operator_table_valid,
//   nullable_v, grammar_fingerprint
// ============================================================================

TEST_CASE("grammar: require_valid_grammar compiles for valid grammar (no-op)", "[samasa][grammar]") {
    // require_valid_grammar<G>() is a consteval void that static_asserts on invalid grammars.
    // For a valid grammar it must compile and do nothing at runtime.
    lang::samasa::require_valid_grammar<MyGrammar>();
    SUCCEED("require_valid_grammar compiled and ran for valid grammar");
}

namespace {

// Valid operator table — two distinct spellings.
using ValidOps = lang::samasa::operator_table<
    lang::samasa::op<"+", MyTK::op_eq, 10, lang::samasa::associativity::left,  lang::samasa::fixity::infix>,
    lang::samasa::op<"=", MyTK::ident,  5, lang::samasa::associativity::none,  lang::samasa::fixity::infix>
>;

// Duplicate operator table — two entries with spelling "+".
using DupOps = lang::samasa::operator_table<
    lang::samasa::op<"+", MyTK::op_eq, 10, lang::samasa::associativity::left,  lang::samasa::fixity::infix>,
    lang::samasa::op<"+", MyTK::ident,  5, lang::samasa::associativity::right, lang::samasa::fixity::infix>
>;

} // anonymous namespace

TEST_CASE("grammar: operator_table_valid — valid table returns true", "[samasa][grammar]") {
    // public API: static_asserts on duplicate; returns true for valid table.
    STATIC_REQUIRE(lang::samasa::operator_table_valid<ValidOps>());
}

TEST_CASE("grammar: operator_table_valid_check — duplicate ops detected via detail trait", "[samasa][grammar]") {
    // operator_table_valid<DupOps>() would fire a static_assert.
    // Use the detail trait directly to observe the false value without triggering the assert.
    STATIC_REQUIRE(!lang::samasa::detail::operator_table_valid_check<DupOps>::value);
}

TEST_CASE("grammar: nullable_v — tok<K> is not nullable", "[samasa][grammar]") {
    STATIC_REQUIRE(!lang::samasa::nullable_v<lang::samasa::tok<MyTK::ident>>);
}

TEST_CASE("grammar: nullable_v — opt<P> is nullable", "[samasa][grammar]") {
    STATIC_REQUIRE(lang::samasa::nullable_v<lang::samasa::opt_t<lang::samasa::tok<MyTK::ident>>>);
}

TEST_CASE("grammar: nullable_v — many<P> is nullable (zero repetitions allowed)", "[samasa][grammar]") {
    STATIC_REQUIRE(lang::samasa::nullable_v<lang::samasa::many_t<lang::samasa::tok<MyTK::ident>>>);
}

TEST_CASE("grammar: nullable_v — seq of non-nullable is not nullable", "[samasa][grammar]") {
    using S = lang::samasa::seq_t<lang::samasa::tok<MyTK::ident>, lang::samasa::tok<MyTK::kw_let>>;
    STATIC_REQUIRE(!lang::samasa::nullable_v<S>);
}

TEST_CASE("grammar: grammar_fingerprint is nonzero for valid grammar", "[samasa][grammar]") {
    constexpr lang::descriptor_fingerprint fp = lang::samasa::grammar_fingerprint<MyGrammar>();
    STATIC_REQUIRE(fp != 0u);
}

TEST_CASE("grammar: grammar_fingerprint is deterministic — two calls return same value", "[samasa][grammar]") {
    constexpr auto fp1 = lang::samasa::grammar_fingerprint<MyGrammar>();
    constexpr auto fp2 = lang::samasa::grammar_fingerprint<MyGrammar>();
    STATIC_REQUIRE(fp1 == fp2);
}

TEST_CASE("grammar: grammar_fingerprint differs for grammars with different rule names", "[samasa][grammar]") {
    // BadManyGrammar has a rule named "root_em"; MyGrammar has "root" — fingerprints must differ.
    constexpr auto fp_good = lang::samasa::grammar_fingerprint<MyGrammar>();
    constexpr auto fp_bad  = lang::samasa::grammar_fingerprint<BadManyGrammar>();
    STATIC_REQUIRE(fp_good != fp_bad);
}
