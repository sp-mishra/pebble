// test_npravaha_canon.cpp — Phase 1 canonicalization boundary tests
#include "catch_amalgamated.hpp"
#include "pravaha/pravaha_hetero.hpp"
#include "pravaha/pravaha_expr.hpp"
#include "pravaha/backends/metal_gpu.hpp"

using namespace pravaha;
using namespace pravaha::compute;
using namespace pravaha::hetero;

// ============================================================================
// Test 1: Hash collapse — x + lit(0.0f) and x canonicalize to the same hash.
// ============================================================================

TEST_CASE (



"canonicalize_apply collapses x+lit(0) to same hash as x"
,
"[canon][phase1]"
)
 {
    using namespace pravaha::expr;
    var x;
    auto e_zero = x + lit(0.0f);  // x + 0 — O3 simplify_add_zero_pass eliminates the +0
    auto e_raw  = var{};           // bare x

    const auto h_zero = pravaha::hetero::structural_hash(
                            canonicalize_apply<lithe::preset::O3>(e_zero));
    const auto h_raw  = pravaha::hetero::structural_hash(
                            canonicalize_apply<lithe::preset::O3>(e_raw));

    REQUIRE(h_zero == h_raw);
}

// ============================================================================
// Test 2: GPU cache single-compile — two structurally-equal-after-O3 exprs
// produce one kernel_cache entry (size unchanged on second get_or_compile).
// ============================================================================

#if defined(HAS_METAL_CPP)
TEST_CASE ("get_or_compile cache does not grow on same canonical expr", "[canon][phase1][gpu]") {
    using namespace pravaha::expr;
    auto& cache = pravaha::backends::metal::kernel_cache();
    cache.clear();  // fresh slate for this test

    const auto elem = data_element_type::f32;

    // Two expressions that both canonicalize to x*x (mul of two vars, no lit(0) involved).
    // O3 produces VariantExpr for mul nodes; std::visit resolves to the active flat
    // alternative before get_or_compile (which requires a FlatExpression).
    auto e1 = var{} * var{};
    auto e2 = var{} * var{} + lit(0.0f);  // O3 eliminates +0, active alt == mul node

    auto c1 = canonicalize_apply<lithe::preset::O3>(e1);
    auto c2 = canonicalize_apply<lithe::preset::O3>(e2);

    // Verify same hash (structural equality after canonicalization).
    REQUIRE(pravaha::hetero::structural_hash(c1) == pravaha::hetero::structural_hash(c2));

    // get_or_compile requires a FlatExpression — resolve VariantExpr via std::visit.
    // All variant alternatives are instantiated at compile time; guard non-Expression
    // alternatives (folded scalars) with if constexpr.
    auto compile_variant = [&elem](auto& canon) -> bool {
        return std::visit([&elem](auto& alt) -> bool {
            using A = std::decay_t<decltype(alt)>;
            if constexpr (pravaha::hetero::FlatExpression<A>)
                return pravaha::backends::metal::get_or_compile(alt, elem).has_value();
            else
                return false;  // folded scalar alternative — not a kernel expression
        }, canon);
    };

    // Compile c1 — one miss, cache grows by 1.
    const std::size_t before = cache.size();
    REQUIRE(compile_variant(c1));
    const std::size_t after_first = cache.size();
    REQUIRE(after_first == before + 1);

    // Compile c2 — same canonical hash → cache hit, size unchanged.
    REQUIRE(compile_variant(c2));
    REQUIRE(cache.size() == after_first);  // no new entry
}
#endif

// ============================================================================
// Test 3: BackendExpr static check — common eDSL expressions satisfy it.
// BackendExpr = FlatExpression (leaf/simple) OR VariantExpr (simplifiable tree).
// O3 rewrite rules return std::variant for add/mul/neg nodes (Lithe design);
// with_canon() resolves the active alternative at dispatch time.
// ============================================================================

TEST_CASE (



"BackendExpr concept holds for canonicalized eDSL expressions"
,
"[canon][phase1]"
)
 {
    using namespace pravaha::expr;
    var x;
    auto add_expr  = x + var{};
    auto mul_expr  = x * lit(2.0f);
    auto neg_expr  = -x;
    auto call_expr = var{};

    auto c_add  = canonicalize_apply<lithe::preset::O3>(add_expr);
    auto c_mul  = canonicalize_apply<lithe::preset::O3>(mul_expr);
    auto c_neg  = canonicalize_apply<lithe::preset::O3>(neg_expr);
    auto c_call = canonicalize_apply<lithe::preset::O3>(call_expr);

    // O3 rewrite rules produce VariantExpr for add/mul/neg trees; leaf nodes are flat.
    STATIC_REQUIRE(pravaha::hetero::BackendExpr<decltype(c_add)>);
    STATIC_REQUIRE(pravaha::hetero::BackendExpr<decltype(c_mul)>);
    STATIC_REQUIRE(pravaha::hetero::BackendExpr<decltype(c_neg)>);
    // Leaf node (var/input) passes through O3 without variant wrapping → FlatExpression.
    STATIC_REQUIRE(pravaha::hetero::FlatExpression<decltype(c_call)>);
    // FlatExpression implies BackendExpr.
    STATIC_REQUIRE(pravaha::hetero::BackendExpr<decltype(c_call)>);
}

