// =============================================================================
// test_vakya_construction.cpp — Smoke tests for the standalone Vākya library.
//
// Verifies: include/vakya/vakya.hpp   (construction surface)
//           include/vakya/pattern.hpp (pattern DSL)
//
// Vākya is the structural-construction layer extracted from Lithe. These tests
// exercise the pure vakya:: surface WITHOUT including any Lithe compiler header,
// proving Vākya stands alone (no upward dependency on semantic/passes/codegen).
//
// Cases:
//   1. node/interface: x + y * 2 builds the expected tag structure.
//   2. as_expr wrappers: lvalue → expr_ref, rvalue → expr.
//   3. structural_equal / structural_hash agree for equal trees.
//   4. structural_equal distinguishes different trees.
//   5. tree::size / depth / arity on a small tree.
//   6. emit::tag_descriptor built-in metadata (symbol/id/arity).
//   7. graph::build_dag interns a shared subexpression once (CSE).
//   8. IRBuilder programmatic construction matches operator surface.
//   9. pattern: add(pv<0>, lit<0>) matches add(x, 0), binds x.
//  10. pattern: named rule("add_zero", …) carries its label and fires.
// =============================================================================

#include "catch_amalgamated.hpp"

#include "vakya/vakya.hpp"
#include "vakya/pattern.hpp"

// ============================================================================
// Test 1 — node/interface: x + y * 2 builds add(x, mul(y, 2)).
// ============================================================================

TEST_CASE (



"vakya: operator surface builds expected tag structure"
,
"[vakya][construction]"
)
{
    int x = 3, y = 4;
    auto e = vakya::as_expr(x) + vakya::as_expr(y) * 2;
    using E = decltype(e);
    STATIC_REQUIRE(std::is_same_v<typename E::tag_type, vakya::add_tag>);
    STATIC_REQUIRE(vakya::Expression<E>);
    CHECK(vakya::tree::arity(e) == 2);
}

// ============================================================================
// Test 2 — as_expr wrappers: lvalue → expr_ref, rvalue → expr.
// ============================================================================

TEST_CASE (



"vakya: as_expr wraps lvalues by ref and rvalues by value"
,
"[vakya][construction]"
)
{
    int x = 7;
    auto lref = vakya::as_expr(x);
    auto rval = vakya::as_expr(42);
    STATIC_REQUIRE(vakya::is_expr_ref_wrapper_v<decltype(lref)>);
    STATIC_REQUIRE(vakya::is_expr_wrapper_v<decltype(rval)>);
    CHECK(*static_cast<int*>(lref) == 7);
}

// ============================================================================
// Test 3 — structural_equal / structural_hash agree for equal trees.
// ============================================================================

TEST_CASE (



"vakya: structural_equal and structural_hash agree on equal trees"
,
"[vakya][hash]"
)
{
    int x = 3, y = 4;
    auto a = vakya::as_expr(x) + vakya::as_expr(y) * 2;
    auto b = vakya::as_expr(x) + vakya::as_expr(y) * 2;
    CHECK(vakya::structural_equal(a, b));
    CHECK(vakya::structural_hash(a) == vakya::structural_hash(b));
}

// ============================================================================
// Test 4 — structural_equal distinguishes different trees.
// ============================================================================

TEST_CASE (



"vakya: structural_equal distinguishes different trees"
,
"[vakya][hash]"
)
{
    int x = 3, y = 4;
    auto a = vakya::as_expr(x) + vakya::as_expr(y);
    auto b = vakya::as_expr(x) * vakya::as_expr(y);
    CHECK_FALSE(vakya::structural_equal(a, b));
}

// ============================================================================
// Test 5 — tree::size / depth / arity.
// ============================================================================

TEST_CASE (



"vakya: tree metrics on a small tree"
,
"[vakya][tree]"
)
{
    int x = 3, y = 4;
    auto e = vakya::as_expr(x) + vakya::as_expr(y) * 2; // add(x, mul(y, 2))
    CHECK(vakya::tree::arity(e) == 2);
    CHECK(vakya::tree::depth(e) >= 3);
    CHECK(vakya::tree::size(e) >= 5); // add, x, mul, y, 2
}

