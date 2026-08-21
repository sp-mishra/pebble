// =============================================================================
// test_containers_conversion_graph.cpp — tests for containers::conversion_graph
// =============================================================================

#include "catch_amalgamated.hpp"

#include "containers/conversion_graph.hpp"

using namespace containers;

// ---------------------------------------------------------------------------
// 1. empty graph
// ---------------------------------------------------------------------------
TEST_CASE (



"conversion_graph: empty graph"
,
"[containers][conversion_graph]"
)
 {
    conversion_graph g;
    CHECK(g.empty());
    CHECK(g.edge_count() == 0);
    CHECK(g.vertex_count() == 0);
    CHECK(g.least_cost_path(1, 2) == std::nullopt);

    auto self = g.least_cost_path(1, 1);
    REQUIRE(self.has_value());
    CHECK(self->empty());
    CHECK(g.path_cost(*self) == 0);
}

// ---------------------------------------------------------------------------
// 2. single edge
// ---------------------------------------------------------------------------
TEST_CASE (



"conversion_graph: single edge"
,
"[containers][conversion_graph]"
)
 {
    conversion_graph g;
    auto eid = g.add_conversion(10, 20, 5);

    auto fwd = g.least_cost_path(10, 20);
    REQUIRE(fwd.has_value());
    REQUIRE(fwd->size() == 1);
    CHECK((*fwd)[0] == eid);
    CHECK(g.path_cost(*fwd) == 5);

    // reverse: no edge 20→10
    CHECK(g.least_cost_path(20, 10) == std::nullopt);
}

// ---------------------------------------------------------------------------
// 3. parallel edges — pick cheapest
// ---------------------------------------------------------------------------
TEST_CASE (



"conversion_graph: parallel edges pick cheapest"
,
"[containers][conversion_graph]"
)
 {
    conversion_graph g;
    g.add_conversion(1, 2, 5); // expensive
    auto cheap = g.add_conversion(1, 2, 2);

    auto path = g.least_cost_path(1, 2);
    REQUIRE(path.has_value());
    REQUIRE(path->size() == 1);
    CHECK((*path)[0] == cheap);
    CHECK(g.path_cost(*path) == 2);
}

// ---------------------------------------------------------------------------
// 4. multi-hop cheaper than direct
// ---------------------------------------------------------------------------
TEST_CASE (



"conversion_graph: multi-hop cheaper than direct"
,
"[containers][conversion_graph]"
)
 {
    conversion_graph g;
    g.add_conversion(1, 2, 10);          // direct A→B cost 10
    auto e_ac = g.add_conversion(1, 3, 1); // A→C cost 1
    auto e_cb = g.add_conversion(3, 2, 1); // C→B cost 1

    auto path = g.least_cost_path(1, 2);
    REQUIRE(path.has_value());
    REQUIRE(path->size() == 2);
    CHECK((*path)[0] == e_ac);
    CHECK((*path)[1] == e_cb);
    CHECK(g.path_cost(*path) == 2);
}

// ---------------------------------------------------------------------------
// 5. unreachable (disjoint components)
// ---------------------------------------------------------------------------
TEST_CASE (



"conversion_graph: unreachable returns nullopt"
,
"[containers][conversion_graph]"
)
 {
    conversion_graph g;
    g.add_conversion(1, 2, 3);
    g.add_conversion(10, 11, 3);

    CHECK(g.least_cost_path(1, 10) == std::nullopt);
    CHECK(g.least_cost_path(2, 10) == std::nullopt);
}

// ---------------------------------------------------------------------------
// 6. zero-cost edge
// ---------------------------------------------------------------------------
TEST_CASE (



"conversion_graph: zero-cost edge"
,
"[containers][conversion_graph]"
)
 {
    conversion_graph g;
    auto eid = g.add_conversion(5, 6, 0);

    auto path = g.least_cost_path(5, 6);
    REQUIRE(path.has_value());
    REQUIRE(path->size() == 1);
    CHECK((*path)[0] == eid);
    CHECK(g.path_cost(*path) == 0);
}

// ---------------------------------------------------------------------------
// 7. determinism — equal-cost tie-break
// ---------------------------------------------------------------------------
TEST_CASE (



"conversion_graph: deterministic tie-break"
,
"[containers][conversion_graph]"
)
 {
    // Two equal-cost paths: 1→2→4 and 1→3→4, each cost 2.
    conversion_graph g;
    g.add_conversion(1, 2, 1);
    g.add_conversion(2, 4, 1);
    g.add_conversion(1, 3, 1);
    g.add_conversion(3, 4, 1);

    auto first  = g.least_cost_path(1, 4);
    auto second = g.least_cost_path(1, 4);

    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(*first == *second);
    CHECK(g.path_cost(*first) == 2);
}

// ---------------------------------------------------------------------------
// 8. self-loop ignored — from==to returns empty path regardless
// ---------------------------------------------------------------------------
TEST_CASE (



"conversion_graph: self-loop does not affect from==to query"
,
"[containers][conversion_graph]"
)
 {
    conversion_graph g;
    g.add_conversion(7, 7, 3); // self-loop

    auto path = g.least_cost_path(7, 7);
    REQUIRE(path.has_value());
    CHECK(path->empty());
    CHECK(g.path_cost(*path) == 0);
}