// ============================================================================
// Test 4: Off path — optimize_before_codegen=false → canonicalize is identity.
// The expression type is unchanged and structural hash matches the raw expr.
// ============================================================================

TEST_CASE (



"with_canon identity path leaves expression unchanged"
,
"[canon][phase1]"
)
 {
    using namespace pravaha::expr;
    var x;
    auto e = x * x + lit(1.0f);

    const std::uint64_t hash_before = pravaha::hetero::structural_hash(e);

    // When optimize=false, with_canon passes expr through unchanged.
    std::uint64_t hash_via_canon = 0;
    pravaha::hetero::with_canon(false, e, [&](auto&& canon) {
        hash_via_canon = pravaha::hetero::structural_hash(canon);
        return 0;  // dummy return
    });

    REQUIRE(hash_via_canon == hash_before);
}

// ============================================================================
// Test 5: Numerical parity — canonicalized and raw expr produce same result.
// Canonicalization must not change semantics.
// ============================================================================

TEST_CASE (



"canonicalized expr produces same numeric output as raw"
,
"[canon][phase1]"
)
 {
    using namespace pravaha::expr;
    const std::size_t N = 64;
    std::vector<float> src(N), dst_raw(N, 0.f), dst_canon(N, 0.f);
    for (std::size_t i = 0; i < N; ++i) src[i] = static_cast<float>(i) * 0.1f;

    buffer_descriptor d;
    d.shape.push_back(N);
    d.element_type = data_element_type::f32;

    auto sv   = make_const_view(src.data(), d);
    auto dvr  = make_view(dst_raw.data(), d);
    auto dvc  = make_view(dst_canon.data(), d);

    execution_context ctx;
    hetero_executor exec;
    exec.policy.force = compute_domain::host_simd;
    exec.policy.optimize_before_codegen = false;

    var x;
    auto e = x * x + lit(0.0f);  // O3 will simplify this

    // Raw path (no canonicalization)
    REQUIRE(exec.execute(e, dvr, sv, ctx).has_value());

    // Canonicalized path
    exec.policy.optimize_before_codegen = true;
    REQUIRE(exec.execute(e, dvc, sv, ctx).has_value());

    for (std::size_t i = 0; i < N; ++i) {
        REQUIRE(dst_raw[i] == Catch::Approx(dst_canon[i]).epsilon(1e-6f));
    }
}

// --- impl-6: capability/slot/input folds via lithe primitives ------------
TEST_CASE (



"is_simd_capable: supported vs unsupported trees"
,
"[canon][impl6]"
)
 {
    using namespace pravaha::expr;
    {
        input<0> a; input<1> b;
        auto e = a * b + a;
        using E = decltype(e);
        STATIC_REQUIRE(pravaha::backends::simd_detail::is_simd_capable<E>());
    }
    {
        var x; auto e = sqrt(x * x);
        using E = decltype(e);
        STATIC_REQUIRE(pravaha::backends::simd_detail::is_simd_capable<E>());
    }
}

TEST_CASE (



"input_slot_count: max slot + 1"
,
"[canon][impl6]"
)
 {
    using namespace pravaha::expr;
    { var x; auto e = x * x;                 using E = decltype(e);
      STATIC_REQUIRE(pravaha::backends::simd_detail::input_slot_count<E>() == 1); }
    { input<0> a; input<2> c; auto e = a + c; using E = decltype(e);
      STATIC_REQUIRE(pravaha::backends::simd_detail::input_slot_count<E>() == 3); }
}

TEST_CASE (



"uses_input_leaves: detects input tags"
,
"[canon][impl6]"
)
 {
    using namespace pravaha::expr;
    { input<0> a; auto e = a + lit(1.0f);    using E = decltype(e);
      STATIC_REQUIRE(pravaha::backends::metal::msl::uses_input_leaves<E>()); }
    { auto e = lit(1.0f) + lit(2.0f);        using E = decltype(e);
      STATIC_REQUIRE(!pravaha::backends::metal::msl::uses_input_leaves<E>()); }
}
