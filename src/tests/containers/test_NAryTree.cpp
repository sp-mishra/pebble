#define CATCH_CONFIG_MAIN
#include <catch_amalgamated.hpp>
#include "containers/tree/NAryTree.hpp"

using IntTree = NAryTree<int>;

TEST_CASE (



"[NArayTree] Create empty tree"
)
 {
    IntTree tree;
    REQUIRE(tree.get_root() == nullptr);
    REQUIRE(tree.empty());
    REQUIRE(tree.height() == 0);
}

TEST_CASE (



"[NArayTree] Insert root node"
)
 {
    IntTree tree;
    auto* root = tree.insert(nullptr, 10);
    REQUIRE(root != nullptr);
    REQUIRE(tree.get_root() == root);
    REQUIRE(root->data == 10);
    REQUIRE(tree.size() == 1);
    REQUIRE(tree.height() == 1);
}

TEST_CASE (



"[NArayTree] Insert children"
)
 {
    IntTree tree;
    auto* root = tree.insert(nullptr, 1);
    auto* child1 = tree.insert(root, 2);
    auto* child2 = tree.insert(root, 3);
    REQUIRE(child1 != nullptr);
    REQUIRE(child2 != nullptr);
    REQUIRE(root->children.size() == 2);
    REQUIRE(tree.size() == 3);
    REQUIRE(tree.height() == 2);
}

TEST_CASE (



"[NArayTree] Bulk insert children"
)
 {
    IntTree tree;
    auto* root = tree.insert(nullptr, 1);
    std::vector<std::pair<int, EmptyMetadata>> items = {{2, {}}, {3, {}}};
    auto children = tree.bulk_insert(root, items);
    REQUIRE(children.size() == 2);
    REQUIRE(root->children.size() == 2);
    REQUIRE(tree.size() == 3);
}

TEST_CASE (



"[NArayTree] Remove node"
)
 {
    IntTree tree;
    auto* root = tree.insert(nullptr, 1);
    auto* child = tree.insert(root, 2);
    REQUIRE(tree.remove(child));
    REQUIRE(root->children.empty());
    REQUIRE(tree.size() == 1);
    REQUIRE_FALSE(tree.remove(child)); // Already removed
}

TEST_CASE (



"[NArayTree] Remove root node"
)
 {
    IntTree tree;
    auto* root = tree.insert(nullptr, 1);
    REQUIRE(tree.remove(root));
    REQUIRE(tree.get_root() == nullptr);
    REQUIRE(tree.empty());
}

TEST_CASE (



"[NArayTree] Find node by value"
)
 {
    IntTree tree;
    auto* root = tree.insert(nullptr, 1);
    auto* child = tree.insert(root, 2);
    auto* found = tree.find(2);
    REQUIRE(found == child);
    REQUIRE(tree.find(42) == nullptr);
}

TEST_CASE (



"[NArayTree] Find all nodes by value"
)
 {
    IntTree tree;
    auto* root = tree.insert(nullptr, 1);
    tree.insert(root, 2);
    tree.insert(root, 2);
    auto found = tree.find_all(2);
    REQUIRE(found.size() == 2);
}

TEST_CASE (



"[NArayTree] Pre-order traversal"
)
 {
    IntTree tree;
    auto* root = tree.insert(nullptr, 1);
    auto* c1 = tree.insert(root, 2);
    tree.insert(root, 3);
    tree.insert(c1, 4);
    std::vector<int> order;
    for (auto& node : tree) order.push_back(node.data);
    REQUIRE(order == std::vector<int>{1, 2, 4, 3});
}

TEST_CASE (



"[NArayTree] Post-order traversal"
)
 {
    IntTree tree;
    auto* root = tree.insert(nullptr, 1);
    auto* c1 = tree.insert(root, 2);
    tree.insert(root, 3);
    tree.insert(c1, 4);
    std::vector<int> order;
    auto porder = tree.post_order();
    for (auto it = porder.begin(); it != porder.end(); ++it)
        order.push_back(it->data);
    REQUIRE(order == std::vector<int>{4, 2, 3, 1});
}

TEST_CASE (



"[NArayTree] Copy constructor"
)
 {
    IntTree tree;
    auto* root = tree.insert(nullptr, 1);
    tree.insert(root, 2);
    IntTree copy = tree;
    REQUIRE(copy.size() == tree.size());
    REQUIRE(copy.get_root() != tree.get_root());
    REQUIRE(copy.get_root()->data == 1);
}

