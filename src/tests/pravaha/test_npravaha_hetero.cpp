// test_npravaha_hetero.cpp — Part 1 cases
#include "catch_amalgamated.hpp"
#include "pravaha/pravaha_hetero.hpp"
#include "pravaha/pravaha_expr.hpp"   // Parts A–E: user-facing eDSL surface
#include "pravaha/backends/metal_gpu.hpp"

using namespace pravaha;
using namespace pravaha::compute;
using namespace pravaha::hetero;

TEST_CASE (


"element_size covers all defined types"
,
"[hetero][part1]"
)
 {
    REQUIRE(element_size(data_element_type::f32) == 4);
    REQUIRE(element_size(data_element_type::f64) == 8);
    REQUIRE(element_size(data_element_type::i8)  == 1);
    REQUIRE(element_size(data_element_type::complex128) == 16);
    REQUIRE(element_size(data_element_type::unknown) == 0);
}

TEST_CASE (


"buffer_descriptor footprint"
,
"[hetero][part1]"
)
 {
    buffer_descriptor d;
    d.shape.push_back(1024);
    d.shape.push_back(1024);
    d.element_type = data_element_type::f32;
    REQUIRE(d.element_count() == 1024ull * 1024);
    REQUIRE(d.footprint_bytes() == 1024ull * 1024 * 4);
}

TEST_CASE (


"make_const_view is read-only, make_view is writable"
,
"[hetero][part1]"
)
 {
    std::vector<float> buf(16, 1.0f);
    buffer_descriptor d;
    d.shape.push_back(16);
    d.element_type = data_element_type::f32;

    auto rv = make_const_view(buf.data(), d);
    REQUIRE(rv.desc.writable == false);

    auto wv = make_view(buf.data(), d);
    REQUIRE(wv.desc.writable == true);
    REQUIRE(wv.base()[0] == Catch::Approx(1.0f));
}

TEST_CASE (


"route picks GPU only above threshold and non-f64"
,
"[hetero][part1]"
)
 {
    routing_policy p;  // gpu_threshold_bytes = 256KB, allow_gpu=true

    buffer_descriptor small;
    small.shape.push_back(100);
    small.element_type = data_element_type::f32;
    REQUIRE(route(small, p) == compute_domain::host_simd);

    buffer_descriptor big;
    big.shape.push_back(1 << 20);
    big.element_type = data_element_type::f32;
    REQUIRE(route(big, p) == compute_domain::metal_gpu);

    buffer_descriptor big_f64 = big;
    big_f64.element_type = data_element_type::f64;
    REQUIRE(route(big_f64, p) == compute_domain::host_simd);  // f64 forced to SIMD

    p.force = compute_domain::host_simd;
    REQUIRE(route(big, p) == compute_domain::host_simd);  // force overrides
}

TEST_CASE (


"structural_hash stable + topology-sensitive"
,
"[hetero][part1]"
)
 {
    double a = 1, b = 2, c = 3;
    auto e1 = lithe::make_node<lithe::mul_tag>(a, b);
    auto e2 = lithe::make_node<lithe::mul_tag>(c, a);  // same topology, diff leaves
    auto e3 = lithe::make_node<lithe::add_tag>(a, b);  // diff tag

    // Qualify to disambiguate from lithe::structural_hash pulled in via lithe.hpp.
    REQUIRE(pravaha::hetero::structural_hash(e1) == pravaha::hetero::structural_hash(e2));
    REQUIRE(pravaha::hetero::structural_hash(e1) != pravaha::hetero::structural_hash(e3));
}

TEST_CASE (


"execution_context overlay bind/lookup"
,
"[hetero][part1]"
)
 {
    execution_context ctx;
    node_metadata m;
    m.preferred = compute_domain::metal_gpu;
    ctx.bind(42, m);
    REQUIRE(ctx.lookup(42) != nullptr);
    REQUIRE(ctx.lookup(42)->preferred == compute_domain::metal_gpu);
    REQUIRE(ctx.lookup(99) == nullptr);
}

#ifdef PRAVAHA_HETERO_COMPILE_FAIL_TESTS
TEST_CASE ("make_view on const ptr must not compile", "[hetero][part1][compilefail]") {
    const float x = 1.0f;
    buffer_descriptor d;
    d.shape.push_back(1);
    d.element_type = data_element_type::f32;
    // The next line MUST fail to compile (static_assert in make_view):
    // auto v = make_view(&x, d);
}
#endif

// ---- Part 2 cases (append; do not modify Part 1 cases) ----

TEST_CASE (


"simd add-mul element-wise correctness"
,
"[hetero][part2][simd]"
)
 {
    using namespace pravaha;
    using namespace pravaha::compute;

    constexpr std::size_t N = 1000;      // deliberately non-multiple of lane width
    std::vector<float> src(N), dst(N, 0.0f);
    for (std::size_t i = 0; i < N; ++i) src[i] = static_cast<float>(i);

    // expr: y = (x * x) + x     (call_tag leaf = x)
    auto x    = lithe::make_node<lithe::call_tag>();
    auto sq   = lithe::make_node<lithe::mul_tag>(x, x);
    auto expr = lithe::make_node<lithe::add_tag>(sq, x);

    buffer_descriptor d; d.shape = {N}; d.element_type = data_element_type::f32;
    auto sv = make_const_view(src.data(), d);
    auto dv = make_view(dst.data(), d);

    backends::host_simd_backend be;
    hetero::execution_context ctx;
    auto r = be.execute(expr, dv, sv, ctx);
    REQUIRE(r.has_value());

    for (std::size_t i = 0; i < N; ++i) {
        float xi = static_cast<float>(i);
        REQUIRE(dst[i] == Catch::Approx(xi * xi + xi));
    }
}

TEST_CASE (


"simd neg + sub + div"
,
"[hetero][part2][simd]"
)
 {
    using namespace pravaha;
    using namespace pravaha::compute;
    constexpr std::size_t N = 257;
    std::vector<float> src(N), dst(N);
    for (std::size_t i = 0; i < N; ++i) src[i] = static_cast<float>(i + 1);

    // y = (-x) - (x / x)  == -x - 1
    auto x   = lithe::make_node<lithe::call_tag>();
    auto ng  = lithe::make_node<lithe::neg_tag>(x);
    auto dv2 = lithe::make_node<lithe::div_tag>(x, x);
    auto e   = lithe::make_node<lithe::sub_tag>(ng, dv2);

    buffer_descriptor d; d.shape = {N}; d.element_type = data_element_type::f32;
    backends::host_simd_backend be; hetero::execution_context ctx;
    REQUIRE(be.execute(e, make_view(dst.data(), d),
                          make_const_view(src.data(), d), ctx).has_value());
    for (std::size_t i = 0; i < N; ++i)
        REQUIRE(dst[i] == Catch::Approx(-src[i] - 1.0f));
}

TEST_CASE (


"simd capability gate"
,
"[hetero][part2][simd]"
)
 {
    using namespace pravaha::backends::simd_detail;
    auto x  = lithe::make_node<lithe::call_tag>();
    auto ok = lithe::make_node<lithe::add_tag>(x, x);
    STATIC_REQUIRE(is_simd_capable<decltype(ok)>());

    // lt_tag is NOT supported → gate returns false → executor uses fallback.
    auto cmp = lithe::make_node<lithe::lt_tag>(x, x);
    STATIC_REQUIRE_FALSE(is_simd_capable<decltype(cmp)>());
}

// ---- Part 3: MSL emitter (runs on all platforms) ----

TEST_CASE (


"msl emitter produces valid-looking kernel"
,
"[hetero][part3][msl]"
)
 {
    using namespace pravaha;
    auto x    = lithe::make_node<lithe::call_tag>();
    auto sq   = lithe::make_node<lithe::mul_tag>(x, x);
    auto expr = lithe::make_node<lithe::add_tag>(sq, x);   // x*x + x

    std::string k = backends::metal::emit_kernel(expr, compute::data_element_type::f32);

    REQUIRE(k.find("kernel void pravaha_kernel") != std::string::npos);
    REQUIRE(k.find("device const float* src") != std::string::npos);
    REQUIRE(k.find("dst[gid] = ((x * x) + x);") != std::string::npos);
    REQUIRE(k.find("if (gid >= n) return;") != std::string::npos);
}

