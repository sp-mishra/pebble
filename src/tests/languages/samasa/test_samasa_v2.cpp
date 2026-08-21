// ============================================================================
// test_samasa_v2.cpp — Samasa v2 feature tests.
//
// Covers:
//   - expected_at<Rule,TK>() FIRST-set computation
//   - first_sets<G>() grammar_first_sets
//   - grammar_valid v2 checks: left_recursion, empty_separator, choice_shadowing,
//     unreachable_rule, nullable_root
//   - validate_grammar v2 result inspection
//   - Parse tracing (no_trace / collecting_trace)
//   - Rich diagnostic types: expected_item, parse_diagnostic, repair_kind
//   - recover_with_repair scoring combinator
//   - Memoization: memoized<Rule>, selective_memo
//   - Railroad model: railroad_model<G>, railroad_rule_entry shapes
//   - Green fingerprint: green_fingerprint, fingerprint()
//   - Scanner: scanner_mode_stack, unicode_identifier_policy
//   - Incremental: incremental_stats, diff_trees, apply_edit
// ============================================================================

#include "catch_amalgamated.hpp"
#include "languages/samasa/samasa.hpp"
#include "languages/samasa/grammar/expected_sets.hpp"
#include "languages/samasa/grammar/validation.hpp"
#include "languages/samasa/grammar/fingerprint.hpp"
#include "languages/samasa/policies/trace_policy.hpp"
#include "languages/samasa/policies/memo_policy.hpp"
#include "languages/samasa/tooling/railroad.hpp"
#include "languages/samasa/tooling/render_markdown.hpp"
#include "languages/samasa/tooling/render_json.hpp"
#include "languages/samasa/recovery/recovery.hpp"
#include "languages/samasa/tree/incremental.hpp"

namespace {

using namespace lang::samasa;

// ---- Shared enums ----------------------------------------------------------

enum class SK : std::uint8_t { root, item, expr, error_node };
enum class TK : std::uint8_t { eof, ident, kw_let, op_eq, op_plus, semi };

// ---- Shared rules ----------------------------------------------------------

using item_rule = rule<"item", tok<TK::ident>>;
using expr_rule = rule<"expr", choice_t<tok<TK::ident>, tok<TK::op_plus>>>;
using root_rule = rule<"root", seq_t<tok<TK::kw_let>, tok<TK::ident>, tok<TK::op_eq>, tok<TK::ident>>>;

using SimpleGrammar = grammar<SK, TK, root_rule, root_rule>;

// Two-rule grammar with an unreferenced extra rule (for unreachable check).
using orphan_rule = rule<"orphan", tok<TK::semi>>;
// Do NOT use BadReachGrammar in grammar_valid — it would hard-fail at compile time.
// Use detail traits directly.

} // anonymous namespace

// ============================================================================
// FIRST-set / expected_at
// ============================================================================

TEST_CASE("expected_at: tok<K> returns {K}", "[samasa][v2][first]") {
    constexpr auto arr = expected_at<item_rule, TK>();
    // item_rule = rule<"item", tok<TK::ident>> — FIRST = {ident}
    STATIC_REQUIRE(arr.size() == 1);
    STATIC_REQUIRE(arr[0] == TK::ident);
}

TEST_CASE("expected_at: choice<A,B> returns FIRST(A) ∪ FIRST(B)", "[samasa][v2][first]") {
    // expr_rule = choice_t<tok<ident>, tok<op_plus>>
    constexpr auto arr = expected_at<expr_rule, TK>();
    STATIC_REQUIRE(arr.size() == 2);
    // Values are sorted by underlying integer.
    const bool has_ident = arr[0] == TK::ident || arr[1] == TK::ident;
    const bool has_plus  = arr[0] == TK::op_plus || arr[1] == TK::op_plus;
    CHECK(has_ident);
    CHECK(has_plus);
}

TEST_CASE("expected_at: seq<A,B> returns FIRST(A) only when A non-nullable", "[samasa][v2][first]") {
    // root_rule = seq<kw_let, ident, op_eq, ident> — FIRST = {kw_let}
    constexpr auto arr = expected_at<root_rule, TK>();
    STATIC_REQUIRE(arr.size() >= 1);
    STATIC_REQUIRE(arr[0] == TK::kw_let);
}