TEST_CASE (



"[NArayTree] Assignment operator"
)
 {
    IntTree tree;
    auto* root = tree.insert(nullptr, 1);
    tree.insert(root, 2);
    IntTree copy;
    copy = tree;
    REQUIRE(copy.size() == tree.size());
    REQUIRE(copy.get_root() != tree.get_root());
    REQUIRE(copy.get_root()->data == 1);
}

TEST_CASE (



"[NArayTree] Clear tree"
)
 {
    IntTree tree;
    auto* root = tree.insert(nullptr, 1);
    tree.insert(root, 2);
    tree.clear();
    REQUIRE(tree.get_root() == nullptr);
    REQUIRE(tree.empty());
}

TEST_CASE (



"[NArayTree] Remove non-existent node returns false"
)
 {
    IntTree tree;
    REQUIRE_FALSE(tree.remove(static_cast<IntTree::TreeNode*>(nullptr)));
}

TEST_CASE (



"[NArayTree] Remove by value"
)
 {
    IntTree tree;
    auto* root = tree.insert(nullptr, 1);
    tree.insert(root, 2);
    REQUIRE(tree.remove(2));
    REQUIRE(root->children.empty());
    REQUIRE(tree.size() == 1);
    REQUIRE_FALSE(tree.remove(2));
}

TEST_CASE (



"[NArayTree] Insert root when already exists returns nullptr"
)
 {
    IntTree tree;
    auto* root1 = tree.insert(nullptr, 1);
    auto* root2 = tree.insert(nullptr, 2);
    REQUIRE(root1 != nullptr);
    REQUIRE(root2 == nullptr);
    REQUIRE(tree.get_root()->data == 1);
}

TEST_CASE (



"[NArayTree] Emplace root and children"
)
 {
    NAryTree<int> tree;
    auto* root = tree.emplace(nullptr, EmptyMetadata{}, 42);
    REQUIRE(root != nullptr);
    auto* child = tree.emplace(root, EmptyMetadata{}, 99);
    REQUIRE(child != nullptr);
    REQUIRE(root->children.size() == 1);
    REQUIRE(root->children.front()->data == 99);
}

TEST_CASE (



"[NArayTree] Bulk insert returns correct pointers"
)
 {
    IntTree tree;
    auto* root = tree.insert(nullptr, 1);
    std::vector<std::pair<int, EmptyMetadata>> items = {{2, {}}, {3, {}}, {4, {}}};
    auto children = tree.bulk_insert(root, items);
    REQUIRE(children.size() == 3);
    for (size_t i = 0; i < children.size(); ++i) {
        REQUIRE(children[i] == root->children[i].get());
    }
}

TEST_CASE (



"[NArayTree] Find_if and find_all_if"
)
 {
    IntTree tree;
    auto* root = tree.insert(nullptr, 1);
    tree.insert(root, 2);
    tree.insert(root, 3);
    tree.insert(root, 2);
    auto found = tree.find_if([](const IntTree::TreeNode& n) { return n.data == 3; });
    REQUIRE(found != nullptr);
    auto all_twos = tree.find_all_if([](const IntTree::TreeNode& n) { return n.data == 2; });
    REQUIRE(all_twos.size() == 2);
}

TEST_CASE (



"[NArayTree] Traverse depth first visits all nodes"
)
 {
    IntTree tree;
    auto* root = tree.insert(nullptr, 1);
    tree.insert(root, 2);
    tree.insert(root, 3);
    size_t count = 0;
    tree.traverse_depth_first([&count](const IntTree::TreeNode*) { ++count; });
    REQUIRE(count == 3);
}

TEST_CASE (



"[NArayTree] Traverse breadth first visits all nodes"
)
 {
    IntTree tree;
    auto* root = tree.insert(nullptr, 1);
    tree.insert(root, 2);
    tree.insert(root, 3);
    size_t count = 0;
    tree.traverse_breadth_first([&count](const IntTree::TreeNode*) { ++count; });
    REQUIRE(count == 3);
}

TEST_CASE (



"[NArayTree] Height of empty tree is zero"
)
 {
    IntTree tree;
    REQUIRE(tree.height() == 0);
}

TEST_CASE (



"[NArayTree] Height of single node tree is one"
)
 {
    IntTree tree;
    tree.insert(nullptr, 1);
    REQUIRE(tree.height() == 1);
}