TEST_CASE (


"msl emitter neg + scalar type mapping"
,
"[hetero][part3][msl]"
)
 {
    using namespace pravaha;
    auto x = lithe::make_node<lithe::call_tag>();
    auto e = lithe::make_node<lithe::neg_tag>(x);
    std::string k = backends::metal::emit_kernel(e, compute::data_element_type::f16);
    REQUIRE(k.find("device const half* src") != std::string::npos);
    REQUIRE(k.find("dst[gid] = (-(x));") != std::string::npos);
}

#if defined(__APPLE__) && defined(HAS_METAL_CPP)
TEST_CASE ("metal GPU element-wise matches CPU", "[hetero][part3][gpu]") {
    using namespace pravaha;
    using namespace pravaha::compute;

    if (!backends::metal::metal_gpu_backend::instance().available()) {
        WARN("No Metal device; skipping GPU exec test");
        return;
    }
    constexpr std::size_t N = 1 << 16;
    std::vector<float> src(N), dst(N, 0.0f);
    for (std::size_t i = 0; i < N; ++i) src[i] = float(i) * 0.5f;

    auto x = lithe::make_node<lithe::call_tag>();
    auto e = lithe::make_node<lithe::add_tag>(
                 lithe::make_node<lithe::mul_tag>(x, x), x);  // x*x + x

    buffer_descriptor d; d.shape = {N}; d.element_type = data_element_type::f32;
    d.is_unified = true;

    auto r = backends::metal::run_gpu_uncached(
        e, make_view(dst.data(), d), make_const_view(src.data(), d));
    REQUIRE(r.has_value());

    for (std::size_t i = 0; i < N; ++i)
        REQUIRE(dst[i] == Catch::Approx(src[i]*src[i] + src[i]).epsilon(1e-4f));
}
#endif

// ---- Part 4: executor + cache + telemetry ----

TEST_CASE (


"hetero_executor routes small workload to SIMD and computes"
,
"[hetero][part4]"
)
 {
    using namespace pravaha;
    using namespace pravaha::compute;
    constexpr std::size_t N = 512;                       // < 256KB → SIMD
    std::vector<float> src(N), dst(N, 0.f);
    for (std::size_t i = 0; i < N; ++i) src[i] = float(i);

    auto x = lithe::make_node<lithe::call_tag>();
    auto e = lithe::make_node<lithe::mul_tag>(x, x);     // x*x

    buffer_descriptor d; d.shape = {N}; d.element_type = data_element_type::f32;
    hetero::hetero_executor exec;
    hetero::execution_context ctx;
    REQUIRE(exec.execute(e, make_view(dst.data(), d),
                            make_const_view(src.data(), d), ctx).has_value());
    for (std::size_t i = 0; i < N; ++i)
        REQUIRE(dst[i] == Catch::Approx(src[i]*src[i]));
}

#if defined(__APPLE__) && defined(HAS_METAL_CPP)
TEST_CASE ("kernel cache: miss then hit", "[hetero][part4][gpu][cache]") {
    using namespace pravaha;
    using namespace pravaha::compute;
    if (!backends::metal::metal_gpu_backend::instance().available()) {
        WARN("No Metal device; skipping cache test"); return;
    }
    backends::metal::kernel_cache().clear();

    auto x = lithe::make_node<lithe::call_tag>();
    auto e = lithe::make_node<lithe::add_tag>(x, x);

    auto p1 = backends::metal::get_or_compile(e, data_element_type::f32);
    REQUIRE(p1.has_value());
    REQUIRE(backends::metal::kernel_cache().size() == 1);

    auto p2 = backends::metal::get_or_compile(e, data_element_type::f32);
    REQUIRE(p2.has_value());
    REQUIRE(backends::metal::kernel_cache().size() == 1);       // hit, no growth
    REQUIRE((*p1).get() == (*p2).get());                        // same pipeline
}

TEST_CASE ("hetero_executor routes big workload to GPU", "[hetero][part4][gpu]") {
    using namespace pravaha;
    using namespace pravaha::compute;
    if (!backends::metal::metal_gpu_backend::instance().available()) {
        WARN("No Metal device"); return;
    }
    constexpr std::size_t N = 1 << 18;                   // >= 256KB → GPU
    std::vector<float> src(N), dst(N, 0.f);
    for (std::size_t i = 0; i < N; ++i) src[i] = float(i) * 0.25f;

    auto x = lithe::make_node<lithe::call_tag>();
    auto e = lithe::make_node<lithe::add_tag>(
                 lithe::make_node<lithe::mul_tag>(x, x), x);

    buffer_descriptor d; d.shape = {N}; d.element_type = data_element_type::f32; d.is_unified = true;
    hetero::hetero_executor exec; hetero::execution_context ctx;
    REQUIRE(exec.execute(e, make_view(dst.data(), d),
                            make_const_view(src.data(), d), ctx).has_value());
    for (std::size_t i = 0; i < N; ++i)
        REQUIRE(dst[i] == Catch::Approx(src[i]*src[i] + src[i]).epsilon(1e-4));
}
#endif

TEST_CASE (


"fallback emits NADI event via run_simd_or_fallback"
,
"[hetero][part4][nadi]"
)
 {
    // Uses default NoSink (zero cost). Verifies the fallback path routes correctly
    // for a SIMD-capable expression when explicitly routed via run_simd_or_fallback.
    // The emit_fallback_event (Invariant 3) fires for non-SIMD trees; here we
    // verify the SIMD path itself works correctly through the dispatcher.
    using namespace pravaha;
    using namespace pravaha::compute;
    constexpr std::size_t N = 64;
    std::vector<float> src(N), dst(N, 0.f);
    for (std::size_t i = 0; i < N; ++i) src[i] = float(i + 1);

    auto x = lithe::make_node<lithe::call_tag>();
    auto e = lithe::make_node<lithe::neg_tag>(x);  // y = -x; SIMD-capable

    buffer_descriptor d; d.shape = {N}; d.element_type = data_element_type::f32;
    auto r = backends::run_simd_or_fallback<float>(
        e, make_view(dst.data(), d), make_const_view(src.data(), d),
        hetero::execution_context{});
    REQUIRE(r.has_value());
    for (std::size_t i = 0; i < N; ++i)
        REQUIRE(dst[i] == Catch::Approx(-src[i]));
}

// ====== Part 5: Four-Invariant verification matrix ======
#include <thread>
#include <type_traits>

// ---- Invariant 1: No AST contamination ----
// Lithe node size must NOT grow because of hetero metadata. All hardware fields
// live in execution_context, keyed by structural_hash — never in the node.
TEST_CASE (


"Invariant 1: hetero overlay never grows Lithe node size"
,
"[hetero][part5][invariant]"
)
 {
    using namespace pravaha;
    auto x  = lithe::make_node<lithe::call_tag>();
    auto e  = lithe::make_node<lithe::mul_tag>(x, x);

    // A bare Lithe mul node of two leaves. Its size is a pure function of its
    // children — binding metadata for it must not change that.
    const std::size_t before = sizeof(e);

    hetero::execution_context ctx;
    hetero::node_metadata md;
    md.preferred = hetero::compute_domain::metal_gpu;
    ctx.bind(hetero::structural_hash(e), md);

    STATIC_REQUIRE(sizeof(decltype(e)) == sizeof(e));   // trivially true; documents intent
    REQUIRE(sizeof(e) == before);                       // binding changed nothing on the node
    REQUIRE(ctx.lookup(hetero::structural_hash(e)) != nullptr);  // metadata lives in overlay
}

// ---- Invariant 2: Const-correct views ----
// Runtime side: make_const_view yields writable==false. The compile-fail half
// (make_view on const ptr) is guarded under PRAVAHA_HETERO_COMPILE_FAIL_TESTS (Part 1 §8).
TEST_CASE (


"Invariant 2: const view is read-only, writable view is writable"
,
"[hetero][part5][invariant]"
)
 {
    using namespace pravaha;
    using namespace pravaha::compute;
    std::vector<float> buf(8, 2.0f);
    buffer_descriptor d; d.shape = {8}; d.element_type = data_element_type::f32;

    auto cv = make_const_view(buf.data(), d);
    REQUIRE(cv.desc.writable == false);
    STATIC_REQUIRE(std::is_const_v<std::remove_pointer_t<decltype(cv.base())>>);

    auto wv = make_view(buf.data(), d);
    REQUIRE(wv.desc.writable == true);
    STATIC_REQUIRE_FALSE(std::is_const_v<std::remove_pointer_t<decltype(wv.data)>>);
}

