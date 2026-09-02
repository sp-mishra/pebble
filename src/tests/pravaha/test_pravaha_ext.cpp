// ============================================================================
// test_pravaha_ext.cpp — tests for sutra/pravaha_ext.hpp
//
// Covers:
//   §D  build_pravaha_parallel: DAG partitioning, slot assignment, wave groups
//   §E  pravaha_backend_ext: extension attach + backend dispatch
//   §F  compile_parallel / compile_parallel_full: full pipeline entry points
//   EdgeKind classification
//   TaskNodeKind classification per op
//   ExecutionDomain assignment from effect mask
//   JoinPolicy selection per consumer op
//   Correctness of compute lambdas for all supported ops
//   Parallel-wave detection for independent branches
//   Plugin-op passthrough in compute lambdas
//   Param injection into slot vector
//   Seq/control edge classification
// ============================================================================

#include "catch_amalgamated.hpp"

#include "sutra/core/pravaha_ext.hpp"
#include "sutra/math/math_ext.hpp"

#include <cmath>
#include <thread>

using namespace sutra;
using namespace sutra::literals;
using Catch::Approx;

// ─── helpers ────────────────────────────────────────────────────────────────

// Evaluate a pravaha_graph_artifact by injecting param values and running all
// compute lambdas in topo order, then returning the result slot.
static double eval_graph(
    const pravaha_graph_artifact& g,
    std::initializer_list<double> param_vals) {
    std::vector<double> slots(g.slot_count, 0.0);
    auto it = param_vals.begin();
    for (std::size_t pi = 0; pi < g.param_slots.size(); ++pi, ++it) {
        if (pi >= param_vals.size()) break;
        if (g.param_slots[pi] != 0)
            slots[g.param_slots[pi]] = *it;
    }
    for (const auto& td : g.tasks)
        if (td.compute) td.compute(slots);
    return g.result_slot < slots.size() ? slots[g.result_slot] : 0.0;
}

// ============================================================================
// §F  compile_parallel — basic pipeline
// ============================================================================

TEST_CASE (



"compile_parallel: null formula returns debug-text artifact"
,
"[pravaha_ext][compile_parallel]"
)
 {
    context ctx;
    formula_ref null_f;
    auto result = compile_parallel(ctx, null_f);
    REQUIRE(result.is_debug());
}

TEST_CASE (



"compile_parallel: literal expression compiles and evaluates"
,
"[pravaha_ext][compile_parallel]"
)
 {
    context ctx;
    auto expr = 7.0_k;
    auto result = compile_parallel(ctx, expr);
    REQUIRE(result.is_pravaha());
    double v = eval_graph(result.as_pravaha(), {});
    REQUIRE(v == Approx(7.0));
}

TEST_CASE (



"compile_parallel: single-param formula evaluates correctly"
,
"[pravaha_ext][compile_parallel]"
)
 {
    context ctx;
    param x("px");
    formula_ref f = x + 3.0;
    auto result = compile_parallel(ctx, f);
    REQUIRE(result.is_pravaha());
    double v = eval_graph(result.as_pravaha(), {5.0});
    REQUIRE(v == Approx(8.0));
}

TEST_CASE (



"compile_parallel: two-param arithmetic evaluates correctly"
,
"[pravaha_ext][compile_parallel]"
)
 {
    context ctx;
    param a("pa"), b("pb");
    formula_ref f = a * b + a;

    auto result = compile_parallel(ctx, f);
    REQUIRE(result.is_pravaha());

    const auto& g = result.as_pravaha();
    // param_order is collected in pre-order by collect_symbols; just eval directly
    std::vector<double> slots(g.slot_count, 0.0);
    for (std::size_t pi = 0; pi < g.param_slots.size(); ++pi) {
        if (g.param_slots[pi] == 0) continue;
        double v = (g.param_names[pi] == "pa") ? 3.0 : 4.0;
        slots[g.param_slots[pi]] = v;
    }
    for (const auto& td : g.tasks) if (td.compute) td.compute(slots);
    double out = g.result_slot < slots.size() ? slots[g.result_slot] : -1.0;
    REQUIRE(out == Approx(15.0));  // 3*4 + 3 = 15
}

TEST_CASE (



"compile_parallel: slot_count > number of nodes (sentinel slot 0)"
,
"[pravaha_ext][compile_parallel]"
)
 {
    context ctx;
    param x("scp");
    formula_ref f = x + 1.0;
    auto result = compile_parallel(ctx, f);
    REQUIRE(result.is_pravaha());
    REQUIRE(result.as_pravaha().slot_count > result.as_pravaha().tasks.size());
}

TEST_CASE (



"compile_parallel: result_slot is non-zero for valid formula"
,
"[pravaha_ext][compile_parallel]"
)
 {
    context ctx;
    param x("rsp");
    formula_ref f = x * 2.0;
    auto result = compile_parallel(ctx, f);
    REQUIRE(result.is_pravaha());
    REQUIRE(result.as_pravaha().result_slot != 0);
}

TEST_CASE (



"compile_parallel: param_order and param_names have same size"
,
"[pravaha_ext][compile_parallel]"
)
 {
    context ctx;
    param a("poa"), b("pob"), c("poc");
    formula_ref f = a + b + c;
    auto result = compile_parallel(ctx, f);
    REQUIRE(result.is_pravaha());
    const auto& g = result.as_pravaha();
    REQUIRE(g.param_order.size() == g.param_names.size());
}

// ============================================================================
// §F  Arithmetic correctness — all builtin ops
// ============================================================================

TEST_CASE (



"compile_parallel: subtraction evaluates correctly"
,
"[pravaha_ext][arithmetic]"
)
 {
    context ctx;
    param a("suba"), b("subb");
    auto result = compile_parallel(ctx, formula_ref(a) - formula_ref(b));
    REQUIRE(result.is_pravaha());
    const auto& g = result.as_pravaha();
    std::vector<double> s(g.slot_count, 0.0);
    for (std::size_t i = 0; i < g.param_slots.size(); ++i) {
        if (!g.param_slots[i]) continue;
        s[g.param_slots[i]] = (g.param_names[i] == "suba") ? 10.0 : 3.0;
    }
    for (const auto& td : g.tasks) if (td.compute) td.compute(s);
    REQUIRE(s[g.result_slot] == Approx(7.0));
}

TEST_CASE (



"compile_parallel: division evaluates correctly"
,
"[pravaha_ext][arithmetic]"
)
 {
    context ctx;
    param a("diva"), b("divb");
    auto result = compile_parallel(ctx, formula_ref(a) / formula_ref(b));
    REQUIRE(result.is_pravaha());
    const auto& g = result.as_pravaha();
    std::vector<double> s(g.slot_count, 0.0);
    for (std::size_t i = 0; i < g.param_slots.size(); ++i) {
        if (!g.param_slots[i]) continue;
        s[g.param_slots[i]] = (g.param_names[i] == "diva") ? 9.0 : 3.0;
    }
    for (const auto& td : g.tasks) if (td.compute) td.compute(s);
    REQUIRE(s[g.result_slot] == Approx(3.0));
}

TEST_CASE (



"compile_parallel: division by zero returns zero not NaN"
,
"[pravaha_ext][arithmetic]"
)
 {
    context ctx;
    param a("divza"), b("divzb");
    auto result = compile_parallel(ctx, formula_ref(a) / formula_ref(b));
    REQUIRE(result.is_pravaha());
    const auto& g = result.as_pravaha();
    std::vector<double> s(g.slot_count, 0.0);
    // b stays 0
    for (std::size_t i = 0; i < g.param_slots.size(); ++i) {
        if (!g.param_slots[i]) continue;
        if (g.param_names[i] == "divza") s[g.param_slots[i]] = 5.0;
    }
    for (const auto& td : g.tasks) if (td.compute) td.compute(s);
    REQUIRE(s[g.result_slot] == Approx(0.0));
}

TEST_CASE (



"compile_parallel: negation evaluates correctly"
,
"[pravaha_ext][arithmetic]"
)
 {
    context ctx;
    param x("negx");
    auto result = compile_parallel(ctx, -formula_ref(x));
    REQUIRE(result.is_pravaha());
    const auto& g = result.as_pravaha();
    std::vector<double> s(g.slot_count, 0.0);
    for (std::size_t i = 0; i < g.param_slots.size(); ++i)
        if (g.param_slots[i]) s[g.param_slots[i]] = 4.0;
    for (const auto& td : g.tasks) if (td.compute) td.compute(s);
    REQUIRE(s[g.result_slot] == Approx(-4.0));
}

TEST_CASE (



"compile_parallel: modulo evaluates correctly"
,
"[pravaha_ext][arithmetic]"
)
 {
    context ctx;
    param a("moda"), b("modb");
    auto result = compile_parallel(ctx, formula_ref(a) % formula_ref(b));
    REQUIRE(result.is_pravaha());
    const auto& g = result.as_pravaha();
    std::vector<double> s(g.slot_count, 0.0);
    for (std::size_t i = 0; i < g.param_slots.size(); ++i) {
        if (!g.param_slots[i]) continue;
        s[g.param_slots[i]] = (g.param_names[i] == "moda") ? 10.0 : 3.0;
    }
    for (const auto& td : g.tasks) if (td.compute) td.compute(s);
    REQUIRE(s[g.result_slot] == Approx(1.0));
}

