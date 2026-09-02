// ============================================================================
// test_ops.cpp — tree query operations (visit, find, subtree, depth)
// ============================================================================

#include <catch_amalgamated.hpp>

#include "drishya/drishya.hpp"
#include "drishya/ops.hpp"

#include <array>
#include <span>

using namespace pebble::drishya;
namespace w = pebble::drishya::widgets;

using M = MonospaceMetrics;
using P = DefaultPainter;
using Tree = WidgetTree<M, P>;

namespace {
    struct Built {
        Tree t;
        NodeId root, row, leaf, b;
    };

    Built make_tree() {
        Built x;
        x.root = x.t.set_root(w::vstack());
        x.row = x.t.add_child(x.root, w::hstack());
        x.leaf = x.t.add_child(x.row, w::label("leaf"));
        x.b = x.t.add_child(x.root, w::label("B"));
        return x;
    }
} // namespace

TEST_CASE (
"ops: visit covers all nodes pre-order"
,
"[drishya][ops]"
)
 {
    Built x = make_tree();
    std::size_t count = 0;
    ops::visit(x.t, [&](NodeId, std::size_t) { ++count; });
    CHECK(count == 4);
}

TEST_CASE (
"ops: subtree_size counts inclusive"
,
"[drishya][ops]"
)
 {
    Built x = make_tree();
    CHECK(ops::subtree_size(x.t, x.root) == 4);
    CHECK(ops::subtree_size(x.t, x.row) == 2);  // row + leaf
    CHECK(ops::subtree_size(x.t, x.leaf) == 1);
}

TEST_CASE (
"ops: depth_of measures from root"
,
"[drishya][ops]"
)
 {
    Built x = make_tree();
    CHECK(ops::depth_of(x.t, x.root) == 0);
    CHECK(ops::depth_of(x.t, x.row) == 1);
    CHECK(ops::depth_of(x.t, x.leaf) == 2);
}

TEST_CASE (
"ops: find returns first matching node"
,
"[drishya][ops]"
)
 {
    Built x = make_tree();
    const NodeId found = ops::find(x.t, [&](NodeId id) { return id == x.b; });
    CHECK(found == x.b);
    const NodeId none = ops::find(x.t, [](NodeId) { return false; });
    CHECK(none == kInvalidNode);
}

TEST_CASE (
"ops: find_all fills the output span heap-free"
,
"[drishya][ops]"
)
 {
    Built x = make_tree();
    std::array<NodeId, 8> buf{};
    const std::size_t n = ops::find_all(x.t, [](NodeId) { return true; },
                                        std::span<NodeId>{buf});
    CHECK(n == 4);
}