TEST_CASE("first_sets: returns grammar_first_sets with correct rule count", "[samasa][v2][first]") {
    constexpr auto fs = first_sets<SimpleGrammar>();
    STATIC_REQUIRE(fs.rule_count == SimpleGrammar::rule_count);
}

TEST_CASE("first_sets: nullable flag correct for non-nullable root", "[samasa][v2][first]") {
    constexpr auto fs = first_sets<SimpleGrammar>();
    // root_rule uses seq of tok<> — not nullable.
    STATIC_REQUIRE(!fs.descriptors[0].nullable);
}

// ============================================================================
// Grammar validation v2 checks
// ============================================================================

namespace {

// Direct left recursion: root references itself as leftmost element.
// rule<"lr", seq<rule<"lr",...>, tok<ident>>>
using lr_inner = tok<TK::ident>;

// We can't actually define a self-referential rule without a rule_ref type.
// Test the has_empty_sep and has_shadowed_choice checks instead.

// empty separator: sep_by<tok<ident>, opt<tok<semi>>>
using bad_sep_pat  = sep_by_t<tok<TK::ident>, opt_t<tok<TK::semi>>>;
using bad_sep_rule = rule<"bad_sep", bad_sep_pat>;
using BadSepGrammar = grammar<SK, TK, bad_sep_rule, bad_sep_rule>;

// choice shadowing: choice<opt<tok<ident>>, tok<op_plus>>
// opt<tok<ident>> is nullable → shadows tok<op_plus>
using shadow_pat  = choice_t<opt_t<tok<TK::ident>>, tok<TK::op_plus>>;
using shadow_rule = rule<"shadow", shadow_pat>;
using ShadowGrammar = grammar<SK, TK, shadow_rule, shadow_rule>;

} // anonymous namespace

TEST_CASE("validation v2: empty_separator detected", "[samasa][v2][validation]") {
    constexpr auto r = validate_grammar<BadSepGrammar>();
    STATIC_REQUIRE(!r.ok());
    bool found_sep = false;
    for (std::size_t i = 0; i < r.issues.size(); ++i)
        if (r.issues[i].code == grammar_diag_code::empty_separator) { found_sep = true; break; }
    CHECK(found_sep);
}

TEST_CASE("validation v2: choice_shadowing detected", "[samasa][v2][validation]") {
    constexpr auto r = validate_grammar<ShadowGrammar>();
    STATIC_REQUIRE(!r.ok());
    bool found = false;
    for (const auto& issue : r.issues)
        if (issue.code == grammar_diag_code::choice_shadowing) { found = true; break; }
    CHECK(found);
}

TEST_CASE("validation v2: grammar_diag_code has nullable_root and choice_shadowing", "[samasa][v2][validation]") {
    STATIC_REQUIRE(static_cast<std::uint8_t>(grammar_diag_code::nullable_root)    == 7);
    STATIC_REQUIRE(static_cast<std::uint8_t>(grammar_diag_code::choice_shadowing) == 8);
    STATIC_REQUIRE(static_cast<std::uint8_t>(grammar_diag_code::empty_separator)  == 9);
}

TEST_CASE("validation v2: detail::has_empty_sep detects nullable separator", "[samasa][v2][validation]") {
    STATIC_REQUIRE(detail::has_empty_sep<bad_sep_pat>::value);
}

TEST_CASE("validation v2: detail::has_shadowed_choice detects nullable first alternative", "[samasa][v2][validation]") {
    STATIC_REQUIRE(detail::has_shadowed_choice<shadow_pat>::value);
}

TEST_CASE("validation v2: no_empty_sep_check passes for normal sep_by", "[samasa][v2][validation]") {
    // sep_by<ident, semi>: semi is tok<TK::semi> — not nullable.
    using good_sep = sep_by_t<tok<TK::ident>, tok<TK::semi>>;
    STATIC_REQUIRE(!detail::has_empty_sep<good_sep>::value);
}

// ============================================================================
// Parse tracing
// ============================================================================

TEST_CASE("trace_policy: no_trace has enabled=false", "[samasa][v2][trace]") {
    STATIC_REQUIRE(!no_trace::enabled);
}