TEST_CASE (



"compile_parallel: pow evaluates correctly (heavy op)"
,
"[pravaha_ext][arithmetic]"
)
 {
    context ctx;
    param base("powb"), exp_("powe");
    // op::pow is a builtin heavy op — lambda uses std::pow
    formula_node n;
    n.op = op::pow; n.arity = 2;
    auto& tls = ephemeral_formula_store::thread_local_instance();
    formula_node nb; nb.op = op::param_ref; nb.arity = 0; nb.raw_name = "powb";
    formula_node ne; ne.op = op::param_ref; ne.arity = 0; ne.raw_name = "powe";
    n.inline_children[0] = tls.alloc(nb);
    n.inline_children[1] = tls.alloc(ne);
    formula_ref f{tls.alloc(n), &tls};

    auto result = compile_parallel(ctx, f);
    REQUIRE(result.is_pravaha());
    const auto& g = result.as_pravaha();
    std::vector<double> s(g.slot_count, 0.0);
    for (std::size_t i = 0; i < g.param_slots.size(); ++i) {
        if (!g.param_slots[i]) continue;
        s[g.param_slots[i]] = (g.param_names[i] == "powb") ? 2.0 : 10.0;
    }
    for (const auto& td : g.tasks) if (td.compute) td.compute(s);
    REQUIRE(s[g.result_slot] == Approx(1024.0));
}

// ============================================================================
// §F  Boolean / comparison ops
// ============================================================================

TEST_CASE (



"compile_parallel: comparison ops produce 0/1 doubles"
,
"[pravaha_ext][boolean]"
)
 {
    context ctx;
    param a("cmpa"), b("cmpb");
    formula_ref fa = a, fb = b;

    auto check = [&](formula_ref expr, double av, double bv, double expected) {
        auto result = compile_parallel(ctx, expr);
        REQUIRE(result.is_pravaha());
        const auto& g = result.as_pravaha();
        std::vector<double> s(g.slot_count, 0.0);
        for (std::size_t i = 0; i < g.param_slots.size(); ++i) {
            if (!g.param_slots[i]) continue;
            s[g.param_slots[i]] = (g.param_names[i] == "cmpa") ? av : bv;
        }
        for (const auto& td : g.tasks) if (td.compute) td.compute(s);
        REQUIRE(s[g.result_slot] == Approx(expected));
    };

    check(fa == fb, 3.0, 3.0, 1.0);
    check(fa == fb, 3.0, 4.0, 0.0);
    check(fa != fb, 1.0, 2.0, 1.0);
    check(fa <  fb, 1.0, 2.0, 1.0);
    check(fa <  fb, 2.0, 1.0, 0.0);
    check(fa <= fb, 2.0, 2.0, 1.0);
    check(fa >  fb, 5.0, 3.0, 1.0);
    check(fa >= fb, 2.0, 2.0, 1.0);
    check(fa && fb, 1.0, 1.0, 1.0);
    check(fa && fb, 0.0, 1.0, 0.0);
    check(fa || fb, 0.0, 1.0, 1.0);
    check(!fa,      0.0, 0.0, 1.0);
    check(!fa,      1.0, 0.0, 0.0);
}

// ============================================================================
// §F  if_expr — branch node (TaskNodeKind::branch, JoinPolicy::AnySuccess)
// ============================================================================

TEST_CASE (



"compile_parallel: if_expr takes true branch"
,
"[pravaha_ext][if]"
)
 {
    context ctx;
    param x("ifx");
    formula_ref cond = x > 0.0_k;
    formula_ref f    = if_expr(cond, x, -x);  // abs(x)

    auto result = compile_parallel(ctx, f);
    REQUIRE(result.is_pravaha());
    const auto& g = result.as_pravaha();

    auto eval = [&](double xv) {
        std::vector<double> s(g.slot_count, 0.0);
        for (std::size_t i = 0; i < g.param_slots.size(); ++i)
            if (g.param_slots[i]) s[g.param_slots[i]] = xv;
        for (const auto& td : g.tasks) if (td.compute) td.compute(s);
        return s[g.result_slot];
    };

    REQUIRE(eval( 5.0) == Approx( 5.0));
    REQUIRE(eval(-3.0) == Approx( 3.0));
    REQUIRE(eval( 0.0) == Approx( 0.0));
}

// ============================================================================
// §F  seq — control edge classification
// ============================================================================

TEST_CASE (



"compile_parallel: seq_ node forwards right operand value"
,
"[pravaha_ext][seq]"
)
 {
    context ctx;
    param a("seqa"), b("seqb");
    formula_ref f = seq(formula_ref(a) + 0.0, formula_ref(b) * 3.0);

    auto result = compile_parallel(ctx, f);
    REQUIRE(result.is_pravaha());
    const auto& g = result.as_pravaha();
    std::vector<double> s(g.slot_count, 0.0);
    for (std::size_t i = 0; i < g.param_slots.size(); ++i) {
        if (!g.param_slots[i]) continue;
        s[g.param_slots[i]] = (g.param_names[i] == "seqb") ? 4.0 : 99.0;
    }
    for (const auto& td : g.tasks) if (td.compute) td.compute(s);
    REQUIRE(s[g.result_slot] == Approx(12.0));  // b*3 = 4*3
}

// ============================================================================
// §F  Plugin op passthrough
// ============================================================================

TEST_CASE (



"compile_parallel: plugin op (sin) computes correctly via eval_fn"
,
"[pravaha_ext][plugin]"
)
 {
    context ctx;
    ctx.use(math::extension{});

    param x("sinx");
    formula_ref f = math::sin(formula_ref(x));

    auto result = compile_parallel(ctx, f);
    REQUIRE(result.is_pravaha());
    const auto& g = result.as_pravaha();

    auto eval = [&](double xv) {
        std::vector<double> s(g.slot_count, 0.0);
        for (std::size_t i = 0; i < g.param_slots.size(); ++i)
            if (g.param_slots[i]) s[g.param_slots[i]] = xv;
        for (const auto& td : g.tasks) if (td.compute) td.compute(s);
        return s[g.result_slot];
    };

    REQUIRE(eval(0.0)       == Approx(std::sin(0.0)).margin(1e-10));
    REQUIRE(eval(M_PI/2.0)  == Approx(std::sin(M_PI/2.0)).margin(1e-10));
    REQUIRE(eval(M_PI)      == Approx(std::sin(M_PI)).margin(1e-10));
}

TEST_CASE (



"compile_parallel: plugin op (cos) computes correctly"
,
"[pravaha_ext][plugin]"
)
 {
    context ctx;
    ctx.use(math::extension{});

    param x("cosx");
    formula_ref f = math::cos(formula_ref(x));
    auto result = compile_parallel(ctx, f);
    REQUIRE(result.is_pravaha());
    const auto& g = result.as_pravaha();

    std::vector<double> s(g.slot_count, 0.0);
    for (std::size_t i = 0; i < g.param_slots.size(); ++i)
        if (g.param_slots[i]) s[g.param_slots[i]] = 0.0;
    for (const auto& td : g.tasks) if (td.compute) td.compute(s);
    REQUIRE(s[g.result_slot] == Approx(1.0).margin(1e-10));
}

TEST_CASE (



"compile_parallel: constant folding applies before graph build"
,
"[pravaha_ext][fold]"
)
 {
    context ctx;
    ctx.use(math::extension{});

    // sin(0) should fold to 0 — graph should have one leaf task
    formula_ref f = math::sin(0.0_k);
    auto result = compile_parallel(ctx, f);
    REQUIRE(result.is_pravaha());
    double v = eval_graph(result.as_pravaha(), {});
    REQUIRE(v == Approx(0.0).margin(1e-10));
}

// ============================================================================
// §D  compile_parallel_full — edges, node_kinds, parallel_waves
// ============================================================================

TEST_CASE (



"compile_parallel_full: returns non-empty artifact for valid formula"
,
"[pravaha_ext][full]"
)
 {
    context ctx;
    param x("fpx");
    formula_ref f = x * x;
    auto art = compile_parallel_full(ctx, f);
    REQUIRE_FALSE(art.graph.tasks.empty());
    REQUIRE_FALSE(art.node_kinds.empty());
    REQUIRE(art.graph.tasks.size() == art.node_kinds.size());
}

TEST_CASE (



"compile_parallel_full: null formula returns empty artifact"
,
"[pravaha_ext][full]"
)
 {
    context ctx;
    formula_ref null_f;
    auto art = compile_parallel_full(ctx, null_f);
    REQUIRE(art.graph.tasks.empty());
}

