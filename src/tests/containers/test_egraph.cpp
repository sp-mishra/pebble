// =============================================================================
// test_egraph.cpp — Generic E-Graph Engine Unit Tests
//
// Verifies: containers/graph/egraph.hpp (namespace egraph)
// No Lithe dependency — pure container test.
//
// Cases:
//   1. union-find: merge → find agrees; congruence propagates after rebuild.
//   2. hashcons: structurally-equal e-nodes → same e_class_id.
//   3. generic commutativity rule collapses op(a,b) and op(b,a).
//   4. saturation_limits: max_iters=0 → no rewrites; tight max_enodes halts.
//   5. extract_best<node_count_cost> picks min-node form.
// =============================================================================

#include "catch_amalgamated.hpp"
#include "containers/graph/egraph.hpp"
#include "mem/smriti.hpp"
#include "mem/arena.hpp"

// ---------------------------------------------------------------------------
// Helpers: simple test op-ids
// ---------------------------------------------------------------------------
namespace {
    enum : std::size_t { kAdd = 1, kMul = 2, kLeaf = 0 };

    // Simple op traits for commutativity tests
    struct add_op_traits {
        static constexpr std::size_t commutative_op = kAdd;
        static constexpr std::size_t associative_op = kAdd;
        static constexpr std::size_t add_op = kAdd;
        static constexpr std::size_t mul_op = kMul;
        static constexpr std::size_t zero_op = kLeaf;
        static constexpr std::size_t one_op = kLeaf;
        static constexpr std::size_t zero_payload = 0;
        static constexpr std::size_t one_payload = 1;
    };

    using TestGraph = egraph::e_graph<std::size_t, std::size_t>;

    // Build a leaf e_node with given payload.
    egraph::e_node<> leaf(std::size_t v) {
        egraph::e_node<> n;
        n.op = kLeaf;
        n.payload = v;
        return n;
    }

    // Build a binary e_node.
    egraph::e_node<> binop(std::size_t op,
                           egraph::e_class_id a,
                           egraph::e_class_id b) {
        egraph::e_node<> n;
        n.op = op;
        n.children.push_back(a);
        n.children.push_back(b);
        return n;
    }
} // namespace

// ============================================================================
// Test 1 — union-find: merge + find
// ============================================================================

TEST_CASE (



"union-find: merge and find are consistent"
,
"[egraph][union-find]"
)
{
    TestGraph g;
    auto la = g.add(leaf(10));
    auto lb = g.add(leaf(20));
    auto lc = g.add(leaf(30));

    REQUIRE(g.find(la) == la);
    REQUIRE(g.find(lb) == lb);

    (void)g.merge(la, lb);
    g.rebuild();

    // After merge, both la and lb refer to the same root.
    REQUIRE(g.find(la) == g.find(lb));

    // lc is still distinct.
    REQUIRE(g.find(lc) != g.find(la));
}

TEST_CASE (



"congruence: merging child classes propagates to parent nodes after rebuild"
,
"[egraph][congruence]"
)
{
    TestGraph g;

    // Build: add(x, y) and add(x, z) where y and z will be merged.
    auto x = g.add(leaf(1));
    auto y = g.add(leaf(2));
    auto z = g.add(leaf(3));

    auto add_xy = g.add(binop(kAdd, x, y));
    auto add_xz = g.add(binop(kAdd, x, z));

    REQUIRE(g.find(add_xy) != g.find(add_xz)); // distinct before merge

    (void)g.merge(y, z);
    g.rebuild();

    // After merging y≡z and rebuilding, add(x,y) ≡ add(x,z)
    REQUIRE(g.find(add_xy) == g.find(add_xz));
}

// ============================================================================
// Test 2 — hashcons: structural equality and distinctness
// ============================================================================

TEST_CASE (



"hashcons: structurally-equal e-nodes → same e_class_id"
,
"[egraph][hashcons]"
)
{
    TestGraph g;
    auto a = g.add(leaf(42));
    auto b = g.add(leaf(42)); // identical
    REQUIRE(a == b);
}

