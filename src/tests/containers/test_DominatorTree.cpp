#define CATCH_CONFIG_MAIN
#include <catch_amalgamated.hpp>
#include "containers/graph/LiteGraph.hpp"
#include "containers/graph/DominatorTree.hpp"
#include <queue>
#include <random>
#include <unordered_map>

using namespace litegraph;

namespace {
    using IdomMap = std::unordered_map<size_t, size_t>;

    auto extract_idoms_from_tree(const NAryTree<NodeId>& dom_tree) -> IdomMap {
        IdomMap idom;
        const auto* root = dom_tree.get_root();
        if (!root) {
            return idom;
        }

        std::queue<const NAryTree<NodeId>::TreeNode*> q;
        q.push(root);
        idom[root->data.value] = root->data.value;

        while (!q.empty()) {
            const auto* node = q.front();
            q.pop();

            for (const auto& child_ptr : node->children) {
                const auto* child = child_ptr.get();
                idom[child->data.value] = node->data.value;
                q.push(child);
            }
        }

        return idom;
    }

    auto reference_idoms(const Graph<int, int, Directed>& g, const NodeId start) -> IdomMap {
        const auto cap = g.node_capacity();
        std::vector<bool> reachable(cap, false);

        std::queue<NodeId> q;
        q.push(start);
        reachable[start.value] = true;
        while (!q.empty()) {
            const NodeId u = q.front();
            q.pop();
            for (const auto v : g.neighbors(u)) {
                if (!reachable[v.value]) {
                    reachable[v.value] = true;
                    q.push(v);
                }
            }
        }

        std::vector<NodeId> nodes;
        for (size_t i = 0; i < cap; ++i) {
            if (reachable[i]) {
                nodes.emplace_back(i);
            }
        }

        auto can_reach_avoiding = [&](const NodeId target, const NodeId blocked) {
            if (start.value == blocked.value) {
                return false;
            }

            std::vector<bool> seen(cap, false);
            std::queue<NodeId> bfs;
            bfs.push(start);
            seen[start.value] = true;

            while (!bfs.empty()) {
                const NodeId u = bfs.front();
                bfs.pop();

                if (u.value == target.value) {
                    return true;
                }

                for (const auto v : g.neighbors(u)) {
                    if (v.value == blocked.value || seen[v.value]) {
                        continue;
                    }
                    seen[v.value] = true;
                    bfs.push(v);
                }
            }

            return false;
        };

        std::vector<std::vector<bool>> dominates(cap, std::vector<bool>(cap, false));
        for (const auto d : nodes) {
            for (const auto n : nodes) {
                if (d.value == n.value) {
                    dominates[d.value][n.value] = true;
                }
                else {
                    dominates[d.value][n.value] = !can_reach_avoiding(n, d);
                }
            }
        }

        IdomMap idom;
        idom[start.value] = start.value;

        for (const auto n : nodes) {
            if (n.value == start.value) {
                continue;
            }

            std::vector<NodeId> strict_doms;
            for (const auto d : nodes) {
                if (d.value != n.value && dominates[d.value][n.value]) {
                    strict_doms.push_back(d);
                }
            }

            NodeId immediate;
            bool found = false;
            for (const auto candidate : strict_doms) {
                bool candidate_is_immediate = true;
                for (const auto other : strict_doms) {
                    if (other.value != candidate.value && dominates[candidate.value][other.value]) {
                        candidate_is_immediate = false;
                        break;
                    }
                }
                if (candidate_is_immediate) {
                    immediate = candidate;
                    found = true;
                    break;
                }
            }

            REQUIRE(found);
            idom[n.value] = immediate.value;
        }

        return idom;
    }

    void require_idoms_equal(const IdomMap& actual, const IdomMap& expected) {
        REQUIRE(actual.size() == expected.size());
        for (const auto& [node, expected_parent] : expected) {
            INFO("node=" << node);
            REQUIRE(actual.contains(node));
            REQUIRE(actual.at(node) == expected_parent);
        }
    }
}

TEST_CASE (



"[DominatorTree] Simple linear graph"
,
"[DominatorTree]"
)
 {
    // Graph: 0 -> 1 -> 2 -> 3
    Graph<int, int, Directed> g;
    auto n0 = g.add_node(0);
    auto n1 = g.add_node(1);
    auto n2 = g.add_node(2);
    auto n3 = g.add_node(3);

    g.add_edge(n0, n1);
    g.add_edge(n1, n2);
    g.add_edge(n2, n3);

    auto dom_tree = compute_dominator_tree(g, n0);

    const IdomMap expected{{n0.value, n0.value}, {n1.value, n0.value}, {n2.value, n1.value}, {n3.value, n2.value}};
    require_idoms_equal(extract_idoms_from_tree(dom_tree), expected);
}