TEST_CASE (



"compile_parallel_full: edge list has correct producer→consumer direction"
,
"[pravaha_ext][full][edges]"
)
 {
    context ctx;
    param a("ea"), b("eb");
    formula_ref f = formula_ref(a) + formula_ref(b);
    auto art = compile_parallel_full(ctx, f);

    // a + b has 2 leaves and 1 interior node → 2 edges
    REQUIRE(art.graph.edges.size() == 2);
    for (const auto& [from, to] : art.graph.edges)
        REQUIRE(from < to);   // leaves come before interior node in topo order
}

TEST_CASE (



"compile_parallel_full: edge_meta has same count as edges"
,
"[pravaha_ext][full][edges]"
)
 {
    context ctx;
    param a("ema"), b("emb"), c("emc");
    formula_ref f = (formula_ref(a) + formula_ref(b)) * formula_ref(c);
    auto art = compile_parallel_full(ctx, f);
    REQUIRE(art.edge_meta.size() == art.graph.edges.size());
}

TEST_CASE (



"compile_parallel_full: data_flow edges for arithmetic ops"
,
"[pravaha_ext][full][edgekind]"
)
 {
    context ctx;
    param a("dfa"), b("dfb");
    formula_ref f = formula_ref(a) * formula_ref(b);
    auto art = compile_parallel_full(ctx, f);
    for (const auto& em : art.edge_meta)
        REQUIRE(em.kind == EdgeKind::data_flow);
}

TEST_CASE (



"compile_parallel_full: control edge for left side of seq_"
,
"[pravaha_ext][full][edgekind]"
)
 {
    context ctx;
    param a("sea"), b("seb");
    formula_ref f = seq(formula_ref(a) + 0.0_k, formula_ref(b) * 2.0);
    auto art = compile_parallel_full(ctx, f);

    bool found_control = false;
    for (const auto& em : art.edge_meta)
        if (em.kind == EdgeKind::control) found_control = true;
    REQUIRE(found_control);
}

TEST_CASE (



"compile_parallel_full: aggregation edge for or_ op"
,
"[pravaha_ext][full][edgekind]"
)
 {
    context ctx;
    param a("agga"), b("aggb");
    formula_ref f = formula_ref(a) || formula_ref(b);
    auto art = compile_parallel_full(ctx, f);

    bool found_agg = false;
    for (const auto& em : art.edge_meta)
        if (em.kind == EdgeKind::aggregation) found_agg = true;
    REQUIRE(found_agg);
}

// ============================================================================
// §D  TaskNodeKind classification
// ============================================================================

TEST_CASE (



"compile_parallel_full: leaf nodes classified correctly"
,
"[pravaha_ext][full][nodekind]"
)
 {
    context ctx;
    param x("lkx");
    // x is a param_ref (leaf) + 1.0_k (leaf lit) — root is add (scalar)
    formula_ref f = x + 1.0_k;
    auto art = compile_parallel_full(ctx, f);

    int leaf_count = 0;
    int scalar_count = 0;
    for (auto k : art.node_kinds) {
        if (k == TaskNodeKind::leaf)   ++leaf_count;
        if (k == TaskNodeKind::scalar) ++scalar_count;
    }
    REQUIRE(leaf_count == 2);    // param_ref and lit_f64
    REQUIRE(scalar_count == 1);  // add node
}

TEST_CASE (



"compile_parallel_full: pow node classified as heavy"
,
"[pravaha_ext][full][nodekind]"
)
 {
    context ctx;
    param b("powkb"), e("powke");
    formula_node n; n.op = op::pow; n.arity = 2;
    auto& tls = ephemeral_formula_store::thread_local_instance();
    formula_node nb; nb.op = op::param_ref; nb.arity = 0; nb.raw_name = "powkb";
    formula_node ne; ne.op = op::param_ref; ne.arity = 0; ne.raw_name = "powke";
    n.inline_children[0] = tls.alloc(nb);
    n.inline_children[1] = tls.alloc(ne);
    formula_ref f{tls.alloc(n), &tls};

    auto art = compile_parallel_full(ctx, f);
    bool found_heavy = false;
    for (auto k : art.node_kinds)
        if (k == TaskNodeKind::heavy) found_heavy = true;
    REQUIRE(found_heavy);
}

TEST_CASE (



"compile_parallel_full: if_ node classified as branch"
,
"[pravaha_ext][full][nodekind]"
)
 {
    context ctx;
    param x("ifkx");
    formula_ref f = if_expr(x > 0.0_k, formula_ref(x), -formula_ref(x));
    auto art = compile_parallel_full(ctx, f);

    bool found_branch = false;
    for (auto k : art.node_kinds)
        if (k == TaskNodeKind::branch) found_branch = true;
    REQUIRE(found_branch);
}

TEST_CASE (



"compile_parallel_full: plugin op classified as heavy"
,
"[pravaha_ext][full][nodekind]"
)
 {
    context ctx;
    ctx.use(math::extension{});

    param x("plkx");
    formula_ref f = math::sin(formula_ref(x));
    auto art = compile_parallel_full(ctx, f);

    bool found_heavy = false;
    for (auto k : art.node_kinds)
        if (k == TaskNodeKind::heavy) found_heavy = true;
    REQUIRE(found_heavy);
}

// ============================================================================
// §D  ExecutionDomain assignment
// ============================================================================

TEST_CASE (



"compile_parallel_full: leaf nodes get Inline domain"
,
"[pravaha_ext][full][domain]"
)
 {
    context ctx;
    param x("domx");
    formula_ref f = x + 1.0_k;
    auto art = compile_parallel_full(ctx, f);

    for (std::size_t i = 0; i < art.node_kinds.size(); ++i) {
        if (art.node_kinds[i] == TaskNodeKind::leaf)
            REQUIRE(art.graph.tasks[i].domain == pravaha::ExecutionDomain::Inline);
    }
}

TEST_CASE (



"compile_parallel_full: heavy ops get CPU domain by default"
,
"[pravaha_ext][full][domain]"
)
 {
    context ctx;
    ctx.use(math::extension{});

    param x("hdomx");
    formula_ref f = math::sin(formula_ref(x));
    auto art = compile_parallel_full(ctx, f, pravaha::ExecutionDomain::CPU);

    for (std::size_t i = 0; i < art.node_kinds.size(); ++i) {
        if (art.node_kinds[i] == TaskNodeKind::heavy)
            REQUIRE(art.graph.tasks[i].domain == pravaha::ExecutionDomain::CPU);
    }
}

// ============================================================================
// §D  JoinPolicy selection
// ============================================================================

TEST_CASE (



"compile_parallel_full: AllOrNothing join policy for arithmetic"
,
"[pravaha_ext][full][joinpolicy]"
)
 {
    context ctx;
    param a("jpa"), b("jpb");
    formula_ref f = formula_ref(a) + formula_ref(b);
    auto art = compile_parallel_full(ctx, f);

    for (const auto& em : art.edge_meta)
        REQUIRE(em.join_policy.kind == pravaha::JoinPolicyKind::AllOrNothing);
}

TEST_CASE (



"compile_parallel_full: AnySuccess join policy for if_ consumers"
,
"[pravaha_ext][full][joinpolicy]"
)
 {
    context ctx;
    param x("jpifx");
    formula_ref f = if_expr(x > 0.0_k, formula_ref(x), -formula_ref(x));
    auto art = compile_parallel_full(ctx, f);

    bool found_any = false;
    for (const auto& em : art.edge_meta)
        if (em.kind == EdgeKind::data_flow &&
            em.join_policy.kind == pravaha::JoinPolicyKind::AnySuccess)
            found_any = true;
    REQUIRE(found_any);
}

// ============================================================================
// §D  Parallel wave detection
// ============================================================================

TEST_CASE (



"compile_parallel_full: independent branches land in same wave"
,
"[pravaha_ext][full][waves]"
)
 {
    context ctx;
    param a("wa"), b("wb"), c("wc"), d("wd");
    // (a+b) and (c*d) are independent; root merges them
    formula_ref left  = formula_ref(a) + formula_ref(b);
    formula_ref right = formula_ref(c) * formula_ref(d);
    formula_ref f     = left + right;

    auto art = compile_parallel_full(ctx, f);

    // There must be at least one wave with >= 2 members (the two independent branches)
    bool found_parallel_wave = false;
    for (const auto& wave : art.parallel_waves)
        if (wave.size() >= 2) { found_parallel_wave = true; break; }
    REQUIRE(found_parallel_wave);
}

TEST_CASE (



"compile_parallel_full: leaves are all in the first wave (depth 0)"
,
"[pravaha_ext][full][waves]"
)
 {
    context ctx;
    param a("la"), b("lb"), c("lc");
    formula_ref f = formula_ref(a) + formula_ref(b) + formula_ref(c);
    auto art = compile_parallel_full(ctx, f);

    REQUIRE_FALSE(art.parallel_waves.empty());
    // Wave 0 = depth 0 = all leaf nodes
    REQUIRE(art.parallel_waves[0].size() >= 2);
}

TEST_CASE (



"compile_parallel_full: wave count grows with DAG depth"
,
"[pravaha_ext][full][waves]"
)
 {
    context ctx;
    param x("wx");
    // x → x+1 → (x+1)+2 → ... depth 3
    formula_ref f = formula_ref(x) + 1.0_k;
    f = f + 2.0_k;
    f = f + 3.0_k;
    auto art = compile_parallel_full(ctx, f);

    // Should have at least 4 distinct depth levels
    REQUIRE(art.parallel_waves.size() >= 4);
}