TEST_CASE (



"hashcons: distinct payload → distinct e_class_id"
,
"[egraph][hashcons]"
)
{
    TestGraph g;
    auto a = g.add(leaf(1));
    auto b = g.add(leaf(2));
    REQUIRE(a != b);
}

TEST_CASE (



"hashcons: structurally-equal op-nodes → same e_class_id"
,
"[egraph][hashcons]"
)
{
    TestGraph g;
    auto x = g.add(leaf(10));
    auto y = g.add(leaf(20));

    auto n1 = g.add(binop(kAdd, x, y));
    auto n2 = g.add(binop(kAdd, x, y));
    REQUIRE(n1 == n2);
}

// ============================================================================
// Test 3 — commutativity rule
// ============================================================================

TEST_CASE (



"commutativity rule collapses op(a,b) and op(b,a)"
,
"[egraph][rules]"
)
{
    TestGraph g;
    auto a = g.add(leaf(1));
    auto b = g.add(leaf(2));

    auto ab = g.add(binop(kAdd, a, b));
    auto ba = g.add(binop(kAdd, b, a));

    REQUIRE(g.find(ab) != g.find(ba)); // distinct before saturation

    auto rules = std::make_tuple(
        egraph::commutativity<add_op_traits, TestGraph>{}
    );
    (void)egraph::saturate(g, rules, egraph::saturation_limits{.max_iters = 5});

    REQUIRE(g.find(ab) == g.find(ba)); // same class after saturation
}

// ============================================================================
// Test 4 — saturation_limits
// ============================================================================

TEST_CASE (



"saturation_limits: max_iters=0 → no rewrites performed"
,
"[egraph][limits]"
)
{
    TestGraph g;
    auto a = g.add(leaf(1));
    auto b = g.add(leaf(2));
    auto ab = g.add(binop(kAdd, a, b));
    auto ba = g.add(binop(kAdd, b, a));

    auto rules = std::make_tuple(
        egraph::commutativity<add_op_traits, TestGraph>{}
    );
    const auto report = egraph::saturate(g,
        rules,
        egraph::saturation_limits{.max_iters = 0});

    // With 0 max_iters, fixpoint loop never executes.
    REQUIRE(report.iters == 0);
    REQUIRE(g.find(ab) != g.find(ba));
}

TEST_CASE (



"saturation_limits: tight max_enodes halts and sets hit_limit=true"
,
"[egraph][limits]"
)
{
    TestGraph g;
    auto a = g.add(leaf(1));
    auto b = g.add(leaf(2));
    (void)g.add(binop(kAdd, a, b));
    (void)g.add(binop(kAdd, b, a));

    auto rules = std::make_tuple(
        egraph::commutativity<add_op_traits, TestGraph>{}
    );
    // max_enodes=1 is tight — the commutativity rule will add new nodes
    const auto report = egraph::saturate(g,
        rules,
        egraph::saturation_limits{.max_iters = 10, .max_enodes = 1});

    REQUIRE(report.hit_limit == true);
}

// ============================================================================
// Test 5 — extract_best<node_count_cost>
// ============================================================================

TEST_CASE (



"extract_best: node_count_cost picks min-node form"
,
"[egraph][extract]"
)
{
    // Build: add(x, 0) — then after identity_zero saturation we get x.
    // Without saturation, test that extraction works and picks the simpler form
    // when we manually merge add(x,zero) with x.

    TestGraph g;
    auto x    = g.add(leaf(7));
    auto zero = g.add(leaf(0));
    auto add_x0 = g.add(binop(kAdd, x, zero));

    // Manually merge add(x,0) with x (simulating identity_zero result).
    (void)g.merge(add_x0, x);
    g.rebuild();

    const auto result = egraph::extract_best(g, add_x0);
    const auto root   = g.find(add_x0);

    REQUIRE(result.best_nodes[root].has_value());
    // The extracted form should be the leaf (cost=1) not the add (cost=3).
    REQUIRE(result.best_costs[root] == 1u);
    REQUIRE(result.best_nodes[root]->children.empty()); // it's a leaf
}