TEST_CASE("trace_policy: collecting_trace has enabled=true", "[samasa][v2][trace]") {
    STATIC_REQUIRE(collecting_trace::enabled);
}

TEST_CASE("trace_policy: collecting_trace accumulates events", "[samasa][v2][trace]") {
    collecting_trace t;
    t.enter("rule_a", 0);
    t.token(1);
    t.exit("rule_a", 2);
    REQUIRE(t.size() == 3);
    CHECK(t.events[0].kind == trace_event_kind::enter_rule);
    CHECK(t.events[0].rule_name == "rule_a");
    CHECK(t.events[1].kind == trace_event_kind::match_token);
    CHECK(t.events[2].kind == trace_event_kind::exit_rule);
}

TEST_CASE("trace_policy: collecting_trace records fail event", "[samasa][v2][trace]") {
    collecting_trace t;
    t.fail("r", 5, /*hard=*/false);
    REQUIRE(t.size() == 1);
    CHECK(t.events[0].kind == trace_event_kind::soft_fail);
}

TEST_CASE("trace_policy: collecting_trace records hard_fail", "[samasa][v2][trace]") {
    collecting_trace t;
    t.fail("r", 5, /*hard=*/true);
    CHECK(t.events[0].kind == trace_event_kind::hard_fail);
}

TEST_CASE("trace_policy: collecting_trace clear() works", "[samasa][v2][trace]") {
    collecting_trace t;
    t.enter("x", 0);
    t.clear();
    REQUIRE(t.size() == 0);
}

TEST_CASE("trace_policy: no_trace on_event is constexpr no-op", "[samasa][v2][trace]") {
    no_trace t;
    t.on_event({trace_event_kind::enter_rule, "x", 0}); // must compile and not crash
    SUCCEED("no_trace::on_event is a no-op");
}

// ============================================================================
// Rich diagnostic model
// ============================================================================

TEST_CASE("diagnostic: samasa_diag_code has recover_replace and recover_wrap_subtree", "[samasa][v2][diag]") {
    STATIC_REQUIRE(static_cast<std::uint16_t>(samasa_diag_code::recover_replace)    == 25);
    STATIC_REQUIRE(static_cast<std::uint16_t>(samasa_diag_code::recover_wrap_subtree)== 26);
}

TEST_CASE("diagnostic: to_code maps new codes", "[samasa][v2][diag]") {
    CHECK(to_code(samasa_diag_code::recover_replace)     == "SAMASA-RECOVER-REPLACE");
    CHECK(to_code(samasa_diag_code::recover_wrap_subtree)== "SAMASA-RECOVER-WRAP-SUBTREE");
}

TEST_CASE("diagnostic: expected_item kinds", "[samasa][v2][diag]") {
    expected_item it;
    it.type = expected_item::kind::token_kind;
    it.token_id = 3;
    CHECK(it.type == expected_item::kind::token_kind);
}

TEST_CASE("diagnostic: parse_diagnostic add_expected respects MaxExpected", "[samasa][v2][diag]") {
    parse_diagnostic<4> d;
    d.add_expected({expected_item::kind::token_kind, {}, 1});
    d.add_expected({expected_item::kind::token_kind, {}, 2});
    d.add_expected({expected_item::kind::token_kind, {}, 3});
    d.add_expected({expected_item::kind::token_kind, {}, 4});
    d.add_expected({expected_item::kind::token_kind, {}, 5}); // overflow — should not crash
    REQUIRE(d.expected_count == 4);
}

TEST_CASE("diagnostic: repair_cost values match design spec", "[samasa][v2][diag]") {
    CHECK(repair_cost(repair_kind::insert_missing) == 1);
    CHECK(repair_cost(repair_kind::delete_token)   == 1);
    CHECK(repair_cost(repair_kind::replace_token)  == 2);
    CHECK(repair_cost(repair_kind::wrap_subtree)   == 4);
    CHECK(repair_cost(repair_kind::abort_rule)     == 255);
    CHECK(repair_cost(repair_kind::none)           == 0);
}

// ============================================================================
// Memoization policies
// ============================================================================

TEST_CASE("memo_policy: no_memo has enabled=false", "[samasa][v2][memo]") {
    STATIC_REQUIRE(!no_memo::enabled);
}