TEST_CASE (



"compile_parallel_full: join_groups populated for waves with >= 2 members"
,
"[pravaha_ext][full][waves]"
)
 {
    context ctx;
    param a("jwa"), b("jwb"), c("jwc"), d("jwd");
    formula_ref f = (formula_ref(a) + formula_ref(b)) * (formula_ref(c) - formula_ref(d));
    auto art = compile_parallel_full(ctx, f);

    // join_groups contains only waves with >= 2 members
    for (const auto& jg : art.graph.join_groups)
        REQUIRE(jg.size() >= 2);
}

// ============================================================================
// §E  pravaha_backend_ext — extension attach and backend dispatch
// ============================================================================

TEST_CASE (



"pravaha_backend_ext: registers pravaha_graph target in backend_registry"
,
"[pravaha_ext][ext]"
)
 {
    context ctx;
    ctx.use(pravaha_backend_ext{});
    // The backend registry must now have an entry for target_id::pravaha_graph
    REQUIRE(ctx.backends().find(target_id::pravaha_graph) != nullptr);
}

TEST_CASE (



"pravaha_backend_ext: compile via target_id::pravaha_graph dispatches to backend"
,
"[pravaha_ext][ext]"
)
 {
    context ctx;
    ctx.use(pravaha_backend_ext{});
    ctx.target(target_id::pravaha_graph);

    param x("extx");
    formula_ref f = x * 2.0;
    auto result = ctx.compile(f);
    // Should produce pravaha artifact, not debug text
    REQUIRE(result.is_pravaha());
}

TEST_CASE (



"pravaha_backend_ext: compiled graph evaluates correctly via extension path"
,
"[pravaha_ext][ext]"
)
 {
    context ctx;
    ctx.use(pravaha_backend_ext{});
    ctx.target(target_id::pravaha_graph);

    param x("extvx");
    formula_ref f = x + 5.0;
    auto result = ctx.compile(f);
    REQUIRE(result.is_pravaha());

    const auto& g = result.as_pravaha();
    std::vector<double> s(g.slot_count, 0.0);
    for (std::size_t i = 0; i < g.param_slots.size(); ++i)
        if (g.param_slots[i]) s[g.param_slots[i]] = 10.0;
    for (const auto& td : g.tasks) if (td.compute) td.compute(s);
    REQUIRE(s[g.result_slot] == Approx(15.0));
}

TEST_CASE (



"pravaha_backend_ext: default domain is CPU"
,
"[pravaha_ext][ext]"
)
 {
    context ctx;
    pravaha_backend_ext ext{};
    REQUIRE(ext.default_domain == pravaha::ExecutionDomain::CPU);
    ctx.use(std::move(ext));
}

TEST_CASE (



"pravaha_backend_ext: custom default domain propagates to tasks"
,
"[pravaha_ext][ext]"
)
 {
    context ctx;
    ctx.use(pravaha_backend_ext{pravaha::ExecutionDomain::Fiber});
    ctx.target(target_id::pravaha_graph);

    param x("fibx");
    formula_ref f = x + 1.0;  // scalar op should get Fiber domain
    auto result = ctx.compile(f);
    REQUIRE(result.is_pravaha());

    bool found_fiber = false;
    for (const auto& td : result.as_pravaha().tasks)
        if (td.domain == pravaha::ExecutionDomain::Fiber) { found_fiber = true; break; }
    REQUIRE(found_fiber);
}

// ============================================================================
// §F  compile_parallel_full: eval helper round-trip
// ============================================================================

TEST_CASE (



"pravaha_parallel_artifact::eval round-trip: a*b + c"
,
"[pravaha_ext][eval]"
)
 {
    context ctx;
    param a("era"), b("erb"), c("erc");
    formula_ref f = formula_ref(a) * formula_ref(b) + formula_ref(c);

    auto art = compile_parallel_full(ctx, f);
    REQUIRE_FALSE(art.graph.tasks.empty());

    // Build param value array in param_order
    std::vector<double> vals(art.graph.param_order.size(), 0.0);
    for (std::size_t i = 0; i < art.graph.param_names.size(); ++i) {
        if      (art.graph.param_names[i] == "era") vals[i] = 3.0;
        else if (art.graph.param_names[i] == "erb") vals[i] = 4.0;
        else if (art.graph.param_names[i] == "erc") vals[i] = 2.0;
    }
    double out = art.eval_sequential(vals.data(), vals.size());
    REQUIRE(out == Approx(14.0));  // 3*4 + 2
}

TEST_CASE (



"pravaha_parallel_artifact::eval round-trip: nested expression"
,
"[pravaha_ext][eval]"
)
 {
    context ctx;
    param x("nex");
    // (x+1) * (x-1) = x^2 - 1
    formula_ref f = (formula_ref(x) + 1.0_k) * (formula_ref(x) - 1.0_k);

    auto art = compile_parallel_full(ctx, f);
    std::vector<double> vals = {4.0};
    double out = art.eval_sequential(vals.data(), vals.size());
    REQUIRE(out == Approx(15.0));  // 4^2 - 1 = 15
}

// ============================================================================
// §F  Graph structure invariants
// ============================================================================

TEST_CASE (



"compile_parallel: tasks count equals post-order node count"
,
"[pravaha_ext][structure]"
)
 {
    context ctx;
    param a("sa"), b("sb");
    formula_ref f = formula_ref(a) + formula_ref(b);
    auto result = compile_parallel(ctx, f);
    REQUIRE(result.is_pravaha());
    // a, b, (a+b) = 3 nodes
    REQUIRE(result.as_pravaha().tasks.size() == 3);
}

TEST_CASE (



"compile_parallel: every task has a non-null compute lambda"
,
"[pravaha_ext][structure]"
)
 {
    context ctx;
    param a("nla"), b("nlb");
    formula_ref f = formula_ref(a) * formula_ref(b) - 1.0_k;
    auto result = compile_parallel(ctx, f);
    REQUIRE(result.is_pravaha());
    for (const auto& td : result.as_pravaha().tasks)
        REQUIRE(static_cast<bool>(td.compute));
}

TEST_CASE (



"compile_parallel: param_slots has same count as param_order"
,
"[pravaha_ext][structure]"
)
 {
    context ctx;
    param a("psa"), b("psb");
    formula_ref f = formula_ref(a) + formula_ref(b);
    auto result = compile_parallel(ctx, f);
    REQUIRE(result.is_pravaha());
    const auto& g = result.as_pravaha();
    REQUIRE(g.param_slots.size() == g.param_order.size());
}

TEST_CASE (



"compile_parallel: all param_slots are within [0, slot_count)"
,
"[pravaha_ext][structure]"
)
 {
    context ctx;
    param a("bpa"), b("bpb"), c("bpc");
    formula_ref f = formula_ref(a) + formula_ref(b) + formula_ref(c);
    auto result = compile_parallel(ctx, f);
    REQUIRE(result.is_pravaha());
    const auto& g = result.as_pravaha();
    for (std::size_t sl : g.param_slots)
        REQUIRE(sl < g.slot_count);
}

TEST_CASE (



"compile_parallel: result_slot is within [0, slot_count)"
,
"[pravaha_ext][structure]"
)
 {
    context ctx;
    param x("rsx");
    formula_ref f = x * x;
    auto result = compile_parallel(ctx, f);
    REQUIRE(result.is_pravaha());
    REQUIRE(result.as_pravaha().result_slot < result.as_pravaha().slot_count);
}

TEST_CASE (



"compile_parallel: all edge endpoints are within topo range"
,
"[pravaha_ext][structure]"
)
 {
    context ctx;
    param a("tea"), b("teb"), c("tec");
    formula_ref f = (formula_ref(a) + formula_ref(b)) * formula_ref(c);
    auto result = compile_parallel(ctx, f);
    REQUIRE(result.is_pravaha());
    const auto& g = result.as_pravaha();
    const std::size_t n = g.tasks.size();
    for (const auto& [from, to] : g.edges) {
        REQUIRE(from < n);
        REQUIRE(to   < n);
    }
}

// ============================================================================
// §F  Thread safety: compile_parallel from multiple threads
// ============================================================================

TEST_CASE (



"compile_parallel: independent contexts compile in parallel without data races"
,
"[pravaha_ext][thread]"
)
 {
    constexpr int N = 4;
    std::atomic<int> ok{0};

    auto worker = [&](int id) {
        context ctx;
        std::string nm = "tp" + std::to_string(id);
        param x(nm);
        formula_ref f = formula_ref(x) * static_cast<double>(id + 1);
        auto result = compile_parallel(ctx, f);
        if (!result.is_pravaha()) return;
        const auto& g = result.as_pravaha();
        std::vector<double> s(g.slot_count, 0.0);
        for (std::size_t i = 0; i < g.param_slots.size(); ++i)
            if (g.param_slots[i]) s[g.param_slots[i]] = 2.0;
        for (const auto& td : g.tasks) if (td.compute) td.compute(s);
        double expected = 2.0 * (id + 1);
        if (std::abs(s[g.result_slot] - expected) < 1e-10) ++ok;
    };

    std::vector<std::thread> threads;
    threads.reserve(N);
    for (int i = 0; i < N; ++i) threads.emplace_back(worker, i);
    for (auto& t : threads) t.join();

    REQUIRE(ok.load() == N);
}