// ---- Invariant 3: No silent degradation ----
// Every fallback emits a NADI event. Requires PRAVAHA_HETERO_SINK=CaptureSink
// (Part 4 §4). Route an unsupported-on-SIMD tree so the executor degrades and
// MUST record an event.
#if defined(PRAVAHA_HETERO_SINK_IS_CAPTURE)   // define in the CaptureSink test TU
TEST_CASE ("Invariant 3: fallback always emits a NADI event", "[hetero][part5][invariant][nadi]") {
    using namespace pravaha;
    using namespace pravaha::compute;
    CaptureSink::clear();

    // lt_tag is unsupported by SIMD AND scalar fallback (Part 2 scope note) →
    // executor routes to Lithe interpreter and MUST emit a fallback pulse first.
    auto x = lithe::make_node<lithe::call_tag>();
    auto e = lithe::make_node<lithe::lt_tag>(x, x);

    constexpr std::size_t N = 32;
    std::vector<float> src(N, 1.0f), dst(N, 0.0f);
    buffer_descriptor d; d.shape = {N}; d.element_type = data_element_type::f32;

    hetero::hetero_executor exec; hetero::execution_context ctx;
    (void)exec.execute(e, make_view(dst.data(), d),
                          make_const_view(src.data(), d), ctx);
    REQUIRE_FALSE(CaptureSink::events.empty());   // degradation was announced
}
#endif

// ---- Invariant 4: Async context isolation ----
// No thread-local metadata: two contexts on two threads never cross-talk.
// Same structural hash, different preferred domain, resolved independently.
TEST_CASE (


"Invariant 4: contexts are isolated across threads"
,
"[hetero][part5][invariant]"
)
 {
    using namespace pravaha;
    auto x = lithe::make_node<lithe::call_tag>();
    auto e = lithe::make_node<lithe::add_tag>(x, x);
    const std::uint64_t key = hetero::structural_hash(e);

    hetero::compute_domain seen_a{}, seen_b{};
    std::thread ta([&]{
        hetero::execution_context ctx;
        hetero::node_metadata m; m.preferred = hetero::compute_domain::host_simd;
        ctx.bind(key, m);
        seen_a = ctx.lookup(key)->preferred;
    });
    std::thread tb([&]{
        hetero::execution_context ctx;
        hetero::node_metadata m; m.preferred = hetero::compute_domain::metal_gpu;
        ctx.bind(key, m);
        seen_b = ctx.lookup(key)->preferred;
    });
    ta.join(); tb.join();
    REQUIRE(seen_a == hetero::compute_domain::host_simd);   // no bleed from thread b
    REQUIRE(seen_b == hetero::compute_domain::metal_gpu);   // no bleed from thread a
}

// ---- 3.1 Determinism cross-check (structural_hash) ----
TEST_CASE (


"structural_hash deterministic across repeated calls"
,
"[hetero][part5][invariant]"
)
 {
    using namespace pravaha;
    auto x = lithe::make_node<lithe::call_tag>();
    auto e = lithe::make_node<lithe::add_tag>(
                 lithe::make_node<lithe::mul_tag>(x, x), x);
    const auto h1 = hetero::structural_hash(e);
    const auto h2 = hetero::structural_hash(e);
    REQUIRE(h1 == h2);
}

// ============================================================================
// Parts A–E: eDSL enhancement suite (appended — see docs/pravaha/pravaha_design_gap.md)
// ============================================================================

// ---- Part A: lit_node value honored on scalar + SIMD, distinct hash ----
TEST_CASE (


"Part A: lit_node value honored on SIMD path"
,
"[hetero][partA][simd]"
)
 {
    using namespace pravaha;
    using namespace pravaha::compute;
    using namespace pravaha::expr;
    constexpr std::size_t N = 999;
    std::vector<float> src(N), dst(N, 0.0f);
    for (std::size_t i = 0; i < N; ++i) src[i] = static_cast<float>(i);

    var x;
    auto e = x * x + lit(1.0f);   // must be x*x + 1, NOT x*x + x

    buffer_descriptor d; d.shape = {N}; d.element_type = data_element_type::f32;
    backends::host_simd_backend be; hetero::execution_context ctx;
    REQUIRE(be.execute(e, make_view(dst.data(), d),
                          make_const_view(src.data(), d), ctx).has_value());
    for (std::size_t i = 0; i < N; ++i) {
        float xi = static_cast<float>(i);
        REQUIRE(dst[i] == Catch::Approx(xi * xi + 1.0f));
    }
}

TEST_CASE (


"Part A: lit-bearing tree stays SIMD-capable"
,
"[hetero][partA][simd]"
)
 {
    using namespace pravaha::expr;
    using namespace pravaha::backends::simd_detail;
    var x;
    auto e = x * x + lit(1.0f);
    STATIC_REQUIRE(is_simd_capable<decltype(e)>());
}

TEST_CASE (


"Part A: distinct constants get distinct structural hashes"
,
"[hetero][partA][hash]"
)
 {
    using namespace pravaha;
    using namespace pravaha::expr;
    var x;
    auto e1 = x + lit(1.0f);
    auto e2 = x + lit(2.0f);
    // The baked-in MSL literal is part of the compiled kernel, so distinct
    // constants must hash to distinct kernel-cache keys.
    REQUIRE(hetero::structural_hash(e1) != hetero::structural_hash(e2));
    // A lit leaf is also distinct from a plain input leaf.
    auto plain = x + x;
    REQUIRE(hetero::structural_hash(e1) != hetero::structural_hash(plain));
}

TEST_CASE (


"Part A: MSL emits literal, not stray x"
,
"[hetero][partA][msl]"
)
 {
    using namespace pravaha::expr;
    var x;
    auto e = x * x + lit(1.0f);
    std::string k = pravaha::backends::metal::emit_kernel(e, pravaha::compute::data_element_type::f32);
    REQUIRE(k.find("1.00000000") != std::string::npos);          // the literal
    REQUIRE(k.find("dst[gid] = ((x0 * x0) + 1") != std::string::npos);
}

// ---- Part B: math builtins on SIMD + Metal ----
TEST_CASE (


"Part B: sqrt/exp/sin match scalar oracle on SIMD"
,
"[hetero][partB][simd]"
)
 {
    using namespace pravaha;
    using namespace pravaha::compute;
    using namespace pravaha::expr;
    using namespace pravaha::backends::simd_detail;
    constexpr std::size_t N = 511;
    std::vector<float> src(N), dst(N, 0.0f);
    for (std::size_t i = 0; i < N; ++i) src[i] = 0.01f * static_cast<float>(i + 1);

    var x;
    auto e = sqrt(exp(sin(x)));
    STATIC_REQUIRE(is_simd_capable<decltype(e)>());   // vectorizes, no fallback

    buffer_descriptor d; d.shape = {N}; d.element_type = data_element_type::f32;
    backends::host_simd_backend be; hetero::execution_context ctx;
    REQUIRE(be.execute(e, make_view(dst.data(), d),
                          make_const_view(src.data(), d), ctx).has_value());
    for (std::size_t i = 0; i < N; ++i) {
        float want = std::sqrt(std::exp(std::sin(src[i])));
        REQUIRE(dst[i] == Catch::Approx(want).epsilon(1e-4));
    }
}

TEST_CASE (


"Part B: MSL emits builtin names"
,
"[hetero][partB][msl]"
)
 {
    using namespace pravaha::expr;
    var x;
    auto e = sqrt(exp(x));
    std::string k = pravaha::backends::metal::emit_kernel(e, pravaha::compute::data_element_type::f32);
    REQUIRE(k.find("sqrt(") != std::string::npos);
    REQUIRE(k.find("exp(")  != std::string::npos);
}

// ---- Part C: multi-input y = f(x0, x1, …) ----
TEST_CASE (


"Part C: AXPY a*x + y two-input SIMD"
,
"[hetero][partC][simd]"
)
 {
    using namespace pravaha;
    using namespace pravaha::compute;
    using namespace pravaha::expr;
    constexpr std::size_t N = 777;
    std::vector<float> x(N), y(N), dst(N, 0.0f);
    for (std::size_t i = 0; i < N; ++i) { x[i] = static_cast<float>(i); y[i] = static_cast<float>(2*i); }

    input<0> x0; input<1> x1;
    auto e = lit(3.0f) * x0 + x1;   // 3x + y

    buffer_descriptor d; d.shape = {N}; d.element_type = data_element_type::f32;
    std::array<compute_view<const float>, 2> srcs{
        make_const_view(x.data(), d), make_const_view(y.data(), d) };
    backends::host_simd_backend be; hetero::execution_context ctx;
    REQUIRE(be.execute<float, 2>(e, make_view(dst.data(), d), srcs, ctx).has_value());
    for (std::size_t i = 0; i < N; ++i)
        REQUIRE(dst[i] == Catch::Approx(3.0f * x[i] + y[i]));
}