TEST_CASE (



"[DominatorTree] Diamond graph"
,
"[DominatorTree]"
)
 {
    // Graph:
    //   0
    //  / \
    // 1   2
    //  \ /
    //   3
    Graph<int, int, Directed> g;
    auto n0 = g.add_node(0);
    auto n1 = g.add_node(1);
    auto n2 = g.add_node(2);
    auto n3 = g.add_node(3);

    g.add_edge(n0, n1);
    g.add_edge(n0, n2);
    g.add_edge(n1, n3);
    g.add_edge(n2, n3);

    auto dom_tree = compute_dominator_tree(g, n0);

    const IdomMap expected{{n0.value, n0.value}, {n1.value, n0.value}, {n2.value, n0.value}, {n3.value, n0.value}};
    require_idoms_equal(extract_idoms_from_tree(dom_tree), expected);
}

TEST_CASE (



"[DominatorTree] Branch and join"
,
"[DominatorTree]"
)
 {
    // Graph:
    //   0
    //  / \
    // 1   2
    //  \ /
    //   3
    //   |
    //   4
    Graph<int, int, Directed> g;
    auto n0 = g.add_node(0);
    auto n1 = g.add_node(1);
    auto n2 = g.add_node(2);
    auto n3 = g.add_node(3);
    auto n4 = g.add_node(4);

    g.add_edge(n0, n1);
    g.add_edge(n0, n2);
    g.add_edge(n1, n3);
    g.add_edge(n2, n3);
    g.add_edge(n3, n4);

    auto dom_tree = compute_dominator_tree(g, n0);

    const IdomMap expected{{n0.value, n0.value}, {n1.value, n0.value}, {n2.value, n0.value}, {n3.value, n0.value},
                           {n4.value, n3.value}};
    require_idoms_equal(extract_idoms_from_tree(dom_tree), expected);
}

TEST_CASE (



"[DominatorTree] Loop graph"
,
"[DominatorTree]"
)
 {
    // Graph: 0 -> 1 -> 2 -> 3 -> 1 (loop)
    Graph<int, int, Directed> g;
    auto n0 = g.add_node(0);
    auto n1 = g.add_node(1);
    auto n2 = g.add_node(2);
    auto n3 = g.add_node(3);

    g.add_edge(n0, n1);
    g.add_edge(n1, n2);
    g.add_edge(n2, n3);
    g.add_edge(n3, n1);

    auto dom_tree = compute_dominator_tree(g, n0);

    const IdomMap expected{{n0.value, n0.value}, {n1.value, n0.value}, {n2.value, n1.value}, {n3.value, n2.value}};
    require_idoms_equal(extract_idoms_from_tree(dom_tree), expected);
}

TEST_CASE (



"[DominatorTree] Disconnected graph (only reachable nodes)"
,
"[DominatorTree]"
)
 {
    // Graph: 0 -> 1, 2 (disconnected)
    Graph<int, int, Directed> g;
    auto n0 = g.add_node(0);
    auto n1 = g.add_node(1);
    auto n2 = g.add_node(2);

    g.add_edge(n0, n1);

    auto dom_tree = compute_dominator_tree(g, n0);

    const IdomMap expected{{n0.value, n0.value}, {n1.value, n0.value}};
    require_idoms_equal(extract_idoms_from_tree(dom_tree), expected);
}

TEST_CASE (



"[DominatorTree] Complex control flow (if-else-join)"
,
"[DominatorTree]"
)
 {
    // Graph:
    //   0
    //  / \
    // 1   2
    //  \ /
    //   3
    //   |
    //   4
    //   |
    //   5
    Graph<int, int, Directed> g;
    auto n0 = g.add_node(0);
    auto n1 = g.add_node(1);
    auto n2 = g.add_node(2);
    auto n3 = g.add_node(3);
    auto n4 = g.add_node(4);
    auto n5 = g.add_node(5);

    g.add_edge(n0, n1);
    g.add_edge(n0, n2);
    g.add_edge(n1, n3);
    g.add_edge(n2, n3);
    g.add_edge(n3, n4);
    g.add_edge(n4, n5);

    auto dom_tree = compute_dominator_tree(g, n0);

    const IdomMap expected{{n0.value, n0.value}, {n1.value, n0.value}, {n2.value, n0.value}, {n3.value, n0.value},
                           {n4.value, n3.value}, {n5.value, n4.value}};
    require_idoms_equal(extract_idoms_from_tree(dom_tree), expected);
}