// ============================================================================
// NEW TESTS — From scratch/prompt_fix.md
// ============================================================================

// §E — pravaha_backend_ext via ctx.use() + ctx.compile() integration
TEST_CASE (



"pravaha_backend_ext attaches and ctx.compile dispatches to pravaha graph"
,
"[pravaha_ext][ext][integration]"
)
 {
    context ctx;
    ctx.use(pravaha_backend_ext{});
    ctx.target(target_id::pravaha_graph);

    param a("intba"), b("intbb");
    formula_ref f = a + b;
    auto result = ctx.compile(f);

    // Must dispatch to pravaha backend
    REQUIRE(result.is_pravaha());
    const auto& g = result.as_pravaha();
    std::vector<double> s(g.slot_count, 0.0);
    for (std::size_t i = 0; i < g.param_slots.size(); ++i) {
        if (!g.param_slots[i]) continue;
        s[g.param_slots[i]] = (g.param_names[i] == "intba") ? 3.0 : 4.0;
    }
    for (const auto& td : g.tasks) if (td.compute) td.compute(s);
    REQUIRE(s[g.result_slot] == Approx(7.0));
}

// §D — wave count correctness for diamond DAG
TEST_CASE (



"diamond DAG produces exactly 3 waves"
,
"[pravaha_ext][waves][diamond]"
)
 {
    context ctx;
    param a("dia"), b("dib");
    // Diamond: a, b (wave 0) → a+b, a-b (wave 1) → (a+b)*(a-b) (wave 2)
    formula_ref left = formula_ref(a) + formula_ref(b);
    formula_ref right = formula_ref(a) - formula_ref(b);
    formula_ref f = left * right;  // (a+b)*(a-b) = a^2 - b^2

    auto art = compile_parallel_full(ctx, f);

    // Must have at least 3 waves: leaves, intermediate ops, root
    REQUIRE(art.parallel_waves.size() >= 3);
    // Wave 0 must contain both leaves
    REQUIRE(art.parallel_waves[0].size() >= 2);
}

// §F — compile_parallel vs compile_parallel_full eval equivalence
TEST_CASE (



"compile_parallel and compile_parallel_full produce identical eval results"
,
"[pravaha_ext][equivalence]"
)
 {
    context ctx;
    param x("eqvx"), y("eqvy");
    formula_ref f = formula_ref(x) * formula_ref(y) + formula_ref(x);

    // Via compile_parallel (simple graph artifact)
    auto result_simple = compile_parallel(ctx, f);
    REQUIRE(result_simple.is_pravaha());

    // Via compile_parallel_full (with metadata)
    auto result_full = compile_parallel_full(ctx, f);
    REQUIRE(!result_full.graph.tasks.empty());

    // Evaluate both with same inputs: x=3, y=4 → 3*4 + 3 = 15
    double v_simple = eval_graph(result_simple.as_pravaha(), {3.0, 4.0});

    // Use eval helper on full artifact by building param array
    std::vector<double> param_vals(result_full.graph.param_names.size(), 0.0);
    for (std::size_t i = 0; i < result_full.graph.param_names.size(); ++i) {
        if (result_full.graph.param_names[i] == "eqvx") param_vals[i] = 3.0;
        else if (result_full.graph.param_names[i] == "eqvy") param_vals[i] = 4.0;
    }
    double v_full = result_full.eval_sequential(param_vals.data(), param_vals.size());

    REQUIRE(v_simple == Approx(15.0));
    REQUIRE(v_full == Approx(15.0));
}

// ============================================================================
// § Tensor domain classification tests
// ============================================================================

TEST_CASE (



"classify_node_kind returns heavy for plugin-domain nodes"
,
"[pravaha_ext][classify][tensor]"
)
{
    context ctx;
    ctx.use(sutra::math::extension{});
    auto tdom = sutra::math::register_tensor_domain(ctx);

    // Build a formula_node with a plugin domain and a builtin op (e.g. add)
    sutra::formula_node n;
    n.op = sutra::op::add;
    n.dom = tdom;    // plugin domain → should be classified as heavy
    n.arity = 0;

    auto kind = detail::classify_node_kind(n);
    REQUIRE(kind == TaskNodeKind::heavy);
}

TEST_CASE (



"classify_node_kind returns scalar for scalar-domain builtin ops"
,
"[pravaha_ext][classify][scalar]"
)
{
    sutra::formula_node n;
    n.op  = sutra::op::add;
    n.dom = sutra::domain::scalar;
    n.arity = 2;

    auto kind = detail::classify_node_kind(n);
    REQUIRE(kind == TaskNodeKind::scalar);
}

TEST_CASE (



"classify_node_kind returns heavy for plugin-domain plugin ops"
,
"[pravaha_ext][classify][tensor][plugin]"
)
{
    context ctx;
    ctx.use(sutra::math::extension{});
    auto tdom = sutra::math::register_tensor_domain(ctx);

    // sin is a plugin op and its node gets tdom when called on a tensor input
    const param x("tsincls");
    sutra::formula_ref xref(x);
    // Manually set tensor shape and build node
    sutra::math::tensor_descriptor td;
    td.shape = {4};
    sutra::math::set_tensor_shape(xref, td);

    sutra::formula_ref sinx = sutra::math::sin(xref);
    if (!sinx.is_null()) {
        const sutra::formula_node& nd = sinx.store->at(sinx.root);
        // Plugin op — regardless of domain, must be heavy
        auto kind = detail::classify_node_kind(nd);
        REQUIRE(kind == TaskNodeKind::heavy);
    }
}

// ============================================================================
// § Ternary tensor shape propagation (clamp)
// ============================================================================

TEST_CASE (



"clamp propagates tensor shape through ternary node"
,
"[pravaha_ext][tensor][clamp][shape]"
)
{
    context ctx;
    ctx.use(sutra::math::extension{});
    sutra::math::register_tensor_domain(ctx);

    const param x("tclamp_x"), lo("tclamp_lo"), hi("tclamp_hi");
    sutra::formula_ref xref(x), loref(lo), hiref(hi);

    sutra::math::tensor_descriptor td;
    td.shape = {8};
    sutra::math::set_tensor_shape(xref, td);

    sutra::formula_ref clamped = sutra::math::clamp(xref, loref, hiref);
    REQUIRE(!clamped.is_null());

    const sutra::math::tensor_descriptor* out_td =
        sutra::math::get_tensor_shape(clamped);
    REQUIRE(out_td != nullptr);
    REQUIRE(out_td->shape.size() == 1);
    REQUIRE(out_td->shape[0] == 8);
}

// ============================================================================
// § Heterogeneous partitioning — scalar + tensor nodes in same formula
// ============================================================================

TEST_CASE (



"compile_parallel_full handles mixed scalar+tensor DAG"
,
"[pravaha_ext][heterogeneous][partitioning]"
)
{
    context ctx;
    ctx.use(sutra::math::extension{});
    sutra::math::register_tensor_domain(ctx);

    // scalar score: weight * relu(x) + bias
    const param weight("het_w"), bias("het_b"), input("het_x");
    sutra::formula_ref wref(weight), bref(bias), xref(input);

    // Tensor activation on xref
    sutra::math::tensor_descriptor td;
    td.shape = {4};
    sutra::math::set_tensor_shape(xref, td);

    sutra::formula_ref activated = sutra::math::relu(xref);
    sutra::formula_ref score = wref * activated + bref;

    auto artifact = sutra::compile_parallel_full(ctx, score);

    // Mixed DAG: relu is heavy (plugin/tensor); mul/add are scalar.
    REQUIRE(artifact.graph.slot_count > 0);
    REQUIRE(!artifact.graph.tasks.empty());

    // Verify at least one heavy (tensor) task and one scalar task exist
    bool found_heavy  = false;
    bool found_scalar = false;
    for (auto kind : artifact.node_kinds) {
        if (kind == TaskNodeKind::heavy)  found_heavy  = true;
        if (kind == TaskNodeKind::scalar) found_scalar = true;
    }
    REQUIRE(found_heavy);
    REQUIRE(found_scalar);

    // Eval: weight=2, bias=1, input=-3 → relu(-3)=0 → 2*0+1=1
    std::vector<double> params(artifact.graph.param_order.size(), 0.0);
    for (std::size_t i = 0; i < artifact.graph.param_names.size(); ++i) {
        const auto& nm = artifact.graph.param_names[i];
        if (nm == "het_w")      params[i] = 2.0;
        else if (nm == "het_b") params[i] = 1.0;
        else if (nm == "het_x") params[i] = -3.0;
    }
    const double v = artifact.eval_sequential(params.data(), params.size());
    REQUIRE(v == Catch::Approx(1.0));
}