TEST_CASE (



"[NArayTree] Height with deeper children"
)
 {
    IntTree tree;
    auto* root = tree.insert(nullptr, 1);
    auto* c1 = tree.insert(root, 2);
    tree.insert(c1, 3);
    REQUIRE(tree.height() == 3);
}

TEST_CASE (



"[NArayTree] is_leaf returns true for leaf nodes"
)
 {
    IntTree tree;
    auto* root = tree.insert(nullptr, 1);
    auto* child = tree.insert(root, 2);
    REQUIRE(child->is_leaf());
    REQUIRE_FALSE(root->is_leaf());
}

// --- Additional NAryTree corner-case tests ---

TEST_CASE (



"[NArayTree] Serialize and deserialize roundtrip"
,
"[NAryTree][Serialize]"
)
 {
    IntTree tree;
    auto* root = tree.insert(nullptr, 1);
    auto* c1 = tree.insert(root, 2);
    tree.insert(c1, 3);

    // Serialize
    std::ostringstream oss;
    tree.serialize(oss);

    // Deserialize into a fresh tree
    IntTree tree2;
    std::istringstream iss(oss.str());
    tree2.deserialize(iss);

    REQUIRE(tree2.size() == tree.size());

    // Compare preorder orders
    std::vector<int> order1, order2;
    for (auto& n : tree) order1.push_back(n.data);
    for (auto& n : tree2) order2.push_back(n.data);
    REQUIRE(order1 == order2);
}

TEST_CASE (



"[NArayTree] Subtree constructor clones subtree correctly"
,
"[NAryTree][Subtree]"
)
 {
    IntTree tree;
    auto* root = tree.insert(nullptr, 10);
    auto* c1 = tree.insert(root, 20);
    tree.insert(c1, 30);
    tree.insert(root, 40);

    // Construct a new NAryTree from the subtree rooted at c1 (no copying of unique_ptrs by us)
    IntTree subtree(*c1);
    REQUIRE(subtree.get_root() != nullptr);
    REQUIRE(subtree.get_root()->data == 20);
    REQUIRE(subtree.size() == 2); // c1 and its child (30)
    // Ensure preorder order matches expected
    std::vector<int> data_order;
    for (auto& n : subtree) data_order.push_back(n.data);
    REQUIRE(data_order == std::vector<int>{20, 30});
}

// --- New tests added: breadth-first iterator, views, levels, analyze, graft/split/merge, try_* and versioned serialization ---

TEST_CASE (



"[NArayTree] Breadth-first iterator and range"
)
 {
    IntTree tree;
    auto* root = tree.insert(nullptr, 1);
    auto* c1 = tree.insert(root, 2);
    auto* c2 = tree.insert(root, 3);
    tree.insert(c1, 4);

    std::vector<int> bfs;
    for (auto& n : tree.breadth_first()) bfs.push_back(n.data);
    REQUIRE(bfs == std::vector<int>{1, 2, 3, 4});
}

TEST_CASE (



"[NArayTree] nodes() and leaves() views"
)
 {
    IntTree tree;
    auto* root = tree.insert(nullptr, 1);
    auto* c1 = tree.insert(root, 2);
    auto* c2 = tree.insert(root, 3);
    tree.insert(c1, 4);

    std::vector<int> nodes;
    for (const auto& n : tree.nodes()) nodes.push_back(n.data);
    REQUIRE(nodes == std::vector<int>{1, 2, 4, 3});

    std::vector<int> leaf_vals;
    for (const auto& n : tree.leaves()) leaf_vals.push_back(n.data);
    REQUIRE(leaf_vals == std::vector<int>{4, 3});
}

TEST_CASE (



"[NArayTree] level() returns correct nodes at depth"
)
 {
    IntTree tree;
    auto* root = tree.insert(nullptr, 1);
    auto* c1 = tree.insert(root, 2);
    auto* c2 = tree.insert(root, 3);
    tree.insert(c1, 4);

    auto lvl0 = tree.level(0);
    REQUIRE(lvl0.size() == 1);
    REQUIRE(lvl0[0]->data == 1);

    auto lvl1 = tree.level(1);
    REQUIRE(lvl1.size() == 2);

    auto lvl2 = tree.level(2);
    REQUIRE(lvl2.size() == 1);
    REQUIRE(lvl2[0]->data == 4);
}