TEST_CASE (



"[DominatorTree] Randomized cross-check against independent reference"
,
"[DominatorTree]"
)
 {
    std::mt19937 rng(0xC0FFEEU); // NOLINT(cert-msc51-cpp): fixed seed keeps this property test reproducible
    std::uniform_real_distribution<double> edge_prob(0.0, 1.0);
    std::uniform_int_distribution<int> node_count_dist(2, 7);

    for (int iter = 0; iter < 200; ++iter) {
        Graph<int, int, Directed> g;
        const int n = node_count_dist(rng);

        std::vector<NodeId> nodes;
        nodes.reserve(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) {
            nodes.push_back(g.add_node(i));
        }

        for (int from = 0; from < n; ++from) {
            for (int to = 0; to < n; ++to) {
                if (from == to) {
                    continue;
                }
                if (edge_prob(rng) < 0.3) {
                    g.add_edge(nodes[static_cast<size_t>(from)], nodes[static_cast<size_t>(to)]);
                }
            }
        }

        const auto start = nodes.front();
        const auto dom_tree = compute_dominator_tree(g, start);
        const auto actual = extract_idoms_from_tree(dom_tree);
        const auto expected = reference_idoms(g, start);

        INFO("iteration=" << iter);
        require_idoms_equal(actual, expected);
    }
}

// =============================================================================
// compute_dominators / dominator_graph_view / dominator_result tests
// Uses size_t as NodeId so these tests are independent of LiteGraph internals.
// =============================================================================

namespace {
    // Build a view from a simple adjacency list {from -> [to, ...]} and a node list.
    auto make_view(size_t entry,
                   std::vector<size_t> nodes,
                   std::vector<std::pair<size_t, size_t>> edges)
        -> dominator_graph_view<size_t> {
        dominator_graph_view<size_t> v;
        v.entry = entry;
        v.nodes = std::move(nodes);
        for (auto const& [from, to] : edges) {
            v.successors[from].push_back(to);
            v.predecessors[to].push_back(from);
        }
        return v;
    }
} // namespace

TEST_CASE (



"[compute_dominators] Linear chain"
,
"[DominatorTree][compute_dominators]"
)
 {
    // 0 -> 1 -> 2 -> 3
    auto r = compute_dominators(make_view(0, {0,1,2,3}, {{0,1},{1,2},{2,3}}));
    REQUIRE(r.ok());
    REQUIRE(r.immediate_dominator.at(0) == 0);
    REQUIRE(r.immediate_dominator.at(1) == 0);
    REQUIRE(r.immediate_dominator.at(2) == 1);
    REQUIRE(r.immediate_dominator.at(3) == 2);
}

TEST_CASE (



"[compute_dominators] Diamond graph"
,
"[DominatorTree][compute_dominators]"
)
 {
    //   0
    //  / \
    // 1   2
    //  \ /
    //   3
    auto r = compute_dominators(make_view(0, {0,1,2,3}, {{0,1},{0,2},{1,3},{2,3}}));
    REQUIRE(r.ok());
    REQUIRE(r.immediate_dominator.at(0) == 0);
    REQUIRE(r.immediate_dominator.at(1) == 0);
    REQUIRE(r.immediate_dominator.at(2) == 0);
    REQUIRE(r.immediate_dominator.at(3) == 0); // both arms merge; 0 is idom of 3
}

TEST_CASE (



"[compute_dominators] Loop back-edge"
,
"[DominatorTree][compute_dominators]"
)
 {
    // 0 -> 1 -> 2 -> 3 -> 1 (loop)
    auto r = compute_dominators(make_view(0, {0,1,2,3}, {{0,1},{1,2},{2,3},{3,1}}));
    REQUIRE(r.ok());
    REQUIRE(r.immediate_dominator.at(1) == 0);
    REQUIRE(r.immediate_dominator.at(2) == 1);
    REQUIRE(r.immediate_dominator.at(3) == 2);
}

TEST_CASE (



"[compute_dominators] Dominated children"
,
"[DominatorTree][compute_dominators]"
)
 {
    // 0 -> 1 -> 2
    //      |
    //      3
    auto r = compute_dominators(make_view(0, {0,1,2,3}, {{0,1},{1,2},{1,3}}));
    REQUIRE(r.ok());
    REQUIRE(r.dominated_children.at(0).count(1));
    REQUIRE(r.dominated_children.at(1).count(2));
    REQUIRE(r.dominated_children.at(1).count(3));
    REQUIRE(r.dominated_children.at(0).size() == 1);
    REQUIRE(r.dominated_children.at(1).size() == 2);
}