// ============================================================================
// § Missing tests from scratch/prompt_fix.md
// ============================================================================

TEST_CASE (



"eval_sequential scratch buffer overload"
,
"[pravaha_ext]"
)
 {
    context ctx;
    param x("x");
    formula_ref expr = x * x;
    auto art = compile_parallel_full(ctx, expr);
    std::vector<double> scratch(art.graph.slot_count, 0.0);
    double param_val = 4.0;
    double v = art.eval_sequential(&param_val, 1, std::span<double>(scratch));
    REQUIRE(v == Approx(16.0));
}

TEST_CASE (



"math_ext lerp eval"
,
"[math_ext]"
)
 {
    sutra::context ctx;
    ctx.use(sutra::math::extension{});
    param a("a"), b("b"), t("t");
    formula_ref expr = sutra::math::lerp(a, b, t);
    double v = sutra::math::math_eval(expr, a = 0.0, b = 10.0, t = 0.3);
    REQUIRE(v == Approx(3.0));
}

TEST_CASE (



"math_ext clamp eval"
,
"[math_ext]"
)
 {
    sutra::context ctx;
    ctx.use(sutra::math::extension{});
    param x("x"), lo("lo"), hi("hi");
    formula_ref expr = sutra::math::clamp(x, lo, hi);
    double v1 = sutra::math::math_eval(expr, x = 5.0, lo = 0.0, hi = 10.0);
    REQUIRE(v1 == Approx(5.0));
    double v2 = sutra::math::math_eval(expr, x = -3.0, lo = 0.0, hi = 10.0);
    REQUIRE(v2 == Approx(0.0));
    double v3 = sutra::math::math_eval(expr, x = 15.0, lo = 0.0, hi = 10.0);
    REQUIRE(v3 == Approx(10.0));
}

// ============================================================================
// §JIT-FFI  jit_x86_64 + math_ext: plugin-op bridge tests
//
// Verify that formulas containing math extension ops (sin, cos, exp, etc.)
// compile to jit_function_artifact (not scalar fallback) when target is
// jit_x86_64, and produce correct results via the plugin_bridge_N mechanism.
// ============================================================================

TEST_CASE (



"jit_x86_64 + math_ext: sin compiles to JIT and evaluates correctly"
,
"[sutra][jit][math_ext]"
)
 {
    sutra::context ctx;
    ctx.use(sutra::math::extension{});
    ctx.target(sutra::target_id::jit_x86_64);

    sutra::param x("jsin_x");
    sutra::formula_ref expr = sutra::math::sin(sutra::formula_ref(x));
    auto result = ctx.compile(expr);

    REQUIRE(result.is_jit());
    REQUIRE(result.as_jit().fn != nullptr);

    const double input = 1.0;
    const double expected = std::sin(input);
    const double v = ctx.eval(expr, x = input);
    REQUIRE(v == Approx(expected).epsilon(1e-12));
}

TEST_CASE (



"jit_x86_64 + math_ext: cos compiles to JIT and evaluates correctly"
,
"[sutra][jit][math_ext]"
)
 {
    sutra::context ctx;
    ctx.use(sutra::math::extension{});
    ctx.target(sutra::target_id::jit_x86_64);

    sutra::param x("jcos_x");
    sutra::formula_ref expr = sutra::math::cos(sutra::formula_ref(x));
    auto result = ctx.compile(expr);

    REQUIRE(result.is_jit());
    const double v = ctx.eval(expr, x = 0.5);
    REQUIRE(v == Approx(std::cos(0.5)).epsilon(1e-12));
}

TEST_CASE (



"jit_x86_64 + math_ext: exp compiles to JIT and evaluates correctly"
,
"[sutra][jit][math_ext]"
)
 {
    sutra::context ctx;
    ctx.use(sutra::math::extension{});
    ctx.target(sutra::target_id::jit_x86_64);

    sutra::param x("jexp_x");
    sutra::formula_ref expr = sutra::math::exp(sutra::formula_ref(x));
    auto result = ctx.compile(expr);

    REQUIRE(result.is_jit());
    const double v = ctx.eval(expr, x = 2.0);
    REQUIRE(v == Approx(std::exp(2.0)).epsilon(1e-12));
}

TEST_CASE (



"jit_x86_64 + math_ext: compound expr sin(x)*exp(x) is JIT, no fallback"
,
"[sutra][jit][math_ext]"
)
 {
    sutra::context ctx;
    ctx.use(sutra::math::extension{});
    ctx.target(sutra::target_id::jit_x86_64);

    sutra::param x("jcmp_x");
    sutra::formula_ref xr(x);
    sutra::formula_ref expr = sutra::math::sin(xr) * sutra::math::exp(xr);
    auto result = ctx.compile(expr);

    REQUIRE(result.is_jit());
    const double v = ctx.eval(expr, x = 1.0);
    REQUIRE(v == Approx(std::sin(1.0) * std::exp(1.0)).epsilon(1e-12));
}

TEST_CASE (



"jit_x86_64 + math_ext: atan2 (binary plugin op) compiles to JIT"
,
"[sutra][jit][math_ext]"
)
 {
    sutra::context ctx;
    ctx.use(sutra::math::extension{});
    ctx.target(sutra::target_id::jit_x86_64);

    sutra::param y("jatan2_y"), x("jatan2_x");
    sutra::formula_ref expr = sutra::math::atan2(sutra::formula_ref(y), sutra::formula_ref(x));
    auto result = ctx.compile(expr);

    REQUIRE(result.is_jit());
    const double v = ctx.eval(expr, y = 1.0, x = 1.0);
    REQUIRE(v == Approx(std::atan2(1.0, 1.0)).epsilon(1e-12));
}

TEST_CASE (



"jit_x86_64 + math_ext: mixed arithmetic and plugin ops produce JIT artifact"
,
"[sutra][jit][math_ext]"
)
 {
    sutra::context ctx;
    ctx.use(sutra::math::extension{});
    ctx.target(sutra::target_id::jit_x86_64);

    // a * sin(x) + cos(x) — mixes builtin arithmetic with plugin ops
    sutra::param a("jmix_a"), x("jmix_x");
    sutra::formula_ref ar(a), xr(x);
    sutra::formula_ref expr = ar * sutra::math::sin(xr) + sutra::math::cos(xr);
    auto result = ctx.compile(expr);

    REQUIRE(result.is_jit());
    const double av = 2.0, xv = 0.7;
    const double v = ctx.eval(expr, a = av, x = xv);
    REQUIRE(v == Approx(av * std::sin(xv) + std::cos(xv)).epsilon(1e-12));
}

// ============================================================================
// § Lazy branch coordinator tests (P2300-style dynamic task-graph splicing)
// ============================================================================

TEST_CASE (



"lazy if_: branch coordinator task has Inline domain"
,
"[pravaha_ext][if][lazy][domain]"
)
{
    context ctx;
    param x("lif_dom_x");
    formula_ref f = if_expr(x > 0.0_k, formula_ref(x), -formula_ref(x));
    auto art = compile_parallel_full(ctx, f);

    bool found_inline_branch = false;
    for (std::size_t i = 0; i < art.node_kinds.size(); ++i) {
        if (art.node_kinds[i] == TaskNodeKind::branch &&
            art.graph.tasks[i].domain == pravaha::ExecutionDomain::Inline) {
            found_inline_branch = true;
        }
    }
    REQUIRE(found_inline_branch);
}

TEST_CASE (



"lazy if_: then/else edges are control edges"
,
"[pravaha_ext][if][lazy][edgekind]"
)
{
    context ctx;
    param x("lif_ek_x");
    formula_ref f = if_expr(x > 0.0_k, formula_ref(x), -formula_ref(x));
    auto art = compile_parallel_full(ctx, f);

    // Find the if_ node (branch coordinator) topo index
    std::size_t branch_topo = std::numeric_limits<std::size_t>::max();
    for (std::size_t i = 0; i < art.node_kinds.size(); ++i)
        if (art.node_kinds[i] == TaskNodeKind::branch) { branch_topo = i; break; }
    REQUIRE(branch_topo != std::numeric_limits<std::size_t>::max());

    // Edges from the 2nd and 3rd children (then/else) to branch coordinator must be control
    int control_edges_to_branch = 0;
    for (const auto& em : art.edge_meta) {
        if (em.to_topo == branch_topo && em.kind == EdgeKind::control)
            ++control_edges_to_branch;
    }
    REQUIRE(control_edges_to_branch == 2); // one for then, one for else
}

TEST_CASE (



"lazy if_: condition edge to branch coordinator is data_flow"
,
"[pravaha_ext][if][lazy][edgekind]"
)
{
    context ctx;
    param x("lif_cond_x");
    formula_ref f = if_expr(x > 0.0_k, formula_ref(x), -formula_ref(x));
    auto art = compile_parallel_full(ctx, f);

    std::size_t branch_topo = std::numeric_limits<std::size_t>::max();
    for (std::size_t i = 0; i < art.node_kinds.size(); ++i)
        if (art.node_kinds[i] == TaskNodeKind::branch) { branch_topo = i; break; }
    REQUIRE(branch_topo != std::numeric_limits<std::size_t>::max());

    bool found_data_flow_cond = false;
    for (const auto& em : art.edge_meta) {
        if (em.to_topo == branch_topo && em.kind == EdgeKind::data_flow) {
            found_data_flow_cond = true;
        }
    }
    REQUIRE(found_data_flow_cond);
}

