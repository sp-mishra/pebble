#include <catch_amalgamated.hpp>
#include <containers/graph/LiteGraph.hpp>
#include <containers/graph/LiteGraphAlgorithms.hpp>
#include <containers/graph/LiteGraphPravaha.hpp>
#include <vector>
#include <numeric>
#include <cmath>

using namespace litegraph;

TEST_CASE("LiteGraph Pravaha Add-on: Parallel Level-Synchronous BFS", "[litegraph][pravaha][bfs]") {
    Graph<int, int, Directed> g;

    // Build a diamond DAG: 0 -> 1, 0 -> 2, 1 -> 3, 2 -> 3
    auto n0 = g.add_node(100);
    auto n1 = g.add_node(101);
    auto n2 = g.add_node(102);
    auto n3 = g.add_node(103);

    g.add_edge(n0, n1, 1);
    g.add_edge(n0, n2, 1);
    g.add_edge(n1, n3, 1);
    g.add_edge(n2, n3, 1);

    std::atomic<int> visit_count{0};
    std::vector<int> visited_nodes;
    std::mutex mtx;

    litegraph::pravaha::parallel_bfs(g, n0, [&](NodeId u, int val) {
        visit_count.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard lock(mtx);
        visited_nodes.push_back(val);
    });

    REQUIRE(visit_count.load() == 4);
    REQUIRE(visited_nodes.size() == 4);
}

TEST_CASE("LiteGraph Pravaha Add-on: Parallel CSR PageRank", "[litegraph][pravaha][pagerank]") {
    Graph<int, double, Directed> g;

    auto n0 = g.add_node(0);
    auto n1 = g.add_node(1);
    auto n2 = g.add_node(2);
    auto n3 = g.add_node(3);

    // 0 -> 1, 0 -> 2, 1 -> 2, 2 -> 0, 3 -> 2 (star to 2)
    g.add_edge(n0, n1, 1.0);
    g.add_edge(n0, n2, 1.0);
    g.add_edge(n1, n2, 1.0);
    g.add_edge(n2, n0, 1.0);
    g.add_edge(n3, n2, 1.0);

    auto csr = freeze_to_csr(g);

    CsrPageRankOptions opts{
        .damping_factor = 0.85,
        .max_iterations = 100,
        .tolerance = 1e-7
    };

    auto serial_res = litegraph::pagerank(csr, opts);
    auto parallel_res = litegraph::pravaha::parallel_pagerank(csr, opts);

    REQUIRE(parallel_res.converged);
    REQUIRE(parallel_res.ranks.size() == csr.node_count());

    // PageRank sums to 1.0
    double sum = 0.0;
    for (double r : parallel_res.ranks) sum += r;
    REQUIRE(sum == Catch::Approx(1.0).epsilon(1e-4));

    // Compare with serial reference
    for (std::size_t i = 0; i < csr.node_count(); ++i) {
        REQUIRE(parallel_res.ranks[i] == Catch::Approx(serial_res.ranks[i]).epsilon(1e-3));
    }
}

TEST_CASE("LiteGraph Pravaha Add-on: Parallel Multi-Source Dijkstra", "[litegraph][pravaha][dijkstra]") {
    Graph<int, double, Directed> g;

    auto n0 = g.add_node(0);
    auto n1 = g.add_node(1);
    auto n2 = g.add_node(2);
    auto n3 = g.add_node(3);

    g.add_edge(n0, n1, 1.0);
    g.add_edge(n1, n2, 2.0);
    g.add_edge(n2, n3, 1.5);
    g.add_edge(n0, n3, 10.0);

    std::vector<NodeId> sources = {n0, n1, n2};
    auto multi_res = litegraph::pravaha::parallel_multi_source_dijkstra(g, sources);

    REQUIRE(multi_res.size() == 3);

    // Check distances from source n0
    auto& [dist_n0, pred_n0] = multi_res[0];
    REQUIRE(dist_n0[n0.value] == Catch::Approx(0.0));
    REQUIRE(dist_n0[n1.value] == Catch::Approx(1.0));
    REQUIRE(dist_n0[n2.value] == Catch::Approx(3.0));
    REQUIRE(dist_n0[n3.value] == Catch::Approx(4.5)); // 1.0 + 2.0 + 1.5 < 10.0

    // Check distances from source n1
    auto& [dist_n1, pred_n1] = multi_res[1];
    REQUIRE(dist_n1[n1.value] == Catch::Approx(0.0));
    REQUIRE(dist_n1[n2.value] == Catch::Approx(2.0));
    REQUIRE(dist_n1[n3.value] == Catch::Approx(3.5));
}

TEST_CASE("LiteGraph Pravaha Add-on: Parallel Betweenness Centrality", "[litegraph][pravaha][betweenness]") {
    Graph<int, int, Undirected> g;

    // Linear chain: 0 - 1 - 2 - 3 - 4
    // Center node (2) has highest betweenness
    auto n0 = g.add_node(0);
    auto n1 = g.add_node(1);
    auto n2 = g.add_node(2);
    auto n3 = g.add_node(3);
    auto n4 = g.add_node(4);

    g.add_edge(n0, n1, 1);
    g.add_edge(n1, n2, 1);
    g.add_edge(n2, n3, 1);
    g.add_edge(n3, n4, 1);

    auto serial_cent = litegraph::betweenness_centrality(g);
    auto parallel_cent = litegraph::pravaha::parallel_betweenness_centrality(g);

    REQUIRE(parallel_cent.size() == g.node_capacity());
    for (std::size_t i = 0; i < g.node_count(); ++i) {
        REQUIRE(parallel_cent[i] == Catch::Approx(serial_cent[i]).epsilon(1e-4));
    }

    // Node 2 has the highest centrality in a 5-node line graph
    REQUIRE(parallel_cent[n2.value] > parallel_cent[n1.value]);
    REQUIRE(parallel_cent[n1.value] > parallel_cent[n0.value]);
}