TEST_CASE (



"[compute_dominators] Dominance frontier – diamond"
,
"[DominatorTree][compute_dominators]"
)
 {
    //   0
    //  / \
    // 1   2
    //  \ /
    //   3
    auto r = compute_dominators(make_view(0, {0,1,2,3}, {{0,1},{0,2},{1,3},{2,3}}));
    REQUIRE(r.ok());
    // 3 is in the DF of 1 and 2 (they don't strictly dominate it)
    REQUIRE(r.dominance_frontier.at(1).count(3));
    REQUIRE(r.dominance_frontier.at(2).count(3));
    // 0 strictly dominates everything; its DF is empty
    REQUIRE(r.dominance_frontier.at(0).empty());
}

TEST_CASE (



"[compute_dominators] Single node"
,
"[DominatorTree][compute_dominators]"
)
 {
    auto r = compute_dominators(make_view(0, {0}, {}));
    REQUIRE(r.ok());
    REQUIRE(r.immediate_dominator.at(0) == 0);
    REQUIRE(r.dominated_children.at(0).empty());
}

TEST_CASE (



"[compute_dominators] Validation – empty graph"
,
"[DominatorTree][compute_dominators]"
)
 {
    dominator_graph_view<size_t> v;
    v.entry = 0;
    auto r = compute_dominators(v);
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.diagnostics.size() == 1);
    REQUIRE(r.diagnostics[0].find("no nodes") != std::string::npos);
}

TEST_CASE (



"[compute_dominators] Validation – entry not in nodes"
,
"[DominatorTree][compute_dominators]"
)
 {
    auto r = compute_dominators(make_view(99, {0,1,2}, {{0,1},{1,2}}));
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.diagnostics[0].find("entry") != std::string::npos);
}

TEST_CASE (



"[compute_dominators] Validation – invalid predecessor reference"
,
"[DominatorTree][compute_dominators]"
)
 {
    dominator_graph_view<size_t> v;
    v.entry = 0;
    v.nodes = {0, 1};
    v.predecessors[1] = {42}; // 42 is not in nodes
    auto r = compute_dominators(v);
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.diagnostics[0].find("predecessor") != std::string::npos);
}

TEST_CASE (



"[compute_dominators] Validation – invalid successor reference"
,
"[DominatorTree][compute_dominators]"
)
 {
    dominator_graph_view<size_t> v;
    v.entry = 0;
    v.nodes = {0, 1};
    v.successors[0] = {99}; // 99 is not in nodes
    auto r = compute_dominators(v);
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.diagnostics[0].find("successor") != std::string::npos);
}

TEST_CASE (



"[compute_dominators] Validation – multiple errors collected"
,
"[DominatorTree][compute_dominators]"
)
 {
    dominator_graph_view<size_t> v;
    v.entry = 0;
    v.nodes = {0, 1};
    v.predecessors[1] = {42, 43}; // two bad refs
    auto r = compute_dominators(v);
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.diagnostics.size() == 2);
}

TEST_CASE (



"[compute_dominators] Cross-check idoms against compute_dominator_tree"
,
"[DominatorTree][compute_dominators]"
)
 {
    // Build the same graph with both APIs and compare idom maps.
    // Graph: 0->1, 0->2, 1->3, 2->3, 3->4
    Graph<int, int, Directed> g;
    auto n0 = g.add_node(0); auto n1 = g.add_node(1); auto n2 = g.add_node(2);
    auto n3 = g.add_node(3); auto n4 = g.add_node(4);
    g.add_edge(n0,n1); g.add_edge(n0,n2);
    g.add_edge(n1,n3); g.add_edge(n2,n3); g.add_edge(n3,n4);

    // Reference: LiteGraph API
    auto tree_idoms = extract_idoms_from_tree(compute_dominator_tree(g, n0));

    // New API
    auto r = compute_dominators(make_view(0, {0,1,2,3,4},
        {{0,1},{0,2},{1,3},{2,3},{3,4}}));
    REQUIRE(r.ok());

    for (auto const& [node, idom_val] : tree_idoms) {
        INFO("node=" << node);
        REQUIRE(r.immediate_dominator.at(node).has_value());
        REQUIRE(r.immediate_dominator.at(node).value() == idom_val);
    }
}

// =============================================================================
// Dominance query helper tests (dominates, immediate_dominator_of,
// dominated_children_of, dominated_nodes)
// =============================================================================