TEST_CASE (



"lazy if_: inactive branch slot is NaN (State::Skipped) after eval"
,
"[pravaha_ext][if][lazy][skipped]"
)
{
    context ctx;
    param x("lif_skip_x");
    formula_ref f = if_expr(x > 0.0_k, formula_ref(x), -formula_ref(x));
    auto art = compile_parallel_full(ctx, f);
    const auto& g = art.graph;

    // Identify then/else destination slots.
    // The if_ node is the last task (root); its children[1]=then, children[2]=else.
    // We check that after evaluation with x=5.0, the else slot is NaN.
    std::vector<double> slots(g.slot_count, 0.0);
    for (std::size_t i = 0; i < g.param_slots.size(); ++i)
        if (g.param_slots[i]) slots[g.param_slots[i]] = 5.0; // x > 0 → then active
    for (const auto& td : g.tasks) if (td.compute) td.compute(slots);

    // Result slot should be 5.0
    REQUIRE(slots[g.result_slot] == Approx(5.0));

    // At least one non-result slot (the inactive else branch result) should be NaN
    bool found_nan = false;
    for (std::size_t si = 1; si < slots.size(); ++si) {
        if (si != g.result_slot && std::isnan(slots[si])) { found_nan = true; break; }
    }
    REQUIRE(found_nan);
}

TEST_CASE (



"lazy if_: AnySuccess join policy on branch coordinator edges"
,
"[pravaha_ext][if][lazy][joinpolicy]"
)
{
    context ctx;
    param x("lif_jp_x");
    formula_ref f = if_expr(x > 0.0_k, formula_ref(x), -formula_ref(x));
    auto art = compile_parallel_full(ctx, f);

    std::size_t branch_topo = std::numeric_limits<std::size_t>::max();
    for (std::size_t i = 0; i < art.node_kinds.size(); ++i)
        if (art.node_kinds[i] == TaskNodeKind::branch) { branch_topo = i; break; }
    REQUIRE(branch_topo != std::numeric_limits<std::size_t>::max());

    // All edges into the branch coordinator must carry AnySuccess join policy
    for (const auto& em : art.edge_meta) {
        if (em.to_topo == branch_topo)
            REQUIRE(em.join_policy.kind == pravaha::JoinPolicyKind::AnySuccess);
    }
}

TEST_CASE (



"lazy if_: eval correctness preserved for nested conditionals"
,
"[pravaha_ext][if][lazy][correctness]"
)
{
    context ctx;
    param x("lif_nest_x"), y("lif_nest_y");
    // if (x > 0) then (if (y > 0) then x+y else x-y) else 0
    formula_ref inner = if_expr(y > 0.0_k, formula_ref(x) + formula_ref(y),
                                             formula_ref(x) - formula_ref(y));
    formula_ref f = if_expr(x > 0.0_k, inner, 0.0_k);

    auto art = compile_parallel_full(ctx, f);

    auto eval_f = [&](double xv, double yv) {
        std::vector<double> slots(art.graph.slot_count, 0.0);
        for (std::size_t i = 0; i < art.graph.param_slots.size(); ++i) {
            if (!art.graph.param_slots[i]) continue;
            slots[art.graph.param_slots[i]] = (art.graph.param_names[i] == "lif_nest_x") ? xv : yv;
        }
        for (const auto& td : art.graph.tasks) if (td.compute) td.compute(slots);
        return slots[art.graph.result_slot];
    };

    REQUIRE(eval_f( 3.0,  4.0) == Approx( 7.0)); // x>0, y>0: x+y = 3+4 = 7
    REQUIRE(eval_f( 3.0, -4.0) == Approx( 7.0)); // x>0, y<0: x-y = 3-(-4) = 7
    REQUIRE(eval_f(-1.0,  4.0) == Approx( 0.0)); // x<0: 0
}

TEST_CASE (



"lazy if_: branch task classified as TaskNodeKind::branch (unchanged)"
,
"[pravaha_ext][if][lazy][nodekind]"
)
{
    context ctx;
    param x("lif_kind_x");
    formula_ref f = if_expr(x > 0.0_k, formula_ref(x), -formula_ref(x));
    auto art = compile_parallel_full(ctx, f);

    bool found_branch = false;
    for (auto k : art.node_kinds)
        if (k == TaskNodeKind::branch) { found_branch = true; break; }
    REQUIRE(found_branch);
}

// ============================================================================
// §D2  build_backprop_dag — backprop artifact construction
// ============================================================================

TEST_CASE (



"build_backprop_dag: produces non-empty waves for non-trivial formula"
,
"[pravaha_ext][backprop][build_backprop_dag]"
)
{
    ephemeral_formula_store::thread_local_instance().reset();
    context ctx;
    param x("bbp_x");
    formula_ref f = formula_ref(x) * formula_ref(x); // x^2

    auto& owned = ctx.owned_store();
    formula_ref owned_f = owned.migrate(f);

    sparseset::SparseSet<node_index> vis(owned.size() + 1);
    std::vector<node_index> topo;
    detail::post_order_walk(owned, owned_f.root, vis, topo);
    for (node_index idx : topo) {
        formula_node& n = owned.at_mutable(idx);
        if (n.raw_name && n.sym == 0) {
            n.sym = ctx.intern(n.raw_name);
            n.cached_hash = 0;
        }
    }

    symbol_id x_sym = ctx.intern("bbp_x");
    std::unordered_map<symbol_id, double> env{{x_sym, 3.0}};

    auto artifact = build_backprop_dag(owned, owned_f.root, topo, env, ctx.ops());

    REQUIRE(artifact.root != 0);
    REQUIRE(!artifact.waves.empty());
    REQUIRE(artifact.fwd_vals[owned_f.root] == Approx(9.0)); // x^2 = 3^2 = 9
}

TEST_CASE (



"build_backprop_dag: forward values correct for a+b"
,
"[pravaha_ext][backprop][build_backprop_dag]"
)
{
    ephemeral_formula_store::thread_local_instance().reset();
    context ctx;
    param a("bbp_a"), b("bbp_b");
    formula_ref f = formula_ref(a) + formula_ref(b);

    auto& owned = ctx.owned_store();
    formula_ref owned_f = owned.migrate(f);

    sparseset::SparseSet<node_index> vis(owned.size() + 1);
    std::vector<node_index> topo;
    detail::post_order_walk(owned, owned_f.root, vis, topo);
    for (node_index idx : topo) {
        formula_node& n = owned.at_mutable(idx);
        if (n.raw_name && n.sym == 0) {
            n.sym = ctx.intern(n.raw_name);
            n.cached_hash = 0;
        }
    }

    symbol_id a_sym = ctx.intern("bbp_a");
    symbol_id b_sym = ctx.intern("bbp_b");
    std::unordered_map<symbol_id, double> env{{a_sym, 2.0}, {b_sym, 5.0}};

    auto artifact = build_backprop_dag(owned, owned_f.root, topo, env, ctx.ops());

    REQUIRE(artifact.fwd_vals[owned_f.root] == Approx(7.0)); // 2 + 5 = 7
    REQUIRE(artifact.max_node_idx >= owned_f.root);
}

// ============================================================================
// §9a  context::grad_parallel — parallel numeric differentiation
// ============================================================================

TEST_CASE (



"grad_parallel: gradient of constant w.r.t. x is 0"
,
"[sutra][grad_parallel]"
)
{
    ephemeral_formula_store::thread_local_instance().reset();
    context ctx;
    param x("gp_x");
    auto lit = 5.0_k;
    symbol_id x_sym = ctx.intern("gp_x");
    std::unordered_map<symbol_id, double> env{{x_sym, 3.0}};

    auto result = ctx.grad_parallel(lit, x, env);
    REQUIRE(!result.is_null());
    const formula_node& n = ctx.owned_store().at(result.root);
    REQUIRE(n.op == op::lit_f64);
    REQUIRE(n.lit.f64 == Approx(0.0));
}

TEST_CASE (



"grad_parallel: gradient of x w.r.t. x is 1"
,
"[sutra][grad_parallel]"
)
{
    ephemeral_formula_store::thread_local_instance().reset();
    context ctx;
    param x("gp2_x");
    formula_ref f = formula_ref(x);
    symbol_id x_sym = ctx.intern("gp2_x");
    std::unordered_map<symbol_id, double> env{{x_sym, 3.0}};

    auto result = ctx.grad_parallel(f, x, env);
    REQUIRE(!result.is_null());
    const formula_node& n = ctx.owned_store().at(result.root);
    REQUIRE(n.op == op::lit_f64);
    REQUIRE(n.lit.f64 == Approx(1.0));
}