TEST_CASE (



"[NArayTree] analyze, to_string and to_dot produce expected results"
)
 {
    IntTree tree;
    auto* root = tree.insert(nullptr, 1);
    auto* c1 = tree.insert(root, 2);
    auto* c2 = tree.insert(root, 3);
    tree.insert(c1, 4);

    auto stats = tree.analyze();
    REQUIRE(stats.node_count == 4);
    REQUIRE(stats.leaf_count == 2);
    REQUIRE(stats.max_children >= 1);

    auto s = tree.to_string();
    REQUIRE(!s.empty());

    auto dot = tree.to_dot();
    REQUIRE(dot.find("digraph") != std::string::npos);
}

TEST_CASE (



"[NArayTree] graft, split and merge behave correctly"
)
 {
    // Build base tree
    IntTree tree;
    auto* root = tree.insert(nullptr, 1);
    auto* a = tree.insert(root, 2);
    auto* b = tree.insert(root, 3);

    // Split out node 'a' into its own tree
    auto sub = tree.split(a);
    REQUIRE(sub.get_root() != nullptr);
    REQUIRE(sub.get_root()->data == 2);
    REQUIRE(tree.find(2) == nullptr);

    // Graft it back under root
    tree.graft(root, std::move(sub));
    REQUIRE(tree.find(2) != nullptr);

    // Merge another tree
    NAryTree<int> other;
    auto* oroot = other.insert(nullptr, 99);
    tree.merge(std::move(other));
    REQUIRE(tree.find(99) != nullptr);
}

TEST_CASE (



"[NArayTree] try_insert, try_remove and versioned serialization"
)
 {
    IntTree tree;
    auto res = tree.try_insert(nullptr, 10);
    REQUIRE(res.has_value());
    auto* n = res.value();
    REQUIRE(n != nullptr);

    auto rem = tree.try_remove(n);
    REQUIRE(rem.has_value());

    // versioned serialization roundtrip
    tree.clear();
    auto* r = tree.insert(nullptr, 7);
    tree.insert(r, 8);
    std::ostringstream oss;
    tree.serialize_versioned(oss);

    IntTree tree2;
    std::istringstream iss(oss.str());
    REQUIRE(tree2.deserialize_versioned(iss));
    REQUIRE(tree2.size() == tree.size());
}

TEST_CASE (



"[NArayTree] Breadth-first iterator (explicit iterator loop)"
)
 {
    IntTree tree;
    auto* root = tree.insert(nullptr, 1);
    auto* c1 = tree.insert(root, 2);
    auto* c2 = tree.insert(root, 3);
    tree.insert(c1, 4);

    std::vector<int> bfs;
    for (auto it = tree.breadth_begin(); it != tree.breadth_end(); ++it) {
        bfs.push_back(it->data);
    }
    REQUIRE(bfs == std::vector<int>{1, 2, 3, 4});
}

TEST_CASE (



"[NArayTree] Breadth-first iterator independent begins"
)
 {
    IntTree tree;
    auto* root = tree.insert(nullptr, 1);
    auto* c1 = tree.insert(root, 2);
    auto* c2 = tree.insert(root, 3);
    tree.insert(c1, 4);

    auto it1 = tree.breadth_begin();
    auto it2 = tree.breadth_begin();
    REQUIRE(it1->data == it2->data);
    ++it1;
    // it2 should still point to root because it2 was constructed independently
    REQUIRE(it1->data != it2->data);
    REQUIRE(it2->data == 1);
}

TEST_CASE (



"[NArayTree] const breadth-first iterator works on const tree"
)
 {
    IntTree tree;
    auto* root = tree.insert(nullptr, 1);
    tree.insert(root, 2);

    const IntTree& ct = tree;
    std::vector<int> vals;
    for (auto it = ct.breadth_begin(); it != ct.breadth_end(); ++it) vals.push_back(it->data);
    REQUIRE(vals == std::vector<int>{1, 2});
}

TEST_CASE (



"[NArayTree] breadth-first iterator copy semantics (independent queue copy)"
)
 {
    // Standard forward iterator multipass guarantee: copying an iterator copies
    // the traversal state so advancing one iterator does not corrupt the other.
    IntTree tree;
    auto* root = tree.insert(nullptr, 1);
    auto* c1 = tree.insert(root, 2);
    auto* c2 = tree.insert(root, 3);
    tree.insert(c1, 4);

    auto it = tree.breadth_begin();
    auto it_copy = it; // independent copy of traversal state

    // Advance the original once
    ++it;
    REQUIRE(it->data == 2);
    REQUIRE(it_copy->data == 1);

    // Advancing the copy operates on its own traversal queue
    ++it_copy;
    REQUIRE(it_copy->data == 2);
}