namespace {
    // 0 -> 1 -> 3
    // 0 -> 2 -> 3
    //      3 -> 4
    // idom: 1->0, 2->0, 3->0, 4->3
    auto make_diamond_with_tail() {
        return compute_dominators(make_view(0, {0, 1, 2, 3, 4},
                                            {{0, 1}, {0, 2}, {1, 3}, {2, 3}, {3, 4}}));
    }
} // namespace

TEST_CASE (
"dominates: Self-dominance is always true"
,
"[DominatorTree][query][dominates]"
)
 {
    auto r = make_diamond_with_tail();
    for (size_t n : {size_t{0},size_t{1},size_t{2},size_t{3},size_t{4}}) {
        INFO("node=" << n);
        REQUIRE(dominates(r, n, n));
    }
}

TEST_CASE (
"dominates: Entry dominates all reachable nodes"
,
"[DominatorTree][query][dominates]"
)
 {
    auto r = make_diamond_with_tail();
    for (size_t n : {size_t{1},size_t{2},size_t{3},size_t{4}}) {
        INFO("node=" << n);
        REQUIRE(dominates(r, size_t{0}, n));
    }
}

TEST_CASE (
"dominates: Non-dominator returns false"
,
"[DominatorTree][query][dominates]"
)
 {
    auto r = make_diamond_with_tail();
    // 1 and 2 are siblings; neither dominates the other
    REQUIRE_FALSE(dominates(r, size_t{1}, size_t{2}));
    REQUIRE_FALSE(dominates(r, size_t{2}, size_t{1}));
    // 4 does not dominate 3 (its own idom)
    REQUIRE_FALSE(dominates(r, size_t{4}, size_t{3}));
}

TEST_CASE (
"dominates: Transitive dominance through chain"
,
"[DominatorTree][query][dominates]"
)
 {
    // 0 -> 1 -> 2 -> 3: 0 dominates 3 transitively
    auto r = compute_dominators(make_view(0, {0,1,2,3}, {{0,1},{1,2},{2,3}}));
    REQUIRE(dominates(r, size_t{0}, size_t{3}));
    REQUIRE(dominates(r, size_t{1}, size_t{3}));
    REQUIRE(dominates(r, size_t{2}, size_t{3}));
    REQUIRE_FALSE(dominates(r, size_t{3}, size_t{0}));
}

TEST_CASE (
"dominates: Missing node returns false"
,
"[DominatorTree][query][dominates]"
)
 {
    auto r = make_diamond_with_tail();
    REQUIRE_FALSE(dominates(r, size_t{99}, size_t{0}));
    REQUIRE_FALSE(dominates(r, size_t{0}, size_t{99}));
}

TEST_CASE (
"immediate_dominator_of: Correct idoms"
,
"[DominatorTree][query][idom]"
)
 {
    auto r = make_diamond_with_tail();
    // entry has no dominator above it
    REQUIRE_FALSE(immediate_dominator_of(r, size_t{0}).has_value());
    REQUIRE(immediate_dominator_of(r, size_t{1}) == size_t{0});
    REQUIRE(immediate_dominator_of(r, size_t{2}) == size_t{0});
    REQUIRE(immediate_dominator_of(r, size_t{3}) == size_t{0});
    REQUIRE(immediate_dominator_of(r, size_t{4}) == size_t{3});
}

TEST_CASE (
"immediate_dominator_of: Missing node returns nullopt"
,
"[DominatorTree][query][idom]"
)
 {
    auto r = make_diamond_with_tail();
    REQUIRE_FALSE(immediate_dominator_of(r, size_t{99}).has_value());
}

TEST_CASE (
"dominated_children_of: Direct children only"
,
"[DominatorTree][query][children]"
)
 {
    auto r = make_diamond_with_tail();
    // 0 directly dominates 1, 2, 3 (not 4)
    auto const& c0 = dominated_children_of(r, size_t{0});
    REQUIRE(c0.size() == 3);
    REQUIRE(c0.count(size_t{1})); REQUIRE(c0.count(size_t{2})); REQUIRE(c0.count(size_t{3}));
    REQUIRE_FALSE(c0.count(size_t{4}));

    // 3 directly dominates 4 only
    auto const& c3 = dominated_children_of(r, size_t{3});
    REQUIRE(c3.size() == 1);
    REQUIRE(c3.count(size_t{4}));

    // leaf nodes have no children
    REQUIRE(dominated_children_of(r, size_t{4}).empty());
}

TEST_CASE (
"dominated_children_of: Missing node returns empty set"
,
"[DominatorTree][query][children]"
)
 {
    auto r = make_diamond_with_tail();
    REQUIRE(dominated_children_of(r, size_t{99}).empty());
}