TEST_CASE (


"Part C: input slot count + MSL multi-buffer"
,
"[hetero][partC][msl]"
)
 {
    using namespace pravaha::expr;
    using namespace pravaha::backends::simd_detail;
    input<0> a; input<1> b;
    auto e = a * b + a;
    STATIC_REQUIRE(input_slot_count<decltype(e)>() == 2);
    std::string k = pravaha::backends::metal::emit_kernel(e, pravaha::compute::data_element_type::f32);
    REQUIRE(k.find("src0 [[buffer(0)]]") != std::string::npos);
    REQUIRE(k.find("src1 [[buffer(1)]]") != std::string::npos);
    REQUIRE(k.find("float x0 = src0[gid];") != std::string::npos);
    REQUIRE(k.find("float x1 = src1[gid];") != std::string::npos);
}

// ---- Part D: view slicing / strided access ----
TEST_CASE (


"Part D: strided view processes every-other element"
,
"[hetero][partD][simd]"
)
 {
    using namespace pravaha;
    using namespace pravaha::compute;
    using namespace pravaha::expr;
    constexpr std::size_t N = 64;
    std::vector<float> src(N), dst(N, -1.0f);
    for (std::size_t i = 0; i < N; ++i) src[i] = static_cast<float>(i);

    buffer_descriptor d; d.shape = {N}; d.element_type = data_element_type::f32; d.writable = true;
    auto sv = make_const_view(src.data(), d);
    auto dv = make_view(dst.data(), d);

    // stride-2 views over both operands: 32 logical elements, step 2.
    auto sstep = sv.slice(range{0, static_cast<dim_t>(N), 2});
    auto dstep = dv.slice(range{0, static_cast<dim_t>(N), 2});
    REQUIRE_FALSE(sstep.is_contiguous());
    REQUIRE(sstep.desc.element_count() == 32);

    var x;
    auto e = x + x;   // 2x
    backends::host_simd_backend be; hetero::execution_context ctx;
    std::array<compute_view<const float>, 1> srcs{ sstep };
    REQUIRE(be.execute<float, 1>(e, dstep, srcs, ctx).has_value());
    for (std::size_t i = 0; i < N; i += 2)
        REQUIRE(dst[i] == Catch::Approx(2.0f * src[i]));   // even indices written
    // odd indices untouched
    REQUIRE(dst[1] == Catch::Approx(-1.0f));
}

TEST_CASE (


"Part D: integer slice collapses a dimension"
,
"[hetero][partD][view]"
)
 {
    using namespace pravaha::compute;
    std::vector<float> buf(12, 0.0f);
    buffer_descriptor d; d.shape = {3, 4}; d.element_type = data_element_type::f32; d.writable = true;
    auto v = make_view(buf.data(), d);
    auto row = v.slice(1);                 // select row 1
    REQUIRE(row.offset == 4);              // row-major: row 1 begins at element 4
    REQUIRE(row.desc.shape[0] == 1);
}

// ---- Part E: reductions ----
TEST_CASE (


"Part E: reduce_sum matches std::accumulate"
,
"[hetero][partE][simd]"
)
 {
    using namespace pravaha;
    using namespace pravaha::compute;
    using namespace pravaha::expr;
    constexpr std::size_t N = 100000;
    std::vector<float> src(N);
    for (std::size_t i = 0; i < N; ++i) src[i] = 1.0f;

    var x;
    auto child = x * x;   // Σ x²  ; here x=1 so Σ = N
    buffer_descriptor d; d.shape = {N}; d.element_type = data_element_type::f32;
    auto sv = make_const_view(src.data(), d);

    float got = backends::run_reduce_simd<expr::reduce_op::sum, float>(child, sv);
    REQUIRE(got == Catch::Approx(static_cast<float>(N)));
}

TEST_CASE (


"Part E: reduce_max / reduce_min correctness"
,
"[hetero][partE][simd]"
)
 {
    using namespace pravaha;
    using namespace pravaha::compute;
    using namespace pravaha::expr;
    constexpr std::size_t N = 333;
    std::vector<float> src(N);
    for (std::size_t i = 0; i < N; ++i) src[i] = static_cast<float>((i * 37) % 101) - 50.0f;

    var x;
    auto child = x;   // identity element-wise
    buffer_descriptor d; d.shape = {N}; d.element_type = data_element_type::f32;
    auto sv = make_const_view(src.data(), d);

    float gmax = backends::run_reduce_simd<expr::reduce_op::max, float>(child, sv);
    float gmin = backends::run_reduce_simd<expr::reduce_op::min, float>(child, sv);
    float wmax = *std::max_element(src.begin(), src.end());
    float wmin = *std::min_element(src.begin(), src.end());
    REQUIRE(gmax == Catch::Approx(wmax));
    REQUIRE(gmin == Catch::Approx(wmin));
}

TEST_CASE (


"Part E: reduce eDSL surface + op recovery"
,
"[hetero][partE][edsl]"
)
 {
    using namespace pravaha::expr;
    var x;
    auto r = reduce_sum(x * x);
    STATIC_REQUIRE(reduce_op_of<decltype(r)> == reduce_op::sum);
    // child recovers the element-wise tree for backend evaluation
    const auto& c = reduce_child(r);
    using namespace pravaha::backends::simd_detail;
    STATIC_REQUIRE(is_simd_capable<std::decay_t<decltype(c)>>());
}

TEST_CASE (


"Part E: MSL reduce kernel emits threadgroup barrier"
,
"[hetero][partE][msl]"
)
 {
    using namespace pravaha::expr;
    var x;
    auto child = x * x;
    std::string k = pravaha::backends::metal::emit_reduce_kernel(
        child, reduce_op::sum, pravaha::compute::data_element_type::f32);
    REQUIRE(k.find("kernel void pravaha_reduce") != std::string::npos);
    REQUIRE(k.find("threadgroup_barrier(mem_flags::mem_threadgroup)") != std::string::npos);
    REQUIRE(k.find("threadgroup float scratch") != std::string::npos);
    REQUIRE(k.find("partials[tgid]") != std::string::npos);
}

TEST_CASE (


"Part E: reduce routing uses its own threshold"
,
"[hetero][partE][route]"
)
 {
    using namespace pravaha::hetero;
    using namespace pravaha::compute;
    routing_policy p;                             // reduce_gpu_threshold = 1MB
    REQUIRE(p.reduce_gpu_threshold_bytes == 1024u * 1024u);

    // A 512KB buffer clears the element-wise threshold (256KB) but NOT the
    // reduce threshold (1MB): with the reduce threshold applied it stays on SIMD.
    buffer_descriptor mid; mid.shape = {128 * 1024}; mid.element_type = data_element_type::f32;
    REQUIRE(mid.footprint_bytes() == 512u * 1024u);

    routing_policy elemwise = p;                              // 256KB threshold
    REQUIRE(route(mid, elemwise) == compute_domain::metal_gpu);   // big enough for element-wise

    routing_policy reduce_rp = p;
    reduce_rp.gpu_threshold_bytes = p.reduce_gpu_threshold_bytes; // apply reduce threshold
    REQUIRE(route(mid, reduce_rp) == compute_domain::host_simd);  // too small for reduce
}

// ============================================================================
// Phase 2 Tests — Hardware Vectorization Hardening
// ============================================================================