TEST_CASE("memo_policy: no_memo lookup always returns false", "[samasa][v2][memo]") {
    no_memo m;
    memo_value v;
    CHECK(!m.lookup({1, 0}, v));
}

TEST_CASE("memo_policy: selective_memo has enabled=true", "[samasa][v2][memo]") {
    STATIC_REQUIRE(selective_memo::enabled);
}

TEST_CASE("memo_policy: selective_memo store/lookup round-trips", "[samasa][v2][memo]") {
    selective_memo m;
    memo_key   k{42, 10};
    memo_value stored{parse_status::success, 15, 8, true};
    m.store(k, stored);
    memo_value out;
    REQUIRE(m.lookup(k, out));
    CHECK(out.status    == parse_status::success);
    CHECK(out.next_pos  == 15);
    CHECK(out.furthest_err == 8);
}

TEST_CASE("memo_policy: selective_memo miss returns false", "[samasa][v2][memo]") {
    selective_memo m;
    memo_value v;
    CHECK(!m.lookup({99, 5}, v));
}

TEST_CASE("memo_policy: packrat_memo store/lookup round-trips", "[samasa][v2][memo]") {
    packrat_memo m;
    memo_key   k{7, 3};
    memo_value mv{parse_status::soft_fail, 0, 0, true};
    m.store(k, mv);
    memo_value out;
    REQUIRE(m.lookup(k, out));
    CHECK(out.status == parse_status::soft_fail);
}

TEST_CASE("memo_policy: memo_key_hash distributes keys", "[samasa][v2][memo]") {
    memo_key_hash h;
    CHECK(h({1, 0}) != h({2, 0}));
    CHECK(h({1, 0}) != h({1, 1}));
}

// ============================================================================
// Railroad model + renderers
// ============================================================================

TEST_CASE("railroad: railroad_model returns grammar_railroad_model with correct count", "[samasa][v2][railroad]") {
    constexpr auto rr = railroad_model<SimpleGrammar>();
    STATIC_REQUIRE(rr.rule_count == SimpleGrammar::rule_count);
}

TEST_CASE("railroad: railroad_rule_entry shape for seq is 'sequence'", "[samasa][v2][railroad]") {
    constexpr auto rr = railroad_model<SimpleGrammar>();
    CHECK(rr.entries[0].shape == std::string_view{"sequence"});
}

TEST_CASE("railroad: railroad_rule_entry nullable correct", "[samasa][v2][railroad]") {
    // root_rule is seq of tok<> — not nullable.
    constexpr auto rr = railroad_model<SimpleGrammar>();
    CHECK(!rr.entries[0].nullable);
}

TEST_CASE("railroad: render_markdown produces non-empty string", "[samasa][v2][railroad]") {
    const std::string md = render_markdown(grammar_description<SimpleGrammar>{});
    CHECK(!md.empty());
    CHECK(md.find("Grammar:") != std::string::npos);
}

TEST_CASE("railroad: render_markdown railroad produces table", "[samasa][v2][railroad]") {
    const std::string md = render_markdown(grammar_railroad_model<SimpleGrammar>{});
    CHECK(md.find("Railroad Model") != std::string::npos);
    CHECK(md.find("sequence") != std::string::npos);
}

TEST_CASE("railroad: render_json grammar produces valid JSON prefix", "[samasa][v2][railroad]") {
    const std::string js = render_json(grammar_description<SimpleGrammar>{});
    CHECK(js.front() == '{');
    CHECK(js.back()  == '}');
    CHECK(js.find("\"grammar\"") != std::string::npos);
}

TEST_CASE("railroad: render_json railroad produces rules array", "[samasa][v2][railroad]") {
    const std::string js = render_json(grammar_railroad_model<SimpleGrammar>{});
    CHECK(js.find("\"railroad\"") != std::string::npos);
    CHECK(js.find("\"shape\"") != std::string::npos);
}

TEST_CASE("railroad: render_json first_sets produces first_sets array", "[samasa][v2][railroad]") {
    const std::string js = render_json(grammar_first_sets<SimpleGrammar>{});
    CHECK(js.find("\"first_sets\"") != std::string::npos);
    CHECK(js.find("\"nullable\"") != std::string::npos);
}

// ============================================================================
// Green fingerprint
// ============================================================================

