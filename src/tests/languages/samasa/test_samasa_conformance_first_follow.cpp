// ============================================================================
// test_samasa_conformance_first_follow.cpp — FIRST / FOLLOW behavioral contract.
//
// Verifies:
//   - FIRST set computation including nullable propagation.
//   - FOLLOW fixed-point terminates correctly (mutually-recursive grammar fixture).
//   - FIRST/FOLLOW of nullable chains.
//
// FOLLOW fixed-point evidence: follow_sets<G>() completes without divergence and
// the root FOLLOW set includes the EOF sentinel — the mandatory fixed-point property.
// ============================================================================

#include "catch_amalgamated.hpp"
#include "languages/samasa/samasa.hpp"
#include "languages/samasa/grammar/expected_sets.hpp"

namespace {

using namespace lang::samasa;

enum class SK : std::uint8_t { root, a_node, b_node };
enum class TK : std::uint8_t { eof, tok_a, tok_b };

// ============================================================================
// FIRST-set computation
// ============================================================================

TEST_CASE("FIRST: tok<K> → {K}", "[samasa][conformance][first]") {
    constexpr auto f = expected_at<rule<"r", tok<TK::tok_a>>, TK>();
    STATIC_REQUIRE(f.size() == 1);
    STATIC_REQUIRE(f[0] == TK::tok_a);
}

TEST_CASE("FIRST: seq<A,B> non-nullable A → FIRST = {A}", "[samasa][conformance][first]") {
    // tok_a is non-nullable → seq<tok_a, tok_b> FIRST = {tok_a} as the first element.
    // Array capacity is upper-bound (2); only arr[0] is filled.
    constexpr auto f = expected_at<rule<"r", seq_t<tok<TK::tok_a>, tok<TK::tok_b>>>, TK>();
    STATIC_REQUIRE(f.size() >= 1);
    STATIC_REQUIRE(f[0] == TK::tok_a);
}

TEST_CASE("FIRST: choice<A,B> → FIRST(A) ∪ FIRST(B)", "[samasa][conformance][first]") {
    constexpr auto f = expected_at<rule<"r", choice_t<tok<TK::tok_a>, tok<TK::tok_b>>>, TK>();
    STATIC_REQUIRE(f.size() == 2);
    const bool has_a = f[0] == TK::tok_a || f[1] == TK::tok_a;
    const bool has_b = f[0] == TK::tok_b || f[1] == TK::tok_b;
    STATIC_REQUIRE(has_a);
    STATIC_REQUIRE(has_b);
}

TEST_CASE("FIRST: nullable A in seq<A,B> → both A and B in FIRST",
          "[samasa][conformance][first]")
{
    // seq<opt<tok_a>, tok_b>: opt<tok_a> nullable → FIRST = {tok_a, tok_b}
    constexpr auto f = expected_at<
        rule<"r", seq_t<opt_t<tok<TK::tok_a>>, tok<TK::tok_b>>>, TK>();
    STATIC_REQUIRE(f.size() == 2);
    const bool has_a = f[0] == TK::tok_a || f[1] == TK::tok_a;
    const bool has_b = f[0] == TK::tok_b || f[1] == TK::tok_b;
    STATIC_REQUIRE(has_a);
    STATIC_REQUIRE(has_b);
}

// ============================================================================
// FOLLOW fixed-point — mutually-recursive grammar
//   G: root → (tok_a | tok_b)* using grammar<> with rule_count and rules.
// ============================================================================

using r_ab   = rule<"ab",   choice_t<tok<TK::tok_a>, tok<TK::tok_b>>>;
using r_root = rule<"root", many_t<r_ab>>;

// Use the grammar<> helper to get rule_count.
using FollowG = grammar<SK, TK, r_root, r_root, r_ab>;

TEST_CASE("FOLLOW fixed-point: follow_sets completes and root has EOF",
          "[samasa][conformance][follow]")
{
    // Verify fixed-point iteration terminates (no divergence).
    constexpr auto& entries = grammar_follow_sets<FollowG>::entries;
    constexpr std::size_t N = grammar_follow_sets<FollowG>::rule_count;

    // Root rule FOLLOW must contain EOF sentinel (TK{} == TK::eof).
    bool root_has_eof = false;
    for (std::size_t i = 0; i < N; ++i) {
        if (entries[i].name == r_root::name_sv) {
            // has_eof flag set by follow_sets for root.
            root_has_eof = entries[i].has_eof;
            // Also check token array.
            for (std::size_t j = 0; j < entries[i].token_count; ++j) {
                if (entries[i].tokens[j] == TK::eof) { root_has_eof = true; break; }
            }
            root_has_eof = root_has_eof || entries[i].has_eof;
        }
    }
    CHECK(root_has_eof);
}

TEST_CASE("FOLLOW fixed-point: FOLLOW(ab) contains tok_a and tok_b (loop body follows itself)",
          "[samasa][conformance][follow]")
{
    constexpr auto& entries = grammar_follow_sets<FollowG>::entries;
    constexpr std::size_t N = grammar_follow_sets<FollowG>::rule_count;

    bool ab_has_a = false, ab_has_b = false;
    for (std::size_t i = 0; i < N; ++i) {
        if (entries[i].name == r_ab::name_sv) {
            for (std::size_t j = 0; j < entries[i].token_count; ++j) {
                if (entries[i].tokens[j] == TK::tok_a) ab_has_a = true;
                if (entries[i].tokens[j] == TK::tok_b) ab_has_b = true;
            }
        }
    }
    // In many<ab>, each iteration of ab is followed by another potential ab → tok_a|tok_b in FOLLOW.
    CHECK(ab_has_a);
    CHECK(ab_has_b);
}

// ============================================================================
// Nullable chain FIRST
// ============================================================================

TEST_CASE("FIRST: seq<opt<tok_a>, opt<tok_b>> → both tok_a and tok_b in FIRST",
          "[samasa][conformance][first]")
{
    constexpr auto f = expected_at<
        rule<"r", seq_t<opt_t<tok<TK::tok_a>>, opt_t<tok<TK::tok_b>>>>, TK>();
    bool has_a = false, has_b = false;
    for (std::size_t i = 0; i < f.size(); ++i) {
        if (f[i] == TK::tok_a) has_a = true;
        if (f[i] == TK::tok_b) has_b = true;
    }
    CHECK(has_a);
    CHECK(has_b);
}

} // anonymous namespace