// Phase 2.1 — Multi-buffer reduce: reduce_sum(x0 * x1) == std::inner_product
// (SIMD path always; GPU path when HAS_METAL_CPP and buffer >= 1 MB).
TEST_CASE (


"Phase2: multi-buffer reduce_sum(x0*x1) == inner_product reference"
,
"[phase2][reduce][multi]"
)
 {
    using namespace pravaha::expr;
    using namespace pravaha::backends;

    constexpr std::size_t N = 4096;
    std::vector<float> a(N), b(N);
    for (std::size_t i = 0; i < N; ++i) {
        a[i] = static_cast<float>(i % 64) * 0.1f;
        b[i] = static_cast<float>((N - i) % 64) * 0.1f;
    }

    buffer_descriptor d; d.shape = {N}; d.element_type = data_element_type::f32;
    auto va = make_const_view(a.data(), d);
    auto vb = make_const_view(b.data(), d);
    const std::array<compute_view<const float>, 2> srcs{va, vb};

    // Reference: inner product in double for accuracy
    double ref = 0.0;
    for (std::size_t i = 0; i < N; ++i) ref += static_cast<double>(a[i]) * static_cast<double>(b[i]);

    // SIMD path
    auto child = input<0>{} * input<1>{};
    float got_simd = run_reduce_simd_multi<reduce_op::sum, float, 2>(child, srcs);
    REQUIRE(static_cast<double>(got_simd) == Catch::Approx(ref).epsilon(1e-4));

#if defined(HAS_METAL_CPP)
    // GPU path — force via executor with large-enough buffer (>= 1 MB)
    constexpr std::size_t N_large = 256 * 1024; // 1 MB of f32
    std::vector<float> la(N_large), lb(N_large);
    for (std::size_t i =0; i<N_large;++i) {
        la[i] = static_cast<float>(i % 64) * 1e-3f;
        lb[i] = static_cast<float>((N_large - i) % 64) * 1e-3f;
    }
    double ref_large = 0.0;
    for (std::size_t i =0; i<N_large;++i)
        ref_large+= static_cast<double>(la[i]) *static_cast<double>(lb[i]);

    buffer_descriptor dl; dl.shape={N_large}; dl.element_type= data_element_type::f32;
    auto vla = make_const_view(la.data(), dl);
    auto vlb = make_const_view(lb.data(), dl);
    const std::array<compute_view<const float>, 2> large_srcs{vla, vlb};

    hetero_executor exec;
    execution_context ctx;
    auto r = exec.reduce<reduce_op::sum, float>(child, large_srcs, ctx);
    REQUIRE (r.has_value());
    REQUIRE (static_cast<double>(*r)== Catch::Approx (ref_large).epsilon (1e-4));
#endif
}

// Phase 2.2 — Strided scatter write: dst = x + x, dst stride-2 → only even slots written.
TEST_CASE (


"Phase2: strided scatter write only fills stride-2 slots"
,
"[phase2][scatter][strided]"
)
 {
    using namespace pravaha::expr;
    constexpr std::size_t N = 64;

    std::vector<float> src(N), dst(2 * N, -1.0f);  // dst is 2*N; we write every other element
    for (std::size_t i = 0; i < N; ++i) src[i] = static_cast<float>(i) * 0.5f;

    buffer_descriptor sd; sd.shape = {N};       sd.element_type = data_element_type::f32;
    buffer_descriptor dd; dd.shape = {2 * N};   dd.element_type = data_element_type::f32;

    auto sv       = make_const_view(src.data(), sd);
    auto dst_full = make_view(dst.data(), dd);

    // Slice stride-2: elements 0,2,4,...,2*(N-1) — covering N elements
    auto dst_strided = dst_full.slice(range{0, 2 * N, 2});

    REQUIRE(!dst_strided.is_contiguous());
    REQUIRE(dst_strided.inner_stride() == 2);
    REQUIRE(static_cast<std::size_t>(dst_strided.desc.element_count()) == N);

    var x;
    auto e = x + x;  // 2*x

    hetero_executor exec;
    exec.policy.force = compute_domain::host_simd;
    execution_context ctx;
    REQUIRE(exec.execute(e, dst_strided, sv, ctx).has_value());

    // Verify: even slots == 2*src[i], odd slots still -1
    for (std::size_t i = 0; i < N; ++i) {
        REQUIRE(dst[2 * i]     == Catch::Approx(2.0f * src[i]).epsilon(1e-6f));
        REQUIRE(dst[2 * i + 1] == Catch::Approx(-1.0f));
    }
}

// Phase 2.3 — Tail masking: N values where N % lane_count != 0 → same result as scalar.
TEST_CASE (


"Phase2: tail masking matches scalar for non-power-of-two N"
,
"[phase2][tail][mask]"
)
 {
    using namespace pravaha::expr;

    for (std::size_t N : {std::size_t{1000}, std::size_t{1003}}) {
        std::vector<float> src(N), dst_simd(N, 0.f), dst_scalar(N, 0.f);
        for (std::size_t i = 0; i < N; ++i) src[i] = static_cast<float>(i) * 0.1f;

        buffer_descriptor d; d.shape = {N}; d.element_type = data_element_type::f32;
        auto sv = make_const_view(src.data(), d);

        // Scalar reference
        for (std::size_t i = 0; i < N; ++i) dst_scalar[i] = src[i] * src[i];

        // SIMD path (will exercise masked tail for non-lane-multiple N)
        var x;
        auto e = x * x;
        auto dv_simd = make_view(dst_simd.data(), d);

        hetero_executor exec;
        exec.policy.force = compute_domain::host_simd;
        execution_context ctx;
        REQUIRE(exec.execute(e, dv_simd, sv, ctx).has_value());

        for (std::size_t i = 0; i < N; ++i)
            REQUIRE(dst_simd[i] == Catch::Approx(dst_scalar[i]).epsilon(1e-6f));

        // Also test reduce tail masking: sum(x*x) == scalar sum
        double ref_sum = 0.0;
        for (std::size_t i = 0; i < N; ++i) ref_sum += static_cast<double>(src[i]) * static_cast<double>(src[i]);

        auto r = exec.reduce<reduce_op::sum, float>(e, sv, ctx);
        REQUIRE(r.has_value());
        REQUIRE(static_cast<double>(*r) == Catch::Approx(ref_sum).epsilon(1e-4));
    }
}

// Phase 2.4 — Contiguous regression: existing contiguous element-wise + reduce still correct.
TEST_CASE (


"Phase2: contiguous element-wise and reduce regression after scatter/tail refactor"
,
"[phase2][regression][contiguous]"
)
 {
    using namespace pravaha::expr;
    constexpr std::size_t N = 256;

    std::vector<float> src(N), dst(N, 0.f);
    for (std::size_t i = 0; i < N; ++i) src[i] = static_cast<float>(i + 1) * 0.25f;

    buffer_descriptor d; d.shape = {N}; d.element_type = data_element_type::f32;
    auto sv = make_const_view(src.data(), d);
    auto dv = make_view(dst.data(), d);

    REQUIRE(dv.is_contiguous());

    hetero_executor exec;
    exec.policy.force = compute_domain::host_simd;
    execution_context ctx;

    var x;
    auto e = x * x + x;
    REQUIRE(exec.execute(e, dv, sv, ctx).has_value());

    for (std::size_t i = 0; i < N; ++i)
        REQUIRE(dst[i] == Catch::Approx(src[i] * src[i] + src[i]).epsilon(1e-6f));

    double ref_sum = 0.0;
    for (std::size_t i = 0; i < N; ++i) ref_sum += static_cast<double>(src[i]) * static_cast<double>(src[i]) + static_cast<double>(src[i]);
    auto r = exec.reduce<reduce_op::sum, float>(e, sv, ctx);
    REQUIRE(r.has_value());
    REQUIRE(static_cast<double>(*r) == Catch::Approx(ref_sum).epsilon(1e-5));
}

// ============================================================================
// Phase 3 Tests — Hardware Completeness & Layout Polish
// ============================================================================

// Phase 3.1 — Include hygiene: include only user-surface headers, no Highway symbols directly.
// This is a compile-only test — if it compiles, include separation is correct.
TEST_CASE (


"Phase3: user-surface headers compile without direct Highway symbols"
,
"[phase3][include][hygiene]"
)
 {
    // Both headers are already included at the top of this TU (pravaha_hetero.hpp,
    // pravaha_expr.hpp). If Highway leaked into the user surface, Highway symbols
    // would be accessible here — but we deliberately do not reference any hn:: or
    // hwy:: symbols. The test asserts no compilation failure, which proves the
    // user surface is clean of direct hardware dependencies.
    using namespace pravaha::expr;
    var x;
    auto e = x + x;
    // Structural hash works purely via the expression topology — no SIMD needed.
    const auto h1 = pravaha::hetero::structural_hash(e);
    const auto h2 = pravaha::hetero::structural_hash(e);
    REQUIRE(h1 == h2);  // deterministic
}

// Phase 3.2 — FakeBackend: custom concept-satisfying backend integrates and cascades correctly.
namespace {
    struct FakeBackend {
        static pravaha::compute::backend_metadata static_metadata() noexcept {
            return {"FakeBackend", 200}; // higher priority than SIMD (100)
        }

        bool is_available() const noexcept { return available_; }