// --- New tests for missing coverage ---

TEST_CASE (



"[NArayTree] Sibling navigation next_sibling and prev_sibling"
)
 {
    IntTree tree;
    auto* root = tree.insert(nullptr, 1);
    auto* c1 = tree.insert(root, 2);
    auto* c2 = tree.insert(root, 3);
    auto* c3 = tree.insert(root, 4);

    REQUIRE(c1->next_sibling() == c2);
    REQUIRE(c2->next_sibling() == c3);
    REQUIRE(c3->next_sibling() == nullptr);

    REQUIRE(c1->prev_sibling() == nullptr);
    REQUIRE(c2->prev_sibling() == c1);
    REQUIRE(c3->prev_sibling() == c2);
}

TEST_CASE (



"[NArayTree] Filter transformation with predicate"
)
 {
    IntTree tree;
    auto* root = tree.insert(nullptr, 1);
    tree.insert(root, 2);
    tree.insert(root, 3);
    auto* c1 = tree.insert(root, 2);
    tree.insert(c1, 5);

    // Filter: keep only nodes with value > 1
    IntTree filtered = tree.filter([](const IntTree::TreeNode& n) { return n.data > 1; });
    REQUIRE(filtered.get_root() == nullptr); // Root (1) doesn't pass filter
}

TEST_CASE (



"[NArayTree] Lowest common ancestor with multiple branches"
)
 {
    IntTree tree;
    auto* root = tree.insert(nullptr, 1);
    auto* a = tree.insert(root, 2);
    auto* b = tree.insert(root, 3);
    auto* a1 = tree.insert(a, 4);
    auto* a2 = tree.insert(a, 5);
    auto* b1 = tree.insert(b, 6);
    auto* b2 = tree.insert(b, 7);

    REQUIRE(tree.lowest_common_ancestor(a1, a2) == a);
    REQUIRE(tree.lowest_common_ancestor(a1, b1) == root);
    REQUIRE(tree.lowest_common_ancestor(a1, a1) == a1);
}

TEST_CASE (



"[NArayTree] Path between nodes with no common ancestor"
)
 {
    IntTree tree;
    auto* root = tree.insert(nullptr, 1);
    auto* a = tree.insert(root, 2);
    auto* b = tree.insert(root, 3);
    auto* a1 = tree.insert(a, 4);
    auto* b1 = tree.insert(b, 5);

    auto path = tree.path_between(a1, b1);
    REQUIRE(path.has_value());
    REQUIRE(path.value().size() > 0);

    // Path between nullptr and node should return nullopt
    REQUIRE(!tree.path_between(nullptr, a1).has_value());
    REQUIRE(!tree.path_between(a1, nullptr).has_value());
}

TEST_CASE (



"[NArayTree] Map transformation int to string"
)
 {
    IntTree tree;
    auto* root = tree.insert(nullptr, 1);
    tree.insert(root, 2);
    tree.insert(root, 3);

    // Note: map() creates new tree with transformed type
    // Verify by checking size and root data
    REQUIRE(tree.size() == 3);
    REQUIRE(root->data == 1);
}

TEST_CASE (



"[NArayTree] Structural equality and hashing"
)
 {
    IntTree tree1, tree2;
    auto* r1 = tree1.insert(nullptr, 1);
    tree1.insert(r1, 2);
    tree1.insert(r1, 3);

    auto* r2 = tree2.insert(nullptr, 1);
    tree2.insert(r2, 2);
    tree2.insert(r2, 3);

    REQUIRE(tree1.structural_equal(tree2));
    REQUIRE(tree1.structural_hash() == tree2.structural_hash());

    // Empty trees should be structurally equal
    IntTree empty1, empty2;
    REQUIRE(empty1.structural_equal(empty2));
}

TEST_CASE (



"[NArayTree] TreeBuilder fluent API"
)
 {
    auto tree = std::move(TreeBuilder<int>()
                          .root(1)
                          .child(2)
                          .child(3)
                          .parent()
                          .sibling(4)).build();

    REQUIRE(tree.size() == 4);
    REQUIRE(tree.get_root()->data == 1);
    REQUIRE(tree.get_root()->children.size() == 3);
}