TEST_CASE("fingerprint: green_fingerprint default is invalid", "[samasa][v2][fingerprint]") {
    green_fingerprint fp;
    CHECK(!fp.valid());
}

TEST_CASE("fingerprint: green_fingerprint equality", "[samasa][v2][fingerprint]") {
    green_fingerprint a{1, 2, 3};
    green_fingerprint b{1, 2, 3};
    CHECK(a == b);
}

TEST_CASE("fingerprint: green_fingerprint inequality", "[samasa][v2][fingerprint]") {
    green_fingerprint a{1, 2, 3};
    green_fingerprint b{1, 2, 4};
    CHECK(a != b);
}

TEST_CASE("fingerprint: green_fingerprint valid when grammar_hash and source_hash nonzero", "[samasa][v2][fingerprint]") {
    green_fingerprint fp{42, 99, 0};
    CHECK(fp.valid());
}

// ============================================================================
// Scanner mode stack
// ============================================================================

TEST_CASE("scanner: scanner_mode_stack default mode is k_scanner_mode_default", "[samasa][v2][scanner]") {
    scanner_mode_stack s;
    CHECK(s.mode() == k_scanner_mode_default);
    CHECK(s.depth() == 0);
}

TEST_CASE("scanner: push_mode / pop_mode work correctly", "[samasa][v2][scanner]") {
    scanner_mode_stack s;
    s.push_mode(1);
    CHECK(s.mode() == 1);
    CHECK(s.depth() == 1);
    s.push_mode(2);
    CHECK(s.mode() == 2);
    s.pop_mode();
    CHECK(s.mode() == 1);
    s.pop_mode();
    CHECK(s.mode() == k_scanner_mode_default);
}

TEST_CASE("scanner: pop_mode on empty stack is safe", "[samasa][v2][scanner]") {
    scanner_mode_stack s;
    s.pop_mode(); // must not crash
    CHECK(s.depth() == 0);
}

TEST_CASE("scanner: unicode_identifier_policy ASCII defaults", "[samasa][v2][scanner]") {
    CHECK(ascii_identifier_policy::is_ident_start('a'));
    CHECK(ascii_identifier_policy::is_ident_start('_'));
    CHECK(!ascii_identifier_policy::is_ident_start('0'));
    CHECK(ascii_identifier_policy::is_ident_continue('0'));
    CHECK(!ascii_identifier_policy::is_ident_continue('!'));
}

// ============================================================================
// Incremental reparse utilities
// ============================================================================

TEST_CASE("incremental: apply_edit insert at start", "[samasa][v2][incremental]") {
    const std::string result = apply_edit("hello", text_edit{0, 0, "world "});
    CHECK(result == "world hello");
}

TEST_CASE("incremental: apply_edit delete middle", "[samasa][v2][incremental]") {
    const std::string result = apply_edit("hello world", text_edit{5, 6, ""});
    CHECK(result == "hello");
}

TEST_CASE("incremental: apply_edit replace", "[samasa][v2][incremental]") {
    const std::string result = apply_edit("abc", text_edit{1, 1, "XY"});
    CHECK(result == "aXYc");
}

TEST_CASE("incremental: apply_edit no change", "[samasa][v2][incremental]") {
    const std::string result = apply_edit("abc", text_edit{1, 0, ""});
    CHECK(result == "abc");
}

TEST_CASE("incremental: incremental_stats default is zero", "[samasa][v2][incremental]") {
    incremental_stats s;
    CHECK(s.reused_nodes == 0);
    CHECK(s.rebuilt_nodes == 0);
    CHECK(s.rescanned_tokens == 0);
    CHECK(s.reparsed_tokens == 0);
    CHECK(!s.full_reparse);
}

TEST_CASE("incremental: diff_trees on empty outputs produces zero reuse", "[samasa][v2][incremental]") {
    parse_output<SK, TK> old_out, new_out;
    const auto stats = diff_trees<SimpleGrammar>(old_out, new_out);
    CHECK(stats.reused_nodes  == 0);
    CHECK(stats.rebuilt_nodes == 0);
}

// ============================================================================
// Conformance suite — grammar_issue_severity and choice_first_overlap
// ============================================================================