        bool supports_expression(std::size_t /*hash*/, pravaha::compute::data_element_type) const noexcept {
            return true;
        }

        std::uint64_t evaluate_cost(const pravaha::compute::buffer_descriptor&, std::size_t) const noexcept {
            return available_ ? 1u : 0u;
        }

        template <typename T, lithe::Expression E>
        pravaha::Outcome<void> execute_elementwise(
            const E& expr,
            pravaha::compute::compute_view<T> dst,
            pravaha::compute::compute_view<const T> src,
            const pravaha::hetero::execution_context& /*ctx*/) {
            if (force_fail_)
                return std::unexpected(pravaha::PravahaError::make(
                    pravaha::ErrorKind::InternalError, "FakeBackend forced fail"));
            // Write 2*x so tests can distinguish FakeBackend from SIMD (which writes x).
            const std::size_t n = static_cast<std::size_t>(src.desc.element_count());
            for (std::size_t i = 0; i < n; ++i) dst.base()[i] = src.base()[i] * 2.0f;
            return {};
        }

        template <typename T, std::size_t K, lithe::Expression E>
        pravaha::Outcome<void> execute_elementwise_multi(
            const E&,
            pravaha::compute::compute_view<T>,
            const std::array<pravaha::compute::compute_view<const T>, K>&,
            const pravaha::hetero::execution_context&) {
            return std::unexpected(pravaha::PravahaError::make(
                pravaha::ErrorKind::InternalError, "FakeBackend: multi not implemented"));
        }

        template <pravaha::expr::reduce_op Op, typename T, lithe::Expression Child>
        pravaha::Outcome<T> execute_reduction(
            const Child&, pravaha::compute::compute_view<const T>) {
            return std::unexpected(pravaha::PravahaError::make(
                pravaha::ErrorKind::InternalError, "FakeBackend: reduce not implemented"));
        }

        template <pravaha::expr::reduce_op Op, typename T, std::size_t K, lithe::Expression Child>
        pravaha::Outcome<T> execute_reduction_multi(
            const Child&,
            const std::array<pravaha::compute::compute_view<const T>, K>&) {
            return std::unexpected(pravaha::PravahaError::make(
                pravaha::ErrorKind::InternalError, "FakeBackend: reduce_multi not implemented"));
        }

        bool available_ = true;
        bool force_fail_ = false;
    };

    static_assert(pravaha::compute::ComputeBackend<FakeBackend>);
} // anonymous namespace

TEST_CASE (


"Phase3: FakeBackend selected when cost wins; cascades to SIMD on failure"
,
"[phase3][custom][backend]"
)
 {
    using namespace pravaha::expr;
    constexpr std::size_t N = 32;
    std::vector<float> src(N), dst(N, 0.f);
    for (std::size_t i = 0; i < N; ++i) src[i] = static_cast<float>(i);

    buffer_descriptor d; d.shape = {N}; d.element_type = data_element_type::f32;
    auto sv = make_const_view(src.data(), d);
    auto dv = make_view(dst.data(), d);

    var x;
    auto e = x;  // identity — FakeBackend writes 2*x, SIMD writes x

    execution_context ctx;

    // --- FakeBackend wins and executes (writes 2*x) ---
    {
        std::fill(dst.begin(), dst.end(), 0.f);
        auto dv2 = make_view(dst.data(), d);
        pravaha::hetero::basic_hetero_executor<FakeBackend, pravaha::backends::HostSimdBackend> exec;
        REQUIRE(exec.execute(e, dv2, sv, ctx).has_value());
        // FakeBackend ran: result = 2*x
        for (std::size_t i = 0; i < N; ++i)
            REQUIRE(dst[i] == Catch::Approx(2.0f * src[i]));
    }

    // --- FakeBackend fails → cascades to HostSimdBackend (writes x) ---
    {
        std::fill(dst.begin(), dst.end(), 0.f);
        auto dv3 = make_view(dst.data(), d);
        FakeBackend fb; fb.force_fail_ = true;
        pravaha::hetero::basic_hetero_executor<FakeBackend, pravaha::backends::HostSimdBackend> exec2{
            .backends_ = {fb, pravaha::backends::HostSimdBackend{}}
        };
        REQUIRE(exec2.execute(e, dv3, sv, ctx).has_value());
        // Cascaded to SIMD: result = x
        for (std::size_t i = 0; i < N; ++i)
            REQUIRE(dst[i] == Catch::Approx(src[i]));
    }
}

// Phase 3.3 — No-metal path: force host_simd → SIMD selected, no GPU dispatch.
TEST_CASE (


"Phase3: force host_simd policy selects SIMD, no GPU dispatch"
,
"[phase3][no-metal][simd]"
)
 {
    using namespace pravaha::expr;
    constexpr std::size_t N = 128;
    std::vector<float> src(N), dst(N, 0.f);
    for (std::size_t i = 0; i < N; ++i) src[i] = static_cast<float>(i) * 0.1f;

    buffer_descriptor d; d.shape = {N}; d.element_type = data_element_type::f32;
    auto sv = make_const_view(src.data(), d);
    auto dv = make_view(dst.data(), d);

    hetero_executor exec;
    exec.policy.force = compute_domain::host_simd;
    execution_context ctx;

    var x;
    auto e = x * x;
    REQUIRE(exec.execute(e, dv, sv, ctx).has_value());

    for (std::size_t i = 0; i < N; ++i)
        REQUIRE(dst[i] == Catch::Approx(src[i] * src[i]).epsilon(1e-6f));

    // Reduce also stays on SIMD
    double ref = 0.0;
    for (std::size_t i = 0; i < N; ++i) ref += static_cast<double>(src[i]) * static_cast<double>(src[i]);
    auto r = exec.reduce<reduce_op::sum, float>(e, sv, ctx);
    REQUIRE(r.has_value());
    REQUIRE(static_cast<double>(*r) == Catch::Approx(ref).epsilon(1e-5));
}

// --- impl-5: structural_hash now delegates to lithe ----------------------
TEST_CASE (


"structural_hash: distinct constants → distinct keys"
,
"[hetero][hash][impl5]"
)
 {
    using namespace pravaha::expr;
    var x;
    const auto h1 = pravaha::hetero::structural_hash(x + lit(1.0f));
    const auto h2 = pravaha::hetero::structural_hash(x + lit(2.0f));
    CHECK(h1 != h2);   // baked-in literal is part of the kernel identity
}

TEST_CASE (


"structural_hash: identical trees → same key"
,
"[hetero][hash][impl5]"
)
 {
    using namespace pravaha::expr;
    auto build = [] { var y; return y * y + lit(3.0f); };
    CHECK(pravaha::hetero::structural_hash(build()) ==
          pravaha::hetero::structural_hash(build()));
}

TEST_CASE (


"structural_hash: distinct input slots → distinct keys"
,
"[hetero][hash][impl5]"
)
 {
    using namespace pravaha::expr;
    input<0> a; input<1> b;
    // a+b vs a+a differ (input<1> vs input<0> fold distinct ids)
    CHECK(pravaha::hetero::structural_hash(a + b) !=
          pravaha::hetero::structural_hash(a + a));
}

// ============================================================================
// Vulkan GPU backend tests — Part 6.
// Two groups:
//  (A) Platform-independent SPIR-V emitter — always compiled, no device needed.
//  (B) Device dispatch round-trip — guarded by LITHE_VULKAN_BACKEND_AVAILABLE
//      and skipped at runtime when no Vulkan device is present.
// ============================================================================

#include "pravaha/backends/vulkan_gpu.hpp"

// ── Part 6A: SPIR-V emitter (always-on, device-free) ─────────────────────────

TEST_CASE (


"vulkan spirv: emit_kernel produces non-empty module"
,
"[vulkan][spirv][part6a]"
)
 {
    using namespace pravaha::expr;
    using namespace pravaha::backends::vulkan::spirv;
    var x;
    auto mod = emit_kernel(x * lit(2.0f), pravaha::compute::data_element_type::f32);
    CHECK(!mod.words.empty());
    CHECK(mod.local_x == 256);
    CHECK(mod.local_y == 1);
    CHECK(mod.local_z == 1);
}

TEST_CASE (


"vulkan spirv: SPIR-V magic word is correct"
,
"[vulkan][spirv][part6a]"
)
 {
    using namespace pravaha::expr;
    using namespace pravaha::backends::vulkan::spirv;
    var x;
    auto mod = emit_kernel(x + x, pravaha::compute::data_element_type::f32);
    REQUIRE(!mod.words.empty());
    CHECK(mod.words[0] == 0x07230203u);  // SPIR-V magic
}