// ============================================================================
// Test 6 — emit::tag_descriptor built-in metadata.
// ============================================================================

TEST_CASE (



"vakya: built-in tag_descriptor metadata"
,
"[vakya][tags]"
)
{
    using vakya::emit::tag_descriptor;
    CHECK(tag_descriptor<vakya::add_tag>::symbol == "+");
    CHECK(tag_descriptor<vakya::add_tag>::arity == 2);
    CHECK(tag_descriptor<vakya::neg_tag>::arity == 1);
    CHECK(vakya::emit::tag_id<vakya::add_tag>::value == 1u);
}

// ============================================================================
// Test 7 — graph::build_dag interns shared subexpressions (CSE).
// ============================================================================

TEST_CASE (



"vakya: build_dag interns a shared subexpression"
,
"[vakya][dag]"
)
{
    int x = 3, y = 4;
    auto sub = vakya::as_expr(x) + vakya::as_expr(y);
    auto e   = sub + sub; // (x+y) + (x+y) — the child is structurally shared
    auto dag = vakya::graph::build_dag(e);
    CHECK(dag.size() >= 1);
    // At least one node is used more than once after interning.
    CHECK(dag.sharing_count() >= 1);
}

// ============================================================================
// Test 8 — IRBuilder programmatic construction matches operator surface.
// ============================================================================

TEST_CASE (



"vakya: IRBuilder matches the operator surface structurally"
,
"[vakya][construction]"
)
{
    int x = 3, y = 4;
    vakya::IRBuilder b;
    auto viaBuilder = b.CreateAdd(vakya::as_expr(x),
                                  b.CreateMul(vakya::as_expr(y), vakya::as_expr(2)));
    auto viaOps     = vakya::as_expr(x) + vakya::as_expr(y) * vakya::as_expr(2);
    CHECK(vakya::structural_equal(viaBuilder, viaOps));
}

// ============================================================================
// Test 9 — pattern: add(pv<0>, lit<0>) matches add(x, 0), binds x.
// ============================================================================

TEST_CASE (



"vakya::pattern: add(pv<0>, lit<0>) matches add(x, 0)"
,
"[vakya][pattern]"
)
{
    namespace pat = vakya::pattern;
    int x = 5;
    auto p = pat::add(pat::pv<0>, pat::lit<0>);
    auto e = vakya::make_node<vakya::add_tag>(vakya::as_expr(x), vakya::as_expr(0));
    auto m = pat::match_pattern(p, e);
    REQUIRE(m.has_value());
    CHECK(m->has(std::size_t{0}));
}

// ============================================================================
// Test 10 — pattern: named rule carries its label and fires via a rule_set.
// ============================================================================

TEST_CASE (



"vakya::pattern: named rule('add_zero', …) labels and fires"
,
"[vakya][pattern]"
)
{
    namespace pat = vakya::pattern;
    auto r = pat::rule("add_zero",
                       pat::add(pat::pv<0>, pat::lit<0>),
                       [](const pat::match_result& m) -> std::optional<std::any> {
                           return m.get<std::any>(std::size_t{0});
                       });
    CHECK(r.name == "add_zero");

    auto rs = pat::make_rule_set(r);
    int x = 9;
    auto e = vakya::make_node<vakya::add_tag>(vakya::as_expr(x), vakya::as_expr(0));
    auto out = rs.apply_first(e);
    CHECK(out.has_value());
}

// ============================================================================
// Test 11 — is_terminal concept-based detection hook.
//   A custom struct with `using vakya_terminal = void;` satisfies Terminal
//   without an explicit is_terminal<T> specialization.
// ============================================================================