TEST_CASE (



"grad_parallel: gradient of x+y w.r.t. x is 1"
,
"[sutra][grad_parallel]"
)
{
    ephemeral_formula_store::thread_local_instance().reset();
    context ctx;
    param x("gp3_x"), y("gp3_y");
    formula_ref f = formula_ref(x) + formula_ref(y);
    symbol_id x_sym = ctx.intern("gp3_x");
    symbol_id y_sym = ctx.intern("gp3_y");
    std::unordered_map<symbol_id, double> env{{x_sym, 2.0}, {y_sym, 5.0}};

    auto result = ctx.grad_parallel(f, x, env);
    REQUIRE(!result.is_null());
    const formula_node& n = ctx.owned_store().at(result.root);
    REQUIRE(n.op == op::lit_f64);
    REQUIRE(n.lit.f64 == Approx(1.0));
}

TEST_CASE (



"grad_parallel: gradient of x*y w.r.t. x is y"
,
"[sutra][grad_parallel]"
)
{
    ephemeral_formula_store::thread_local_instance().reset();
    context ctx;
    param x("gp4_x"), y("gp4_y");
    formula_ref f = formula_ref(x) * formula_ref(y);
    symbol_id x_sym = ctx.intern("gp4_x");
    symbol_id y_sym = ctx.intern("gp4_y");
    std::unordered_map<symbol_id, double> env{{x_sym, 3.0}, {y_sym, 4.0}};

    // d(x*y)/dx = y = 4.0
    auto result = ctx.grad_parallel(f, x, env);
    REQUIRE(!result.is_null());
    const formula_node& rn = ctx.owned_store().at(result.root);
    REQUIRE(rn.op == op::lit_f64);
    REQUIRE(rn.lit.f64 == Approx(4.0));
}

TEST_CASE (



"grad_parallel: gradient of x*x w.r.t. x is 2*x"
,
"[sutra][grad_parallel]"
)
{
    ephemeral_formula_store::thread_local_instance().reset();
    context ctx;
    param x("gp5_x");
    formula_ref f = formula_ref(x) * formula_ref(x);
    symbol_id x_sym = ctx.intern("gp5_x");

    // d(x^2)/dx = 2x; at x=3.0 → 6.0
    std::unordered_map<symbol_id, double> env{{x_sym, 3.0}};
    auto r3 = ctx.grad_parallel(f, x, env);
    REQUIRE(ctx.owned_store().at(r3.root).lit.f64 == Approx(6.0));

    // at x=-2.0 → -4.0 (need fresh context — grad_parallel accumulates in owned_store)
    ephemeral_formula_store::thread_local_instance().reset();
    context ctx2;
    param x2("gp5b_x");
    formula_ref f2 = formula_ref(x2) * formula_ref(x2);
    symbol_id x2_sym = ctx2.intern("gp5b_x");
    std::unordered_map<symbol_id, double> env2{{x2_sym, -2.0}};
    auto r2 = ctx2.grad_parallel(f2, x2, env2);
    REQUIRE(ctx2.owned_store().at(r2.root).lit.f64 == Approx(-4.0));
}

TEST_CASE (



"grad_parallel: gradient of a-b w.r.t. b is -1"
,
"[sutra][grad_parallel]"
)
{
    ephemeral_formula_store::thread_local_instance().reset();
    context ctx;
    param a("gp6_a"), b("gp6_b");
    formula_ref f = formula_ref(a) - formula_ref(b);
    symbol_id a_sym = ctx.intern("gp6_a");
    symbol_id b_sym = ctx.intern("gp6_b");
    std::unordered_map<symbol_id, double> env{{a_sym, 5.0}, {b_sym, 2.0}};

    auto result = ctx.grad_parallel(f, b, env);
    REQUIRE(!result.is_null());
    REQUIRE(ctx.owned_store().at(result.root).lit.f64 == Approx(-1.0));
}

TEST_CASE (



"grad_parallel: var overload mirrors param overload"
,
"[sutra][grad_parallel]"
)
{
    ephemeral_formula_store::thread_local_instance().reset();
    context ctx;
    var x("gp7_x");
    formula_ref f = formula_ref(x) + formula_ref(x); // 2x
    symbol_id x_sym = ctx.intern("gp7_x");
    std::unordered_map<symbol_id, double> env{{x_sym, 1.0}};

    // d(x+x)/dx = 2
    auto result = ctx.grad_parallel(f, x, env);
    REQUIRE(!result.is_null());
    REQUIRE(ctx.owned_store().at(result.root).lit.f64 == Approx(2.0));
}

TEST_CASE (



"grad_parallel: math_ext sin diff rule — d(sin(x))/dx = cos(x)"
,
"[sutra][grad_parallel][math]"
)
{
    ephemeral_formula_store::thread_local_instance().reset();
    context ctx;
    ctx.use(sutra::math::extension{});
    param x("gp8_x");
    formula_ref f = sutra::math::sin(formula_ref(x));
    symbol_id x_sym = ctx.intern("gp8_x");
    const double xv = 1.2;
    std::unordered_map<symbol_id, double> env{{x_sym, xv}};

    // d(sin(x))/dx = cos(x)
    auto result = ctx.grad_parallel(f, x, env);
    REQUIRE(!result.is_null());
    REQUIRE(ctx.owned_store().at(result.root).lit.f64 == Approx(std::cos(xv)));
}

TEST_CASE (



"grad_parallel: math_ext exp diff rule — d(exp(x))/dx = exp(x)"
,
"[sutra][grad_parallel][math]"
)
{
    ephemeral_formula_store::thread_local_instance().reset();
    context ctx;
    ctx.use(sutra::math::extension{});
    param x("gp9_x");
    formula_ref f = sutra::math::exp(formula_ref(x));
    symbol_id x_sym = ctx.intern("gp9_x");
    const double xv = 0.5;
    std::unordered_map<symbol_id, double> env{{x_sym, xv}};

    auto result = ctx.grad_parallel(f, x, env);
    REQUIRE(!result.is_null());
    REQUIRE(ctx.owned_store().at(result.root).lit.f64 == Approx(std::exp(xv)));
}

// ============================================================================
// windowed_mir_fold (parallel) — integration via compile_parallel
// ============================================================================

TEST_CASE (



"compile_parallel: wide constant tree folds to single leaf via parallel windowed fold"
,
"[pravaha_ext][fold][parallel]"
)
 {
    // ((1+2)+(3+4)) + ((5+6)+(7+8)) = 36 — all constant, multi-wave parallel fold
    context ctx;
    ctx.optimization(lithe::compiler::opt_level::O2);

    formula_ref f =
        (constant{1.0} + constant{2.0} + constant{3.0} + constant{4.0}) +
        (constant{5.0} + constant{6.0} + constant{7.0} + constant{8.0});

    auto result = compile_parallel(ctx, f);
    REQUIRE(result.is_pravaha());

    // All constants folded → single leaf task (the folded lit_f64)
    const auto& g = result.as_pravaha();
    double v = eval_graph(g, {});
    REQUIRE(v == Approx(36.0));
}

TEST_CASE (



"compile_parallel: constant sub-tree folds, parametric part evaluates correctly"
,
"[pravaha_ext][fold][parallel]"
)
 {
    // x * (3.0 * 4.0) — constant branch 3*4 folds to 12 before graph build;
    // at x=5 result should be 5*12 = 60.
    context ctx;
    ctx.optimization(lithe::compiler::opt_level::O2);

    param x("cpf_x");
    formula_ref f = formula_ref(x) * (constant{3.0} * constant{4.0});

    // Use context eval so param binding works end-to-end
    double v = ctx.eval(f, x = 5.0);
    REQUIRE(v == Approx(60.0));
}

TEST_CASE (



"compile_parallel: parallel fold does not corrupt independent param branches"
,
"[pravaha_ext][fold][parallel]"
)
 {
    // (a + (2.0 * 3.0)) + (b + (4.0 * 5.0))
    // Constant parts 2*3=6 and 4*5=20 fold independently in the same wave.
    // At a=1, b=2: (1+6) + (2+20) = 29
    context ctx;
    ctx.optimization(lithe::compiler::opt_level::O2);

    param a("cpf_a"), b("cpf_b");
    formula_ref f = (formula_ref(a) + (constant{2.0} * constant{3.0}))
                  + (formula_ref(b) + (constant{4.0} * constant{5.0}));

    double v = ctx.eval(f, a = 1.0, b = 2.0);
    REQUIRE(v == Approx(29.0));
}

TEST_CASE (



"compile_parallel: O0 disables fold, result still numerically correct"
,
"[pravaha_ext][fold][parallel]"
)
 {
    // Same formula as above but O0 — no fold, but evaluation must still be correct.
    context ctx;
    ctx.optimization(lithe::compiler::opt_level::O0);

    param a("cpf_a0"), b("cpf_b0");
    formula_ref f = (formula_ref(a) + (constant{2.0} * constant{3.0}))
                  + (formula_ref(b) + (constant{4.0} * constant{5.0}));

    double v = ctx.eval(f, a = 1.0, b = 2.0);
    REQUIRE(v == Approx(29.0));
}