TEST_CASE (
"dominated_nodes: Full transitive subtree"
,
"[DominatorTree][query][subtree]"
)
 {
    auto r = make_diamond_with_tail();
    // Entry dominates everything below it (1,2,3,4)
    auto dn0 = dominated_nodes(r, size_t{0});
    std::unordered_set<size_t> dn0_set(dn0.begin(), dn0.end());
    REQUIRE(dn0_set.size() == 4);
    for (size_t n : {size_t{1},size_t{2},size_t{3},size_t{4}}) REQUIRE(dn0_set.count(n));

    // 3 transitively dominates only 4
    auto dn3 = dominated_nodes(r, size_t{3});
    REQUIRE(dn3.size() == 1);
    REQUIRE(dn3[0] == size_t{4});

    // leaves have no dominated nodes
    REQUIRE(dominated_nodes(r, size_t{4}).empty());
    REQUIRE(dominated_nodes(r, size_t{1}).empty());
}

TEST_CASE (
"dominated_nodes: Missing node returns empty"
,
"[DominatorTree][query][subtree]"
)
 {
    auto r = make_diamond_with_tail();
    REQUIRE(dominated_nodes(r, size_t{99}).empty());
}

TEST_CASE (
"dominated_nodes: Deep chain is fully transitive"
,
"[DominatorTree][query][subtree]"
)
 {
    // 0->1->2->3->4->5: 0 should dominate all of 1-5
    auto r = compute_dominators(make_view(0, {0,1,2,3,4,5},
        {{0,1},{1,2},{2,3},{3,4},{4,5}}));
    auto dn = dominated_nodes(r, size_t{0});
    std::unordered_set<size_t> s(dn.begin(), dn.end());
    REQUIRE(s.size() == 5);
    for (size_t n : {size_t{1},size_t{2},size_t{3},size_t{4},size_t{5}}) REQUIRE(s.count(n));
}

// =============================================================================
// compute_dominance_frontier – standalone tests
// =============================================================================

TEST_CASE (



"[compute_dominance_frontier] Diamond via standalone call"
,
"[DominatorTree][df]"
)
 {
    //   0
    //  / \
    // 1   2
    //  \ /
    //   3
    auto view = make_view(0, {0,1,2,3}, {{0,1},{0,2},{1,3},{2,3}});
    auto r = compute_dominators(view);
    REQUIRE(r.ok());

    // Wipe and recompute via standalone function to verify it produces the same result.
    for (auto& [k, v] : r.dominance_frontier) v.clear();
    compute_dominance_frontier(view, r);

    REQUIRE(r.dominance_frontier.at(1).count(size_t{3}));
    REQUIRE(r.dominance_frontier.at(2).count(size_t{3}));
    REQUIRE(r.dominance_frontier.at(0).empty());
    REQUIRE(r.dominance_frontier.at(3).empty());
}

TEST_CASE (



"[compute_dominance_frontier] Linear chain has no frontier"
,
"[DominatorTree][df]"
)
 {
    auto view = make_view(0, {0,1,2,3}, {{0,1},{1,2},{2,3}});
    auto r = compute_dominators(view);
    REQUIRE(r.ok());

    for (auto& [k, v] : r.dominance_frontier) v.clear();
    compute_dominance_frontier(view, r);

    for (size_t n : {size_t{0},size_t{1},size_t{2},size_t{3}})
        REQUIRE(r.dominance_frontier.at(n).empty());
}

TEST_CASE (



"[compute_dominance_frontier] Loop join point"
,
"[DominatorTree][df]"
)
 {
    // 0 -> 1 -> 2 -> 3 -> 1 (back edge 3->1)
    // Join point is 1 (predecessors: 0 and 3).
    // DF of 3 should contain 1; DF of 0 should be empty.
    auto view = make_view(0, {0,1,2,3}, {{0,1},{1,2},{2,3},{3,1}});
    auto r = compute_dominators(view);
    REQUIRE(r.ok());

    for (auto& [k, v] : r.dominance_frontier) v.clear();
    compute_dominance_frontier(view, r);

    REQUIRE(r.dominance_frontier.at(size_t{3}).count(size_t{1}));
    REQUIRE(r.dominance_frontier.at(size_t{0}).empty());
}

// =============================================================================
// find_back_edges / find_loop_headers tests
// =============================================================================