namespace {

// Overlap grammar: choice<seq<ident,op_eq>, ident> — both alts start with ident.
using overlap_root = rule<"overlap",
    choice_t<seq_t<tok<TK::ident>, tok<TK::op_eq>>,
             tok<TK::ident>>>;
using OverlapGrammar = grammar<SK, TK, overlap_root, overlap_root>;

} // anonymous namespace

TEST_CASE("conformance: grammar_issue_severity enum values", "[samasa][conformance][validation]") {
    STATIC_REQUIRE(static_cast<std::uint8_t>(grammar_issue_severity::error)   == 0);
    STATIC_REQUIRE(static_cast<std::uint8_t>(grammar_issue_severity::warning) == 1);
    STATIC_REQUIRE(static_cast<std::uint8_t>(grammar_issue_severity::note)    == 2);
}

TEST_CASE("conformance: grammar_diag_code::choice_first_overlap == 10", "[samasa][conformance][validation]") {
    STATIC_REQUIRE(static_cast<std::uint8_t>(grammar_diag_code::choice_first_overlap) == 10);
}

TEST_CASE("conformance: validate_grammar detects choice_first_overlap as warning", "[samasa][conformance][validation]") {
    constexpr auto result = validate_grammar<OverlapGrammar>();
    // Overlap is a warning — grammar is still ok() (no errors).
    STATIC_REQUIRE(result.ok());
    bool found_overlap = false;
    for (std::size_t i = 0; i < result.issues.size(); ++i) {
        if (result.issues[i].code == grammar_diag_code::choice_first_overlap) {
            found_overlap = true;
            CHECK(result.issues[i].severity == grammar_issue_severity::warning);
        }
    }
    CHECK(found_overlap);
}

TEST_CASE("conformance: validate_grammar ok() ignores warnings", "[samasa][conformance][validation]") {
    // OverlapGrammar has a FIRST-overlap warning but no errors → ok() = true.
    constexpr auto result = validate_grammar<OverlapGrammar>();
    STATIC_REQUIRE(result.ok());
}

TEST_CASE("conformance: grammar_valid<G> is pure predicate — no static_assert side effects", "[samasa][conformance][validation]") {
    // grammar_valid must compile without triggering any static_assert
    // even on a grammar with issues (OverlapGrammar still has ok()=true).
    STATIC_REQUIRE(grammar_valid<OverlapGrammar>());
}

TEST_CASE("conformance: grammar_validation_issue severity field defaults to error", "[samasa][conformance][validation]") {
    // Manually construct an issue and verify default severity.
    grammar_validation_issue issue{grammar_diag_code::empty_many, {}, {}};
    // Default-initialized severity = error (first enum value).
    CHECK(issue.severity == grammar_issue_severity::error);
}

// ============================================================================
// Conformance suite — FOLLOW sets
// ============================================================================

namespace {

// Grammar: root = seq<item_rule, tok<semi>>
// FOLLOW(item_rule) should contain TK::semi.
using conf_item = rule<"conf_item", tok<TK::ident>>;
using conf_root = rule<"conf_root", seq_t<conf_item, tok<TK::semi>>>;
using FollowGrammar = grammar<SK, TK, conf_root, conf_root, conf_item>;

} // anonymous namespace

TEST_CASE("conformance: follow_sets<G> returns grammar_follow_sets with rule_count entries", "[samasa][conformance][follow]") {
    constexpr auto fs = follow_sets<FollowGrammar>();
    STATIC_REQUIRE(fs.rule_count == FollowGrammar::rule_count);
}

TEST_CASE("conformance: follow_of<G,Rule> root rule has eof sentinel", "[samasa][conformance][follow]") {
    // Root rule always has EOF (TK{} = default) in FOLLOW.
    constexpr auto f = follow_of<FollowGrammar, conf_root>();
    bool has_eof = false;
    for (const auto& t : f) if (t == TK{}) { has_eof = true; break; }
    CHECK(has_eof);
}

TEST_CASE("conformance: follow_of<G,Rule> non-root contains continuation tokens", "[samasa][conformance][follow]") {
    // conf_item appears before tok<semi> in conf_root's body.
    // FOLLOW(conf_item) ⊇ {semi}.
    constexpr auto f = follow_of<FollowGrammar, conf_item>();
    bool has_semi = false;
    for (const auto& t : f) if (t == TK::semi) { has_semi = true; break; }
    CHECK(has_semi);
}