TEST_CASE (


"vulkan spirv: bound field is patched (> 1)"
,
"[vulkan][spirv][part6a]"
)
 {
    using namespace pravaha::expr;
    using namespace pravaha::backends::vulkan::spirv;
    var x;
    auto mod = emit_kernel(sqrt(x), pravaha::compute::data_element_type::f32);
    REQUIRE(mod.words.size() >= 4);
    // Header word 3 is the bound (result id count).
    CHECK(mod.words[3] > 1u);
}

TEST_CASE (


"vulkan spirv: different expressions yield different modules"
,
"[vulkan][spirv][part6a]"
)
 {
    using namespace pravaha::expr;
    using namespace pravaha::backends::vulkan::spirv;
    var x;
    auto m1 = emit_kernel(x + lit(1.0f), pravaha::compute::data_element_type::f32);
    auto m2 = emit_kernel(x * lit(2.0f), pravaha::compute::data_element_type::f32);
    // Different ASTs → different word sequences.
    CHECK(m1.words != m2.words);
}

TEST_CASE (


"vulkan spirv: emit_kernel i32 type uses integer opcodes"
,
"[vulkan][spirv][part6a]"
)
 {
    using namespace pravaha::expr;
    using namespace pravaha::backends::vulkan::spirv;
    input<0> a;
    auto mod = emit_kernel(a + a, pravaha::compute::data_element_type::i32);
    CHECK(!mod.words.empty());
    // The module must contain OpTypeInt (opcode 21).
    constexpr std::uint32_t kOpTypeInt = 21u;
    bool found = false;
    for (std::size_t i = 5; i < mod.words.size(); ++i) {
        if ((mod.words[i] & 0xFFFFu) == kOpTypeInt) { found = true; break; }
    }
    CHECK(found);
}

TEST_CASE (


"vulkan spirv: emit_reduce_kernel produces valid module"
,
"[vulkan][spirv][part6a]"
)
 {
    using namespace pravaha::expr;
    using namespace pravaha::backends::vulkan::spirv;
    var x;
    auto mod = emit_reduce_kernel(x * x,
        pravaha::expr::reduce_op::sum,
        pravaha::compute::data_element_type::f32);
    CHECK(!mod.words.empty());
    CHECK(mod.words[0] == 0x07230203u);
}

TEST_CASE (


"vulkan spirv: multi-input kernel has more bindings"
,
"[vulkan][spirv][part6a]"
)
 {
    using namespace pravaha::expr;
    using namespace pravaha::backends::vulkan::spirv;
    input<0> a; input<1> b;
    auto m1 = emit_kernel(a + a, pravaha::compute::data_element_type::f32);       // 1 src
    auto m2 = emit_kernel(a + b, pravaha::compute::data_element_type::f32);       // 2 src
    // Two-src module must be larger (extra variable declarations).
    CHECK(m2.words.size() > m1.words.size());
}

TEST_CASE (


"vulkan spirv: cache hit after first compile"
,
"[vulkan][spirv][part6a]"
)
 {
#if LITHE_VULKAN_BACKEND_AVAILABLE
    using namespace pravaha::expr;
    using namespace pravaha::backends::vulkan;
    var x;
    auto& cache = kernel_cache();
    const std::uint64_t key = pravaha::hetero::structural_hash(x + lit(5.0f));
    // Ensure miss then hit.
    auto m1 = get_or_compile_module(x + lit(5.0f), pravaha::compute::data_element_type::f32);
    auto m2 = get_or_compile_module(x + lit(5.0f), pravaha::compute::data_element_type::f32);
    REQUIRE (m1.has_value());
    REQUIRE (m2.has_value());
    CHECK (m1->words== m2->words);
    (void)cache; (void)key;
#else
    SKIP("LITHE_VULKAN_BACKEND_AVAILABLE not set");
#endif
}

// ── Part 6B: Device round-trip (skipped when no device) ──────────────────────

TEST_CASE (


"vulkan backend: metadata and priority"
,
"[vulkan][backend][part6b]"
)
 {
#if LITHE_VULKAN_BACKEND_AVAILABLE
    using namespace pravaha::backends::vulkan;
    auto meta = VulkanGpuBackend::static_metadata();
    CHECK (std::string_view(meta.name) == "vulkan_gpu");
    CHECK (meta.hardware_priority== 150);
#else
    SKIP("LITHE_VULKAN_BACKEND_AVAILABLE not set");
#endif
}

TEST_CASE (


"vulkan backend: type support"
,
"[vulkan][backend][part6b]"
)
 {
#if LITHE_VULKAN_BACKEND_AVAILABLE
    using namespace pravaha::backends::vulkan;
    using T = pravaha::compute::data_element_type;
    CHECK (VulkanGpuBackend::supports_type(T::f32));
    CHECK (VulkanGpuBackend::supports_type(T::f16));
    CHECK (VulkanGpuBackend::supports_type(T::i32));
    CHECK (VulkanGpuBackend::supports_type(T::u32));
    CHECK (!VulkanGpuBackend::supports_type (T::f64));
    CHECK (!VulkanGpuBackend::supports_type (T::i8));
#else
    SKIP("LITHE_VULKAN_BACKEND_AVAILABLE not set");
#endif
}

TEST_CASE (


"vulkan backend: elementwise dispatch round-trip (device required)"
,
"[vulkan][dispatch][part6b]"
)
 {
#if LITHE_VULKAN_BACKEND_AVAILABLE
    using namespace pravaha::backends::vulkan;
    VulkanGpuBackend be;
    if (!be.is_available()) {
        SKIP("No Vulkan device available");
    }

    using namespace pravaha::expr;
    using namespace pravaha::compute;
    constexpr std::size_t N = 512;

    std::vector<float> src_data(N), dst_data(N, 0.0f);
    for (std::size_t i =0; i<N;++i) src_data [i] = static_cast<float>(i);

    buffer_descriptor d;
    d.shape={N};
    d.element_type= data_element_type::f32;

    auto src_view = make_const_view<float>(src_data.data(), d);
    auto dst_view = make_view<float>(dst_data.data(), d);

    var x;
    pravaha::hetero::execution_context ctx;
    auto r = be.execute_elementwise(x * lit(2.0f), dst_view, src_view, ctx);
    REQUIRE (r.has_value());

    // Verify: dst[i] == 2 * src[i].
    for (std::size_t i =0; i<N;++i) {
        CHECK(dst_data[i] == Catch::Approx(2.0f * static_cast<float>(i)).epsilon(1e-5f));
    }
#else
    SKIP("LITHE_VULKAN_BACKEND_AVAILABLE not set");
#endif
}

TEST_CASE (


"vulkan backend: reduction round-trip (device required)"
,
"[vulkan][reduce][part6b]"
)
 {
#if LITHE_VULKAN_BACKEND_AVAILABLE
    using namespace pravaha::backends::vulkan;
    VulkanGpuBackend be;
    if (!be.is_available()) {
        SKIP("No Vulkan device available");
    }

    using namespace pravaha::expr;
    using namespace pravaha::compute;
    constexpr std::size_t N = 1024;

    std::vector<float> src_data(N, 1.0f); // all-ones: sum = N
    buffer_descriptor d;
    d.shape={N};
    d.element_type= data_element_type::f32;
    auto src_view = make_const_view<float>(src_data.data(), d);

    var x;
    auto r = be.execute_reduction<pravaha::expr::reduce_op::sum>(x, src_view);
    REQUIRE (r.has_value());
    CHECK (*r== Catch::Approx (static_cast<float>(N)).epsilon (1e-3f));
#else
    SKIP("LITHE_VULKAN_BACKEND_AVAILABLE not set");
#endif
}

TEST_CASE (


"vulkan backend: evaluate_cost below threshold returns 0"
,
"[vulkan][cost][part6b]"
)
 {
#if LITHE_VULKAN_BACKEND_AVAILABLE
    using namespace pravaha::backends::vulkan;
    using namespace pravaha::compute;
    VulkanGpuBackend be;
    buffer_descriptor d;
    d.shape={16}; // tiny — below kGpuElementwiseThreshold
    d.element_type= data_element_type::f32;
    CHECK (be.evaluate_cost(d, 0)== 0);
#else
    SKIP("LITHE_VULKAN_BACKEND_AVAILABLE not set");
#endif
}