TEST_CASE (



"[find_back_edges] No back edges in a DAG"
,
"[DominatorTree][loop]"
)
 {
    auto view = make_view(0, {0,1,2,3}, {{0,1},{0,2},{1,3},{2,3}});
    auto r = compute_dominators(view);
    REQUIRE(r.ok());
    REQUIRE(find_back_edges(view, r).empty());
}

TEST_CASE (



"[find_back_edges] Single back edge in simple loop"
,
"[DominatorTree][loop]"
)
 {
    // 0->1->2->3->1
    auto view = make_view(0, {0,1,2,3}, {{0,1},{1,2},{2,3},{3,1}});
    auto r = compute_dominators(view);
    REQUIRE(r.ok());

    auto be = find_back_edges(view, r);
    REQUIRE(be.size() == 1);
    REQUIRE(be[0].from == size_t{3});
    REQUIRE(be[0].to   == size_t{1});
}

TEST_CASE (



"[find_back_edges] Self-loop is a back edge"
,
"[DominatorTree][loop]"
)
 {
    // 0->1->1 (self-loop on 1)
    auto view = make_view(0, {0,1}, {{0,1},{1,1}});
    auto r = compute_dominators(view);
    REQUIRE(r.ok());

    auto be = find_back_edges(view, r);
    REQUIRE(be.size() == 1);
    REQUIRE(be[0].from == size_t{1});
    REQUIRE(be[0].to   == size_t{1});
}

TEST_CASE (



"[find_back_edges] Two nested loops"
,
"[DominatorTree][loop]"
)
 {
    // 0->1->2->3->2 (inner), 3->1 (outer)
    auto view = make_view(0, {0,1,2,3}, {{0,1},{1,2},{2,3},{3,2},{3,1}});
    auto r = compute_dominators(view);
    REQUIRE(r.ok());

    auto be = find_back_edges(view, r);
    // Expect back edges 3->2 and 3->1
    std::unordered_set<size_t> targets;
    for (auto const& e : be) {
        REQUIRE(e.from == size_t{3});
        targets.insert(e.to);
    }
    REQUIRE(targets.count(size_t{2}));
    REQUIRE(targets.count(size_t{1}));
}

TEST_CASE (



"[find_loop_headers] No loops means no headers"
,
"[DominatorTree][loop]"
)
 {
    auto view = make_view(0, {0,1,2,3}, {{0,1},{1,2},{2,3}});
    auto r = compute_dominators(view);
    REQUIRE(r.ok());
    REQUIRE(find_loop_headers(view, r).empty());
}

TEST_CASE (



"[find_loop_headers] Simple loop header is 1"
,
"[DominatorTree][loop]"
)
 {
    auto view = make_view(0, {0,1,2,3}, {{0,1},{1,2},{2,3},{3,1}});
    auto r = compute_dominators(view);
    REQUIRE(r.ok());

    auto headers = find_loop_headers(view, r);
    REQUIRE(headers.size() == 1);
    REQUIRE(headers.count(size_t{1}));
}

TEST_CASE (



"[find_loop_headers] Two distinct loop headers"
,
"[DominatorTree][loop]"
)
 {
    // 0->1->2->1 (loop A, header=1), 0->3->4->3 (loop B, header=3)
    // Note: 0 reaches both loops independently.
    auto view = make_view(0, {0,1,2,3,4}, {{0,1},{1,2},{2,1},{0,3},{3,4},{4,3}});
    auto r = compute_dominators(view);
    REQUIRE(r.ok());

    auto headers = find_loop_headers(view, r);
    REQUIRE(headers.count(size_t{1}));
    REQUIRE(headers.count(size_t{3}));
}

// =============================================================================
// Named-node tests for generic dominator_graph_view utilities.
// Node IDs are string labels so the assertions read as close to the spec as
// possible.  No LiteGraph or Lithe MIR involvement.
// =============================================================================

namespace {
    auto make_string_view(std::string entry,
                          std::vector<std::string> nodes,
                          std::vector<std::pair<std::string, std::string>> edges)
        -> dominator_graph_view<std::string> {
        dominator_graph_view<std::string> v;
        v.entry = std::move(entry);
        v.nodes = std::move(nodes);
        for (auto const& [from, to] : edges) {
            v.successors[from].push_back(to);
            v.predecessors[to].push_back(from);
        }
        return v;
    }
} // namespace