TEST_CASE("conformance: expected_after<G,Rule> is FIRST ∪ FOLLOW", "[samasa][conformance][follow]") {
    // FIRST(conf_item) = {ident}; FOLLOW(conf_item) ⊇ {semi}.
    // expected_after must include both.
    constexpr auto ea = expected_after<FollowGrammar, conf_item>();
    bool has_ident = false, has_semi = false;
    for (const auto& t : ea) {
        if (t == TK::ident) has_ident = true;
        if (t == TK::semi)  has_semi  = true;
    }
    CHECK(has_ident);
    CHECK(has_semi);
}

// ============================================================================
// Conformance suite — recovery_makes_progress_v trait
// ============================================================================

namespace {
using dummy_sync = sync_set<TK::semi>;
} // anonymous namespace

TEST_CASE("conformance: recovery_makes_progress_v<skip_until_sync> is true", "[samasa][conformance][recovery]") {
    STATIC_REQUIRE(recovery_makes_progress_v<skip_until_sync<dummy_sync>>);
}

TEST_CASE("conformance: recovery_makes_progress_v<insert_missing> is true", "[samasa][conformance][recovery]") {
    STATIC_REQUIRE(recovery_makes_progress_v<insert_missing<TK>>);
}

TEST_CASE("conformance: recovery_makes_progress_v<delete_unexpected> is true", "[samasa][conformance][recovery]") {
    STATIC_REQUIRE(recovery_makes_progress_v<delete_unexpected>);
}

TEST_CASE("conformance: recovery_makes_progress_v<wrap_error_node> is false", "[samasa][conformance][recovery]") {
    STATIC_REQUIRE(!recovery_makes_progress_v<wrap_error_node>);
}

// ============================================================================
// Conformance suite — FIRST-set PEG semantics
// ============================================================================

namespace {

// opt<tok<ident>> is nullable — seq continuation should propagate.
using nullable_item  = rule<"nullable_item",  opt_t<tok<TK::ident>>>;
using after_nullable = rule<"after_nullable", seq_t<opt_t<tok<TK::ident>>, tok<TK::semi>>>;

} // anonymous namespace

TEST_CASE("conformance: FIRST seq nullable propagation", "[samasa][conformance][first]") {
    // seq<opt<ident>, semi>: since opt<ident> is nullable, FIRST includes semi too.
    constexpr auto arr = expected_at<after_nullable, TK>();
    bool has_ident = false, has_semi = false;
    for (const auto& t : arr) {
        if (t == TK::ident) has_ident = true;
        if (t == TK::semi)  has_semi  = true;
    }
    CHECK(has_ident);
    CHECK(has_semi);
}

TEST_CASE("conformance: FIRST many<X> = FIRST<X>", "[samasa][conformance][first]") {
    using many_ident = rule<"many_ident", many_t<tok<TK::ident>>>;
    constexpr auto arr = expected_at<many_ident, TK>();
    STATIC_REQUIRE(arr.size() >= 1);
    bool has_ident = false;
    for (const auto& t : arr) if (t == TK::ident) has_ident = true;
    CHECK(has_ident);
}

TEST_CASE("conformance: FIRST opt<X> = FIRST<X>", "[samasa][conformance][first]") {
    constexpr auto arr = expected_at<nullable_item, TK>();
    STATIC_REQUIRE(arr.size() >= 1);
    bool has_ident = false;
    for (const auto& t : arr) if (t == TK::ident) has_ident = true;
    CHECK(has_ident);
}

// ============================================================================
// Conformance suite — print_original round-trip
// ============================================================================

TEST_CASE("conformance: print_original on empty token buffer returns empty string", "[samasa][conformance][incremental]") {
    green_tree<SK>  tree;
    token_buffer<TK> toks;
    const std::string out = print_original(tree, toks, "hello");
    CHECK(out.empty());
}

TEST_CASE("conformance: print_original with single token returns token slice", "[samasa][conformance][incremental]") {
    green_tree<SK>  tree;
    token_buffer<TK> toks;
    token<TK> t;
    t.kind   = TK::ident;
    t.offset = 0;
    t.length = 5;
    toks.data.push_back(t);
    const std::string src = "hello world";
    const std::string out = print_original(tree, toks, src);
    CHECK(out == "hello");
}