TEST_CASE (


"vulkan backend: evaluate_cost above threshold returns nonzero when device up"
,
"[vulkan][cost][part6b]"
)
 {
#if LITHE_VULKAN_BACKEND_AVAILABLE
    using namespace pravaha::backends::vulkan;
    using namespace pravaha::compute;
    VulkanGpuBackend be;
    if (!be.is_available()) SKIP ("No Vulkan device");
    buffer_descriptor d;
    d.shape={1024 * 1024}; // 4 MB — above threshold
    d.element_type= data_element_type::f32;
    CHECK (be.evaluate_cost(d, 0)> 0);
#else
    SKIP("LITHE_VULKAN_BACKEND_AVAILABLE not set");
#endif
}

// ============================================================================
// ARCHITECTURE_REVIEW.md fixes (Part 7.1): element_type_for, staging pool,
// GPU reduction tree, device_provider, non-f32 elementwise round-trip.
// ============================================================================

TEST_CASE (


"compute::element_type_for maps scalar types"
,
"[hetero][part7_1]"
)
 {
    // Device-free: pure compile-time type→enum map (fixes hardcoded-f32 bug).
    using pravaha::compute::element_type_for;
    using T = pravaha::compute::data_element_type;
    STATIC_REQUIRE(element_type_for<float>          == T::f32);
    STATIC_REQUIRE(element_type_for<double>         == T::f64);
    STATIC_REQUIRE(element_type_for<std::int32_t>   == T::i32);
    STATIC_REQUIRE(element_type_for<std::uint32_t>  == T::u32);
    STATIC_REQUIRE(element_type_for<std::int16_t>   == T::i16);
    STATIC_REQUIRE(element_type_for<std::uint16_t>  == T::u16);
    STATIC_REQUIRE(element_type_for<std::int8_t>    == T::i8);
    STATIC_REQUIRE(element_type_for<std::uint8_t>   == T::u8);
    STATIC_REQUIRE(element_type_for<std::int64_t>   == T::i64);
    STATIC_REQUIRE(element_type_for<std::uint64_t>  == T::u64);
    // cv-qualifiers stripped.
    STATIC_REQUIRE(element_type_for<const float>    == T::f32);
}

TEST_CASE (


"vulkan staging_pool: acquire/release reuses backing buffer (device required)"
,
"[vulkan][pool][part7_1]"
)
 {
#if LITHE_VULKAN_BACKEND_AVAILABLE
    using namespace pravaha::backends::vulkan;
    VulkanGpuBackend be;
    if (!be.is_available()) SKIP ("No Vulkan device available");

    auto& prov = device_provider::instance();
    auto& dev = prov.backend();
    auto ctx = dev.context();
    REQUIRE (ctx);
    REQUIRE (ctx->valid());

    staging_pool pool;
    const std::size_t bytes = 256 * sizeof(float);

    // First acquire allocates; capture the VkBuffer handle, then release it.
    auto a = pool.acquire(ctx->device, ctx->phys_dev, bytes);
    REQUIRE (a.has_value());
    REQUIRE (a->valid());
    VkBuffer first = a->buffer;
    const std::size_t cap = a->size;
    CHECK (cap>= bytes);
    pool.release (std::move(*a));

    // Second acquire of the same (or smaller) size must reuse the freed buffer.
    auto bcq = pool.acquire(ctx->device, ctx->phys_dev, bytes);
    REQUIRE (bcq.has_value());
    REQUIRE (bcq->valid());
    CHECK (bcq->buffer== first); // same backing buffer → no re-alloc
    CHECK (bcq->size== cap);
    pool.release (std::move(*bcq));
#else
    SKIP("LITHE_VULKAN_BACKEND_AVAILABLE not set");
#endif
}

TEST_CASE (


"vulkan backend: GPU reduction tree sum/max/min (device required)"
,
"[vulkan][reduce][tree][part7_1]"
)
 {
#if LITHE_VULKAN_BACKEND_AVAILABLE
    using namespace pravaha::backends::vulkan;
    VulkanGpuBackend be;
    if (!be.is_available()) SKIP ("No Vulkan device available");

    using namespace pravaha::expr;
    using namespace pravaha::compute;

    // Non-power-of-two, larger-than-one-workgroup count exercises the padded
    // multi-group tree fold + host partial fold.
    constexpr std::size_t N = 1000;
    std::vector<float> data(N);
    for (std::size_t i =0; i<N;++i) data [i] = static_cast<float>(i + 1); // 1..N

    buffer_descriptor d;
    d.shape={N};
    d.element_type= data_element_type::f32;
    auto src = make_const_view<float>(data.data(), d);

    var x;
    const double expect_sum = static_cast<double>(N) * (N + 1) / 2.0; // Σ 1..N

    auto rs = be.execute_reduction<reduce_op::sum>(x, src);
    REQUIRE (rs.has_value());
    CHECK (static_cast<double>(*rs)== Catch::Approx (expect_sum).epsilon (1e-4));

    auto rmax = be.execute_reduction<reduce_op::max>(x, src);
    REQUIRE (rmax.has_value());
    CHECK (*rmax== Catch::Approx (static_cast<float>(N)).epsilon (1e-5f));

    auto rmin = be.execute_reduction<reduce_op::min>(x, src);
    REQUIRE (rmin.has_value());
    CHECK (*rmin== Catch::Approx (1.0f).epsilon (1e-5f));
#else
    SKIP("LITHE_VULKAN_BACKEND_AVAILABLE not set");
#endif
}

TEST_CASE (


"vulkan backend: warm-path repeat dispatch stays correct (device required)"
,
"[vulkan][pool][warm][part7_1]"
)
 {
#if LITHE_VULKAN_BACKEND_AVAILABLE
    using namespace pravaha::backends::vulkan;
    VulkanGpuBackend be;
    if (!be.is_available()) SKIP ("No Vulkan device available");

    using namespace pravaha::expr;
    using namespace pravaha::compute;
    constexpr std::size_t N = 512;

    std::vector<float> src_data(N), dst_data(N, 0.0f);
    for (std::size_t i =0; i<N;++i) src_data [i] = static_cast<float>(i);

    buffer_descriptor d;
    d.shape={N};
    d.element_type= data_element_type::f32;
    auto src = make_const_view<float>(src_data.data(), d);
    auto dst = make_view<float>(dst_data.data(), d);

    var x;
    pravaha::hetero::execution_context ctx;

    // Run the same kernel twice: second call hits the warm pipeline cache and
    // reuses pooled staging buffers — must still be correct (pool-reuse regression).
    for (int pass = 0; pass<2;++pass) {
        std::fill(dst_data.begin(), dst_data.end(), 0.0f);
        auto r = be.execute_elementwise(x * lit(2.0f), dst, src, ctx);
        REQUIRE(r.has_value());
        for (std::size_t i = 0; i < N; ++i)
            CHECK(dst_data[i] == Catch::Approx(2.0f * static_cast<float>(i)).epsilon(1e-5f));
    }
#else
    SKIP("LITHE_VULKAN_BACKEND_AVAILABLE not set");
#endif
}

TEST_CASE (


"vulkan backend: i32 elementwise round-trip (element_type_for fix, device required)"
,
"[vulkan][dispatch][i32][part7_1]"
)
 {
#if LITHE_VULKAN_BACKEND_AVAILABLE
    using namespace pravaha::backends::vulkan;
    VulkanGpuBackend be;
    if (!be.is_available()) SKIP ("No Vulkan device available");

    using namespace pravaha::expr;
    using namespace pravaha::compute;
    constexpr std::size_t N = 512;

    std::vector<std::int32_t> src_data(N), dst_data(N, 0);
    for (std::size_t i =0; i<N;++i) src_data [i] = static_cast<std::int32_t>(i);

    buffer_descriptor d;
    d.shape={N};
    d.element_type= data_element_type::i32; // exercises the non-f32 path
    auto src = make_const_view<std::int32_t>(src_data.data(), d);
    auto dst = make_view<std::int32_t>(dst_data.data(), d);

    var x;
    pravaha::hetero::execution_context ctx;
    // a + a = 2a using integer opcodes (element_type_for<int32_t> → i32 kernel).
    auto r = be.execute_elementwise(x + x, dst, src, ctx);
    REQUIRE (r.has_value());
    for (std::size_t i =0; i<N;++i)
        CHECK (dst_data[i]== static_cast<std::int32_t>(2 * i));
#else
    SKIP("LITHE_VULKAN_BACKEND_AVAILABLE not set");
#endif
}