TEST_CASE (



"vakya: vakya_terminal tag opts custom type into Terminal concept"
,
"[vakya][terminal]"
)
{
    struct MyScalar {
        using vakya_terminal = void;
        float v;
        explicit MyScalar(float x) : v(x) {}
    };

    STATIC_REQUIRE(vakya::Terminal<MyScalar>);
    STATIC_REQUIRE(vakya::is_terminal_v<MyScalar>);

    // Custom terminal participates in operator+.
    MyScalar a{1.0f};
    int b = 2;
    auto e = vakya::as_expr(a) + vakya::as_expr(b);
    STATIC_REQUIRE(std::is_same_v<typename decltype(e)::tag_type, vakya::add_tag>);
}

// ============================================================================
// Test 12 — Non-linear pattern matching.
//   Pattern add(pv<0>, pv<0>) must match add(x, x) but reject add(x, y).
// ============================================================================

TEST_CASE (



"vakya::pattern: non-linear pattern var requires structural equality"
,
"[vakya][pattern][nonlinear]"
)
{
    namespace pat = vakya::pattern;

    // Pattern: x + x  (pv<0> appears twice)
    auto double_pat = pat::add(pat::pv<0>, pat::pv<0>);

    int x = 5, y = 7;

    // Same lvalue used for both children: must match.
    auto same = vakya::make_node<vakya::add_tag>(vakya::as_expr(x), vakya::as_expr(x));
    auto m_same = pat::match_pattern(double_pat, same);
    CHECK(m_same.has_value());

    // Different lvalues: must NOT match.
    auto diff = vakya::make_node<vakya::add_tag>(vakya::as_expr(x), vakya::as_expr(y));
    auto m_diff = pat::match_pattern(double_pat, diff);
    CHECK_FALSE(m_diff.has_value());
}

// ============================================================================
// Test 13 — Commutative pattern matching.
//   Pattern add(pv<0>, lit<0>) (x+0) also matches add(0, x) (0+x) automatically
//   because add_tag has is_commutative = true.
// ============================================================================

TEST_CASE (



"vakya::pattern: commutative tag matches reversed operand order"
,
"[vakya][pattern][commutative]"
)
{
    namespace pat = vakya::pattern;

    // Pattern: pv<0> + 0
    auto p = pat::add(pat::pv<0>, pat::lit<0>);

    int x = 3;

    // Canonical order: x + 0 — must match directly.
    auto canonical = vakya::make_node<vakya::add_tag>(vakya::as_expr(x), vakya::as_expr(0));
    auto m1 = pat::match_pattern(p, canonical);
    CHECK(m1.has_value());

    // Reversed order: 0 + x — must match via commutative retry.
    auto reversed = vakya::make_node<vakya::add_tag>(vakya::as_expr(0), vakya::as_expr(x));
    auto m2 = pat::match_pattern(p, reversed);
    CHECK(m2.has_value());

    // sub is NOT commutative: 0 - x must NOT match sub(pv<0>, lit<0>).
    auto sub_pat = pat::sub(pat::pv<0>, pat::lit<0>);
    auto sub_rev = vakya::make_node<vakya::sub_tag>(vakya::as_expr(0), vakya::as_expr(x));
    auto m3 = pat::match_pattern(sub_pat, sub_rev);
    CHECK_FALSE(m3.has_value());
}

// ============================================================================
// Test 14 — tag_descriptor::is_commutative metadata.
//   add and mul are commutative; the default (primary template) is false.
// ============================================================================

TEST_CASE (



"vakya: tag_descriptor::is_commutative is true for add/mul, false by default"
,
"[vakya][tag]"
)
{
    STATIC_REQUIRE(vakya::emit::tag_descriptor<vakya::add_tag>::is_commutative);
    STATIC_REQUIRE(vakya::emit::tag_descriptor<vakya::mul_tag>::is_commutative);
    // Primary template (any unspecialized tag) defaults to false.
    struct my_custom_tag {};
    STATIC_REQUIRE_FALSE(vakya::emit::tag_descriptor<my_custom_tag>::is_commutative);
}