TEST_CASE (



"[generic] Linear graph dominance queries"
,
"[DominatorTree][generic]"
)
 {
    // entry -> A -> B
    auto view = make_string_view("entry", {"entry","A","B"},
                                 {{"entry","A"},{"A","B"}});
    auto r = compute_dominators(view);
    REQUIRE(r.ok());

    REQUIRE(dominates(r, std::string{"entry"}, std::string{"A"}));
    REQUIRE(dominates(r, std::string{"entry"}, std::string{"B"}));
    REQUIRE(dominates(r, std::string{"A"},     std::string{"B"}));

    REQUIRE(immediate_dominator_of(r, std::string{"A"}) == std::string{"entry"});
    REQUIRE(immediate_dominator_of(r, std::string{"B"}) == std::string{"A"});
}

TEST_CASE (
"generic: Branch-merge: entry dominates merge, branches do not"
,
"[DominatorTree][generic]"
)
 {
    // entry -> then
    // entry -> else
    // then  -> merge
    // else  -> merge
    auto view = make_string_view("entry", {"entry","then","else","merge"},
                                 {{"entry","then"},{"entry","else"},
                                  {"then","merge"},{"else","merge"}});
    auto r = compute_dominators(view);
    REQUIRE(r.ok());

    REQUIRE(    dominates(r, std::string{"entry"}, std::string{"merge"}));
    REQUIRE_FALSE(dominates(r, std::string{"then"},  std::string{"merge"}));
    REQUIRE_FALSE(dominates(r, std::string{"else"},  std::string{"merge"}));
}

TEST_CASE (
"generic: Loop: back edge and loop header identified"
,
"[DominatorTree][generic]"
)
 {
    // entry -> header -> body -> header  (back edge: body -> header)
    auto view = make_string_view("entry", {"entry","header","body"},
                                 {{"entry","header"},{"header","body"},{"body","header"}});
    auto r = compute_dominators(view);
    REQUIRE(r.ok());

    auto be = find_back_edges(view, r);
    bool found_back_edge = false;
    for (auto const& e : be) {
        if (e.from == std::string{"body"} && e.to == std::string{"header"})
            found_back_edge = true;
    }
    REQUIRE(found_back_edge);

    auto headers = find_loop_headers(view, r);
    REQUIRE(headers.count(std::string{"header"}));
}

// ---------------------------------------------------------------------------
// Prompt 2 — Hardened validation: map-key checks + unreachable nodes
// ---------------------------------------------------------------------------

TEST_CASE (



"[compute_dominators] predecessors key not in nodes is structural error"
,
"[DominatorTree][compute_dominators][validation]"
)
 {
    // predecessors map has key 99 which is not in nodes list
    dominator_graph_view<size_t> view;
    view.entry = 0;
    view.nodes = {0, 1};
    view.predecessors[99] = {0};   // 99 is not a node
    view.successors[0]    = {1};
    view.predecessors[1]  = {0};
    auto r = compute_dominators(view);
    REQUIRE(!r.ok());
    bool found = false;
    for (const auto &d : r.diagnostics)
        if (d.find("predecessors key") != std::string::npos) { found = true; break; }
    REQUIRE(found);
}

TEST_CASE (



"[compute_dominators] successors key not in nodes is structural error"
,
"[DominatorTree][compute_dominators][validation]"
)
 {
    dominator_graph_view<size_t> view;
    view.entry = 0;
    view.nodes = {0, 1};
    view.successors[99]   = {1};   // 99 is not a node
    view.predecessors[1]  = {0};
    auto r = compute_dominators(view);
    REQUIRE(!r.ok());
    bool found = false;
    for (const auto &d : r.diagnostics)
        if (d.find("successors key") != std::string::npos) { found = true; break; }
    REQUIRE(found);
}

TEST_CASE (



"[compute_dominators] unreachable node does not fail ok()"
,
"[DominatorTree][compute_dominators][validation]"
)
 {
    // node 2 has no path from entry 0
    auto view = make_view(0, {0, 1, 2}, {{0, 1}});
    auto r = compute_dominators(view);
    REQUIRE(r.ok());               // structural validity: ok()
    REQUIRE(!r.warnings.empty()); // but warning recorded
    bool found = false;
    for (const auto &w : r.warnings)
        if (w.find("unreachable") != std::string::npos) { found = true; break; }
    REQUIRE(found);
    // Unreachable node has null idom
    REQUIRE(r.immediate_dominator.count(2));
    REQUIRE(!r.immediate_dominator.at(2).has_value());
}

TEST_CASE (



"[compute_dominators] all nodes reachable produces no warnings"
,
"[DominatorTree][compute_dominators][validation]"
)
 {
    auto view = make_view(0, {0, 1, 2}, {{0, 1}, {1, 2}});
    auto r = compute_dominators(view);
    REQUIRE(r.ok());
    REQUIRE(r.warnings.empty());
}