TEST_CASE (



"extract_best: works on simple tree"
,
"[egraph][extract]"
)
{
    TestGraph g;
    auto a = g.add(leaf(3));
    auto b = g.add(leaf(4));
    auto ab = g.add(binop(kMul, a, b));

    const auto result = egraph::extract_best(g, ab);
    const auto root   = g.find(ab);

    REQUIRE(result.best_nodes[root].has_value());
    // mul(3,4): cost = 1(mul) + 1(3) + 1(4) = 3
    REQUIRE(result.best_costs[root] == 3u);
}

// ============================================================================
// Test — saturation_report fields
// ============================================================================

TEST_CASE (



"saturation_report: saturated=true on fixpoint"
,
"[egraph][report]"
)
{
    TestGraph g;
    auto x = g.add(leaf(1));
    auto y = g.add(leaf(2));
    (void)g.add(binop(kAdd, x, y));

    // Empty rule tuple → fixpoint on iteration 1
    auto report = egraph::saturate(g, std::tuple<>{},
                                   egraph::saturation_limits{.max_iters = 10});
    REQUIRE(report.saturated == true);
    REQUIRE(report.hit_limit == false);
}

TEST_CASE (
"e_graph: smriti arena allocator integration"
,
"[egraph][mem][arena]"
)
 {
    using Resource = smriti::ManagedResource<smriti::domains::SystemRAMDomain,
                                            smriti::pools::BumpPool<smriti::domains::SystemRAMDomain>>;
    Resource res{smriti::domains::SystemRAMDomain{},
                 smriti::pools::BumpPool<smriti::domains::SystemRAMDomain>{64 * 1024}};

    using ArenaAlloc = smriti::SmritiAllocator<char, Resource>;
    using ArenaGraph = egraph::e_graph<std::size_t, std::size_t,
                                       egraph::default_enode_hash<std::size_t, std::size_t>,
                                       egraph::default_enode_eq<std::size_t, std::size_t>,
                                       std::monostate,
                                       ArenaAlloc>;

    ArenaAlloc alloc{res};
    ArenaGraph g{alloc};

    auto a = g.add(leaf(10));
    auto b = g.add(leaf(20));
    auto sum = g.add(binop(kAdd, a, b));

    REQUIRE(g.find(a) != g.find(b));
    REQUIRE(g.find(sum) != g.find(a));

    (void)g.merge(a, b);
    g.rebuild();

    REQUIRE(g.find(a) == g.find(b));
    REQUIRE(g.class_count_live() == 2);
}

// ============================================================================
// Test — egraph_analysis concept, applicability, and saturation_budget
// ============================================================================

struct ConstantFoldingAnalysis {
    using data_type = std::optional<std::size_t>;

    data_type make(const egraph::e_node<std::size_t, std::size_t>& node,
                   std::span<const data_type> children) {
        if (node.op == kLeaf) {
            return node.payload;
        }
        if (node.op == kAdd && children.size() == 2 && children[0] && children[1]) {
            return *children[0] + *children[1];
        }
        return std::nullopt;
    }

    bool merge(data_type& target, const data_type& other) {
        if (!target && other) {
            target = other;
            return true;
        }
        return false;
    }
};

static_assert(egraph::egraph_analysis<ConstantFoldingAnalysis, egraph::e_node<std::size_t, std::size_t>>);

TEST_CASE("egraph: saturation_budget and stop_reason", "[egraph][budget]") {
    TestGraph g;
    auto a = g.add(leaf(1));
    auto b = g.add(leaf(2));
    (void)g.add(binop(kAdd, a, b));

    egraph::saturation_budget budget{
        .max_iterations = 5,
        .max_enodes = 1000,
        .max_eclasses = 500,
        .max_time = std::chrono::milliseconds(500)
    };

    auto report = egraph::saturate(g, std::tuple<>{}, budget);
    REQUIRE(report.saturated == true);
    REQUIRE(report.stop_reason == egraph::saturation_stop_reason::fixpoint);
    REQUIRE(report.hit_limit == false);

    egraph::egraph_workspace workspace{};
    workspace.reset();
}

TEST_CASE("egraph: applicability enum", "[egraph][applicability]") {
    REQUIRE(egraph::applicability::proven != egraph::applicability::disproven);
    REQUIRE(egraph::applicability::unknown != egraph::applicability::proven);
}

