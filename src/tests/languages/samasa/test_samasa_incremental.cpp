// ============================================================================
// test_samasa_incremental.cpp — Stage 6: partial-window incremental reparse.
//
// Covers:
//   - token_range_for_span: boundary, mid-token, empty span, EOF cases
//   - find_affected_root + default boundary policy
//   - find_affected_root with expand-to-root policy
//   - splice_subtree / recompute_ancestor_hashes (via green_arena directly)
//   - partial == full equivalence (core correctness gate)
//   - incremental_stats: localized edit rebuilds strict subset of nodes
// ============================================================================

#include "catch_amalgamated.hpp"
#include "languages/samasa/samasa.hpp"
#include "languages/samasa/tree/incremental.hpp"

namespace {

using namespace lang::samasa;

// ---- Shared grammar -------------------------------------------------------

enum class SK : std::uint8_t { root, stmt, expr, name, number, eof_node };
enum class TK : std::uint8_t { eof, ident, number_lit, kw_let, op_eq, semi };

// Token kind descriptor: maps scanner categories to enum values.
inline scan_token_kinds<TK> make_tk_kinds() {
    return { TK::eof, TK::ident, TK::number_lit, TK::number_lit, TK::ident, TK::ident };
}

// Keyword table: "let" → kw_let.
using TestKWTable = keyword_table<keyword<"let", TK::kw_let>>;

// Operator trie: "=" → op_eq, ";" → semi.
using TestOpTrie  = operator_trie<operator_token<"=", TK::op_eq>, operator_token<";", TK::semi>>;

// Grammar: root → stmt*;  stmt → "let" ident "=" ident ";"
using name_rule  = rule<"name",  tok<TK::ident>>;
using num_rule   = rule<"num",   tok<TK::number_lit>>;
using stmt_rule  = rule<"stmt",  seq_t<tok<TK::kw_let>,
                                       tok<TK::ident>,
                                       tok<TK::op_eq>,
                                       tok<TK::ident>,
                                       tok<TK::semi>>>;
using root_rule  = rule<"root",  many_t<stmt_rule>>;

struct TestGrammar {
    using syntax_kind = SK;
    using token_kind  = TK;
    using root_rule   = ::root_rule;
};

// Helper: parse with correct scan configuration.
inline auto test_parse(std::string_view src) {
    return parse<TestGrammar, TestKWTable, TestOpTrie>(src, {}, make_tk_kinds());
}

// Minimal single-statement source.
constexpr std::string_view kSrc1 = "let x = y ;";
// Two statements.
constexpr std::string_view kSrc2 = "let a = b ; let c = d ;";

// ============================================================================
// token_range_for_span
// ============================================================================

TEST_CASE("token_range_for_span: empty tokens → empty range", "[samasa][incremental][trsf]") {
    lang::samasa::token_buffer<TK> buf;
    const auto tr = token_range_for_span(buf.view(), {0, 5});
    CHECK(tr.empty());
}

TEST_CASE("token_range_for_span: empty span → empty range", "[samasa][incremental][trsf]") {
    auto out = test_parse(kSrc1);
    REQUIRE(out.success);
    const auto tr = token_range_for_span(out.tokens.view(), {3, 0});
    CHECK(tr.empty());
}

TEST_CASE("token_range_for_span: span covering first token", "[samasa][incremental][trsf]") {
    auto out = test_parse(kSrc1);
    REQUIRE(out.success);
    // kSrc1 = "let x = y ;"  → first token 'let' at offset 0, length 3.
    const auto tr = token_range_for_span(out.tokens.view(), {0, 3});
    CHECK(tr.start == 0);
    CHECK(tr.end >= 1u);
}

TEST_CASE("token_range_for_span: span past end → empty", "[samasa][incremental][trsf]") {
    auto out = test_parse(kSrc1);
    REQUIRE(out.success);
    const std::uint32_t far = static_cast<std::uint32_t>(kSrc1.size()) + 100;
    const auto tr = token_range_for_span(out.tokens.view(), {far, 5});
    CHECK(tr.empty());
}

TEST_CASE("token_range_for_span: span covering whole source", "[samasa][incremental][trsf]") {
    auto out = test_parse(kSrc1);
    REQUIRE(out.success);
    const auto n = static_cast<std::uint32_t>(kSrc1.size());
    const auto tr = token_range_for_span(out.tokens.view(), {0, n});
    CHECK(tr.start == 0);
    // Should include all content tokens (not necessarily EOF depending on scanner).
    CHECK(tr.size() >= 4u);
}

// ============================================================================
// find_affected_root
// ============================================================================

TEST_CASE("find_affected_root: edit inside a leaf → affected node found", "[samasa][incremental][far]") {
    auto out = test_parse(kSrc1);
    REQUIRE(out.success);

    // Edit replacing 'x' (somewhere in the middle of kSrc1 = "let x = y ;").
    // 'x' is at offset 4, length 1.
    text_edit ed{4, 1, "z"};
    default_reparse_boundary_policy<SK> pol;
    const auto result = find_affected_root<default_reparse_boundary_policy<SK>>(
        out.tree, ed, pol);

    CHECK(result.id != k_null_green);
    // Affected span must contain the edit.
    CHECK(result.span.offset <= ed.offset);
    CHECK(result.span.end()  >= ed.offset + ed.removed_length);
}

TEST_CASE("find_affected_root: empty tree → null result", "[samasa][incremental][far]") {
    green_tree<SK> empty;
    text_edit ed{0, 1, "x"};
    default_reparse_boundary_policy<SK> pol;
    const auto r = find_affected_root<default_reparse_boundary_policy<SK>>(empty, ed, pol);
    CHECK(r.id == k_null_green);
}

// Policy that always expands to root.
template <class SyntaxKind>
struct always_expand_policy {
    static constexpr bool should_expand([[maybe_unused]] SyntaxKind) noexcept { return true; }
};

TEST_CASE("find_affected_root: always-expand policy → returns root", "[samasa][incremental][far]") {
    auto out = test_parse(kSrc1);
    REQUIRE(out.success);

    text_edit ed{4, 1, "z"};
    always_expand_policy<SK> pol;
    const auto result = find_affected_root<always_expand_policy<SK>>(out.tree, ed, pol);

    CHECK(result.id == out.tree.root());
}

// ============================================================================
// splice_subtree / recompute_ancestor_hashes
// ============================================================================

TEST_CASE("splice_subtree + recompute_ancestor_hashes: spliced tree hashes == full rebuild",
          "[samasa][incremental][splice]")
{
    // Parse source A, parse source B, splice B's subtree into A's tree at root.
    // recompute_ancestor_hashes. The root hash should equal B's root hash.

    constexpr std::string_view srcA = "let x = y ;";
    constexpr std::string_view srcB = "let a = b ;";

    auto outA = test_parse(srcA);
    auto outB = test_parse(srcB);
    REQUIRE(outA.success);
    REQUIRE(outB.success);

    const std::uint64_t hash_b_root = outB.tree[outB.tree.root()].structural_hash;

    // Splice B's arena into A's at A's root.
    lang::green_arena<SK>& arenaA =
        static_cast<lang::green_arena<SK>&>(outA.tree);
    arenaA.splice_subtree(outA.tree.root(),
                          static_cast<const lang::green_arena<SK>&>(outB.tree));
    arenaA.recompute_ancestor_hashes(outA.tree.root());

    // After splice+recompute the root hash must match B's root.
    CHECK(arenaA[arenaA.root()].structural_hash == hash_b_root);
}

// ============================================================================
// partial == full equivalence (core correctness gate)
// ============================================================================

// Helper: tree structural equality by node-for-node hash comparison.
template <class SK2>
bool trees_equal(const green_tree<SK2>& a, const green_tree<SK2>& b) {
    if (a.size() != b.size()) return false;
    if (a.empty() && b.empty()) return true;
    // Compare structural_hash at root — sufficient because the hash is recursive.
    return a[a.root()].structural_hash == b[b.root()].structural_hash;
}

TEST_CASE("partial == full: insert mid-identifier", "[samasa][incremental][equiv]") {
    const std::string src  = "let x = y ;";
    // Replace 'x' (offset 4, len 1) with 'xy'.
    const text_edit   edit{4, 1, "xy"};
    const std::string new_src = apply_edit(src, edit);

    auto old_out = test_parse(src);
    REQUIRE(old_out.success);

    // Partial reparse.
    incremental_stats st;
    auto partial = reparse<TestGrammar, TestKWTable, TestOpTrie>(old_out, edit, new_src, st, {}, make_tk_kinds());

    // Full reparse.
    auto full = test_parse(new_src);

    REQUIRE(partial.success == full.success);
    CHECK(trees_equal(partial.tree, full.tree));
}

TEST_CASE("partial == full: delete identifier", "[samasa][incremental][equiv]") {
    const std::string src  = "let abc = xyz ;";
    // Delete 'abc' (offset 4, len 3) → replace with 'q'.
    const text_edit   edit{4, 3, "q"};
    const std::string new_src = apply_edit(src, edit);

    auto old_out = test_parse(src);
    REQUIRE(old_out.success);

    incremental_stats st;
    auto partial = reparse<TestGrammar, TestKWTable, TestOpTrie>(old_out, edit, new_src, st, {}, make_tk_kinds());
    auto full    = test_parse(new_src);

    REQUIRE(partial.success == full.success);
    CHECK(trees_equal(partial.tree, full.tree));
}

TEST_CASE("partial == full: replace identifier at node boundary", "[samasa][incremental][equiv]") {
    const std::string src  = "let a = b ;";
    // Replace 'b' (offset 8, len 1) with 'bbb'.
    const text_edit   edit{8, 1, "bbb"};
    const std::string new_src = apply_edit(src, edit);

    auto old_out = test_parse(src);
    REQUIRE(old_out.success);

    incremental_stats st;
    auto partial = reparse<TestGrammar, TestKWTable, TestOpTrie>(old_out, edit, new_src, st, {}, make_tk_kinds());
    auto full    = test_parse(new_src);

    REQUIRE(partial.success == full.success);
    CHECK(trees_equal(partial.tree, full.tree));
}

TEST_CASE("partial == full: edit at start of source", "[samasa][incremental][equiv]") {
    const std::string src  = "let x = y ;";
    // Replace 'let' (offset 0, len 3) — stays 'let' for syntactic validity.
    const text_edit   edit{0, 3, "let"};
    const std::string new_src = apply_edit(src, edit);

    auto old_out = test_parse(src);
    REQUIRE(old_out.success);

    incremental_stats st;
    auto partial = reparse<TestGrammar, TestKWTable, TestOpTrie>(old_out, edit, new_src, st, {}, make_tk_kinds());
    auto full    = test_parse(new_src);

    REQUIRE(partial.success == full.success);
    CHECK(trees_equal(partial.tree, full.tree));
}

// ============================================================================
// incremental_stats: localized edit rebuilds strict subset
// ============================================================================

TEST_CASE("incremental_stats: localized edit does not full-reparse when subtree found",
          "[samasa][incremental][stats]")
{
    // Two-statement source; edit inside second statement only.
    const std::string src  = std::string(kSrc2);
    // kSrc2 = "let a = b ; let c = d ;"
    // 'd' is near offset 20 (rough); we just replace the last ident.
    const auto d_pos = src.rfind('d');
    REQUIRE(d_pos != std::string::npos);
    const text_edit edit{static_cast<std::uint32_t>(d_pos), 1, "z"};
    const std::string new_src = apply_edit(src, edit);

    auto old_out = test_parse(src);
    REQUIRE(old_out.success);

    incremental_stats st;
    auto partial = reparse<TestGrammar, TestKWTable, TestOpTrie>(old_out, edit, new_src, st, {}, make_tk_kinds());
    auto full    = test_parse(new_src);

    // Core gate: structural equivalence.
    REQUIRE(partial.success == full.success);
    CHECK(trees_equal(partial.tree, full.tree));

    // Stats gate: when partial path was taken, rebuilt < total nodes.
    if (!st.full_reparse) {
        CHECK(st.rebuilt_nodes < old_out.tree.size());
    }
}

TEST_CASE("incremental_stats: full_reparse set on root-wide edit", "[samasa][incremental][stats]") {
    const std::string src = std::string(kSrc1);
    // Edit covering the entire source.
    const text_edit edit{0, static_cast<std::uint32_t>(src.size()), "let z = w ;"};
    const std::string new_src = apply_edit(src, edit);

    auto old_out = test_parse(src);
    REQUIRE(old_out.success);

    incremental_stats st;
    auto partial = reparse<TestGrammar, TestKWTable, TestOpTrie>(old_out, edit, new_src, st, {}, make_tk_kinds());
    auto full    = test_parse(new_src);

    REQUIRE(partial.success == full.success);
    CHECK(trees_equal(partial.tree, full.tree));
    // Root-covering edit → full_reparse expected.
    CHECK(st.full_reparse);
}

} // anonymous namespace