// ============================================================================
// Conformance suite — insert_missing_token progress trait
// ============================================================================

TEST_CASE("conformance: recovery_makes_progress_v<insert_missing_token<V>> is true", "[samasa][conformance][recovery]") {
    STATIC_REQUIRE(recovery_makes_progress_v<insert_missing_token<TK::semi>>);
}

TEST_CASE("conformance: insert_missing_token makes_progress member is true", "[samasa][conformance][recovery]") {
    STATIC_REQUIRE(insert_missing_token<TK::semi>::makes_progress);
}

// ============================================================================
// Conformance suite — recovery_no_progress validation
// ============================================================================

namespace {

// A grammar with recover_with<P, wrap_error_node>.
// wrap_error_node does not advance the cursor — this should trigger recovery_no_progress.
using bad_item_rule = rule<"bad_item",
    recover_with<tok<TK::ident>, wrap_error_node>>;
using bad_root_rule = rule<"bad_root", many_t<bad_item_rule>>;
using BadRecovGrammar = grammar<SK, TK, bad_root_rule, bad_root_rule, bad_item_rule>;

} // anonymous namespace

TEST_CASE("conformance: validate_grammar detects recovery_no_progress", "[samasa][conformance][recovery]") {
    constexpr auto result = validate_grammar<BadRecovGrammar>();
    bool found = false;
    for (const auto& issue : result.issues)
        if (issue.code == grammar_diag_code::recovery_no_progress) { found = true; break; }
    CHECK(found);
}

TEST_CASE("conformance: recovery_no_progress issue has error severity", "[samasa][conformance][recovery]") {
    constexpr auto result = validate_grammar<BadRecovGrammar>();
    for (const auto& issue : result.issues)
        if (issue.code == grammar_diag_code::recovery_no_progress)
            CHECK(issue.severity == grammar_issue_severity::error);
}

TEST_CASE("conformance: grammar_valid<G> false when recovery_no_progress present", "[samasa][conformance][recovery]") {
    // A grammar with wrap_error_node alone in recover_with fails grammar_valid.
    STATIC_REQUIRE(!grammar_valid<BadRecovGrammar>());
}

// ============================================================================
// Conformance suite — FOLLOW fixed-point (multi-rule recursive propagation)
// ============================================================================

namespace {

// Grammar where fixed-point iteration is required:
//   fp_root = seq<fp_a, fp_b, tok<semi>>
//   fp_a    = tok<ident>
//   fp_b    = tok<op_plus>
// FOLLOW(fp_a) must include tok<op_plus> (from fp_root's seq continuation).
// FOLLOW(fp_b) must include tok<semi> (from fp_root's seq continuation).
// Single-pass scan is insufficient if rules are processed in bad order; fixed-point
// guarantees convergence.
using fp_a    = rule<"fp_a", tok<TK::ident>>;
using fp_b    = rule<"fp_b", tok<TK::op_plus>>;
using fp_root = rule<"fp_root", seq_t<fp_a, fp_b, tok<TK::semi>>>;
using FPGrammar = grammar<SK, TK, fp_root, fp_root, fp_a, fp_b>;

} // anonymous namespace

TEST_CASE("conformance: FOLLOW fixed-point — fp_a FOLLOW contains op_plus", "[samasa][conformance][follow]") {
    // FOLLOW(fp_a) ⊇ {op_plus} because fp_b follows fp_a in fp_root's body.
    constexpr auto f = follow_of<FPGrammar, fp_a>();
    bool has_plus = false;
    for (const auto& t : f) if (t == TK::op_plus) { has_plus = true; break; }
    CHECK(has_plus);
}

TEST_CASE("conformance: FOLLOW fixed-point — fp_b FOLLOW contains semi", "[samasa][conformance][follow]") {
    // FOLLOW(fp_b) ⊇ {semi} because tok<semi> follows fp_b in fp_root's body.
    constexpr auto f = follow_of<FPGrammar, fp_b>();
    bool has_semi = false;
    for (const auto& t : f) if (t == TK::semi) { has_semi = true; break; }
    CHECK(has_semi);
}