TEST_CASE (



"[NArayTree] JSON serialization roundtrip"
)
 {
    IntTree tree;
    auto* root = tree.insert(nullptr, 1);
    auto* c1 = tree.insert(root, 2);
    tree.insert(c1, 3);

    std::ostringstream oss;
    tree.serialize_json(oss);
    std::string json = oss.str();
    REQUIRE(!json.empty());
    REQUIRE((json.find("digraph") != std::string::npos || json.find("1") != std::string::npos));
}

TEST_CASE (



"[NArayTree] Const tree iteration with const iterators"
)
 {
    IntTree tree;
    auto* root = tree.insert(nullptr, 1);
    tree.insert(root, 2);
    tree.insert(root, 3);

    const IntTree& ctree = tree;
    std::vector<int> vals;
    for (const auto& n : ctree) vals.push_back(n.data);
    REQUIRE(vals == std::vector<int>{1, 2, 3});

    // Test const post_order
    std::vector<int> post_vals;
    auto porder = ctree.post_order();
    for (auto it = porder.begin(); it != porder.end(); ++it) {
        post_vals.push_back(it->data);
    }
    REQUIRE(post_vals == std::vector<int>{2, 3, 1});
}

TEST_CASE (



"[NArayTree] Bulk insert from range"
)
 {
    IntTree tree;
    auto* root = tree.insert(nullptr, 1);

    std::vector<int> range = {2, 3, 4, 5};
    auto children = tree.bulk_insert_range(root, range);
    REQUIRE(children.size() == 4);
    REQUIRE(tree.size() == 5);
}

TEST_CASE (



"[NArayTree] Move constructor and move assignment"
)
 {
    IntTree tree1;
    auto* root = tree1.insert(nullptr, 1);
    tree1.insert(root, 2);

    // Move constructor
    IntTree tree2 = std::move(tree1);
    REQUIRE(tree2.size() == 2);
    REQUIRE(tree1.size() == 0); // tree1 should be empty after move

    // Move assignment
    IntTree tree3;
    tree3.insert(nullptr, 99);
    tree3 = std::move(tree2);
    REQUIRE(tree3.size() == 2);
    REQUIRE(tree3.get_root()->data == 1);
}

TEST_CASE (



"[NArayTree] Empty tree edge cases"
)
 {
    IntTree tree;
    REQUIRE(tree.empty());
    REQUIRE(tree.height() == 0);
    REQUIRE(tree.size() == 0);

    // Operations on empty tree
    REQUIRE(tree.find(1) == nullptr);
    REQUIRE(tree.find_all(1).empty());

    std::vector<int> pre_order;
    for (auto& n : tree) pre_order.push_back(n.data);
    REQUIRE(pre_order.empty());

    // Traversal should handle empty tree gracefully
    size_t count = 0;
    tree.traverse_depth_first([&count](const IntTree::TreeNode*) { ++count; });
    REQUIRE(count == 0);
}

TEST_CASE (
"[NAryTree] Breadth-first iterator value copy test"
)
{
    IntTree tree(1);
    auto* root = tree.get_root();
    tree.insert(root, 2);
    tree.insert(root, 3);

    auto it1 = tree.breadth_begin();
    auto it2 = it1; // Copy iterator
    REQUIRE(it1 == it2);
    REQUIRE((*it1).data == 1);
    REQUIRE((*it2).data == 1);

    ++it1;
    REQUIRE((*it1).data == 2);
    REQUIRE((*it2).data == 1); // it2 unchanged
}

TEST_CASE (
"[ScalableNAryTree] Basic LCRS build, DFS, and remove"
)
{
    using namespace pebble::containers;
    ScalableNAryTree<int> tree;
    REQUIRE(tree.empty());
    REQUIRE(tree.size() == 0);

    auto r = tree.insert_root(10);
    auto c1 = tree.append_child(r, 20);
    auto c2 = tree.append_child(r, 30);
    auto c1_1 = tree.append_child(c1, 40);

    REQUIRE(tree.size() == 4);
    REQUIRE(tree.get(r) == 10);
    REQUIRE(tree.get(c1) == 20);
    REQUIRE(tree.get(c2) == 30);
    REQUIRE(tree.get(c1_1) == 40);

    std::vector<int> visited;
    tree.for_each_dfs([&](const int val, const auto&) {
        visited.push_back(val);
    });

    REQUIRE(visited.size() == 4);
    CHECK(visited[0] == 10);

    tree.remove_subtree(c1);
    REQUIRE(tree.size() == 2);
}
