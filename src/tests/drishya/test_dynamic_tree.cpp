// ============================================================================
// test_dynamic_tree.cpp — retained WidgetTree structure, handles, dirty set
// ============================================================================

#include <catch_amalgamated.hpp>

#include "drishya/drishya.hpp"

#include <vector>

using namespace pebble::drishya;
namespace w = pebble::drishya::widgets;

using M = MonospaceMetrics;
using P = DefaultPainter;
using Tree = WidgetTree<M, P>;

TEST_CASE (
"dynamic_tree: build parent/child structure"
,
"[drishya][tree]"
)
 {
    Tree t;
    const NodeId root = t.set_root(w::vstack(8));
    const NodeId a = t.add_child(root, w::label("A"));
    const NodeId b = t.add_child(root, w::label("B"));

    CHECK(t.root() == root);
    CHECK(t.node_count() == 3);
    CHECK(t.parent(a) == root);
    CHECK(t.parent(b) == root);
    CHECK(t.first_child(root) == a);
    CHECK(t.next_sibling(a) == b);
    CHECK(t.next_sibling(b) == kInvalidNode);
}

TEST_CASE (
"dynamic_tree: pre-order walk visits every node"
,
"[drishya][tree]"
)
 {
    Tree t;
    const NodeId root = t.set_root(w::vstack());
    const NodeId a = t.add_child(root, w::hstack());
    t.add_child(a, w::label("leaf"));
    t.add_child(root, w::label("B"));

    std::vector<NodeId> order;
    t.walk(root, [&](NodeId id, std::size_t) { order.push_back(id); });
    CHECK(order.size() == 4);
    CHECK(order.front() == root); // root first in pre-order
}

TEST_CASE (
"dynamic_tree: stable handles resolve across inserts"
,
"[drishya][tree]"
)
 {
    Tree t;
    const NodeId root = t.set_root(w::vstack());
    const NodeId a = t.add_child(root, w::label("A"));
    const WidgetHandle h = t.make_handle(a);

    CHECK(t.alive(h));
    CHECK(t.resolve(h) == a);

    // Inserting more nodes does not invalidate the handle.
    t.add_child(root, w::label("B"));
    CHECK(t.resolve(h) == a);
}

TEST_CASE (
"dynamic_tree: remove unlinks a subtree"
,
"[drishya][tree]"
)
 {
    Tree t;
    const NodeId root = t.set_root(w::vstack());
    const NodeId a = t.add_child(root, w::hstack());
    t.add_child(a, w::label("child"));
    const NodeId b = t.add_child(root, w::label("B"));

    const std::size_t before = t.node_count();
    t.remove(a); // removes a and its child
    CHECK(t.node_count() == before - 2);
    CHECK(t.first_child(root) == b); // b is now the only child
}

TEST_CASE (
"dynamic_tree: dirty set merges and drains"
,
"[drishya][tree]"
)
 {
    Tree t;
    const NodeId root = t.set_root(w::vstack());
    const NodeId a = t.add_child(root, w::label("A"));

    t.mark_dirty(a, kDirtyLayout);
    t.mark_dirty(a, kDirtyPaint); // should merge, not replace
    CHECK(t.any_dirty());

    std::uint8_t seen = 0;
    NodeId seen_id = kInvalidNode;
    t.drain_dirty([&](NodeId id, std::uint8_t bits) { seen_id = id; seen = bits; });
    CHECK(seen_id == a);
    CHECK((seen & kDirtyLayout) != 0);
    CHECK((seen & kDirtyPaint) != 0);
    CHECK_FALSE(t.any_dirty()); // drained
}

TEST_CASE (
"dynamic_tree: clear resets everything"
,
"[drishya][tree]"
)
 {
    Tree t;
    const NodeId root = t.set_root(w::vstack());
    t.add_child(root, w::label("A"));
    t.clear();
    CHECK(t.root() == kInvalidNode);
    CHECK(t.node_count() == 0);
}
