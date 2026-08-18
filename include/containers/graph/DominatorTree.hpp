#pragma once

#include "LiteGraph.hpp"
#include "LiteGraphAlgorithms.hpp"
#include "../tree/NAryTree.hpp"
#include <vector>
#include <span>
#include <concepts>
#include <optional>
#include <functional>
#include <algorithm>
#include <queue>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <string>

namespace litegraph {
    // C++23 Concepts for Dominator Tree
    template <typename T>
    concept DominatorTreeNode = requires(T t) {
        { t.value } -> std::convertible_to<size_t>;
    };

    template <typename GraphT>
    concept DirectedGraphForDominators = LiteGraphModel<GraphT> &&
        std::is_same_v<typename GraphT::directed_tag, Directed>;

    // Error types for dominator tree computation
    enum class DominatorError {
        InvalidStartNode,
        GraphNotDirected,
        UnreachableNodes,
        ComputationFailed
    };

    namespace detail {
        // Modern C++23 DSU implementation for dominator computation
        class DominatorDSU {
        private:
            std::vector<size_t> parent_;
            std::vector<size_t> label_;
            std::vector<size_t> size_;

        public:
            explicit constexpr DominatorDSU(const size_t n)
                : parent_(n), label_(n), size_(n, 1) {
                for (size_t i = 0; i < n; ++i) {
                    parent_[i] = i;
                    label_[i] = i;
                }
            }

            constexpr void unite(const size_t i, const size_t j) noexcept {
                size_t root_i = find(i);
                if (size_t root_j = find(j); root_i != root_j) {
                    if (size_[root_i] < size_[root_j]) [[likely]] {
                        std::swap(root_i, root_j);
                    }
                    parent_[root_j] = root_i;
                    size_[root_i] += size_[root_j];
                }
            }

            constexpr size_t find(const size_t i) noexcept {
                if (parent_[i] == i) [[likely]] return i;

                const size_t root = find(parent_[i]);
                // Path compression with label optimization
                if (label_[parent_[i]] < label_[i]) [[unlikely]] {
                    label_[i] = label_[parent_[i]];
                }
                parent_[i] = root;
                return root;
            }

            constexpr size_t eval(const size_t i) noexcept {
                find(i);
                return label_[i];
            }

            [[nodiscard]] constexpr std::span<const size_t> parents() const noexcept {
                return parent_;
            }

            [[nodiscard]] constexpr std::span<const size_t> labels() const noexcept {
                return label_;
            }
        };

        // Predecessor computation (with potential for future parallel optimization)
        template <DirectedGraphForDominators GraphT>
        auto compute_predecessors(const GraphT& g, std::span<const int> dfs_num) {
            const auto node_cap = g.node_capacity();
            std::vector<std::vector<NodeId>> pred(node_cap);

            // Sequential implementation (can be parallelized in future)
            for (const auto& [eid_val, edge] : g.edges()) {
                if (dfs_num[edge.to.value] != -1) {
                    pred[edge.to.value].push_back(edge.from);
                }
            }

            return pred;
        }
    } // namespace detail

    /**
     * @brief Modern C++23 implementation of Lengauer-Tarjan Dominator Tree algorithm.
     *
     * This function computes dominator relationships efficiently using C++23 features,
     * concepts for type safety, and improved algorithms while maintaining API compatibility.
     *
     * @tparam GraphT The graph type conforming to LiteGraphModel concept
     * @param g The directed graph to analyze
     * @param start_node The entry point (root) of the dominator tree
     * @return NAryTree representing the dominator relationships
     */
    template <LiteGraphModel GraphT>
    auto compute_dominator_tree(const GraphT& g, NodeId start_node) -> NAryTree<NodeId> {
        static_assert(std::is_same_v<typename GraphT::directed_tag, Directed>,
                      "Dominator Tree is only defined for directed graphs.");

        if (!g.valid_node(start_node)) {
            throw std::out_of_range("compute_dominator_tree: start_node is invalid");
        }

        const auto node_cap = g.node_capacity();

        // Reachability pass from entry.
        std::vector<bool> reachable(node_cap, false);
        std::vector<NodeId> postorder;
        postorder.reserve(g.node_count());

        std::function<void(NodeId)> dfs = [&](NodeId u) {
            reachable[u.value] = true;
            for (const auto v : g.neighbors(u)) {
                if (!reachable[v.value]) {
                    dfs(v);
                }
            }
            postorder.push_back(u);
        };
        dfs(start_node);

        std::vector rpo(postorder.rbegin(), postorder.rend());
        std::vector<int> rpo_index(node_cap, -1);
        for (size_t i = 0; i < rpo.size(); ++i) {
            rpo_index[rpo[i].value] = static_cast<int>(i);
        }

        std::vector<std::vector<NodeId>> pred(node_cap);
        for (const auto& [eid_val, edge] : g.edges()) {
            if (reachable[edge.from.value] && reachable[edge.to.value]) {
                pred[edge.to.value].push_back(edge.from);
            }
        }

        // Iterative immediate-dominator solver (Cooper-Harvey-Kennedy style).
        std::vector<NodeId> idom(node_cap, INVALID_NODE_ID);
        idom[start_node.value] = start_node;

        auto intersect = [&](const NodeId a, const NodeId b) {
            NodeId finger1 = a;
            NodeId finger2 = b;

            while (finger1.value != finger2.value) {
                while (rpo_index[finger1.value] > rpo_index[finger2.value]) {
                    finger1 = idom[finger1.value];
                }
                while (rpo_index[finger2.value] > rpo_index[finger1.value]) {
                    finger2 = idom[finger2.value];
                }
            }
            return finger1;
        };

        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto b : rpo) {
                if (b.value == start_node.value) {
                    continue;
                }

                NodeId new_idom = INVALID_NODE_ID;
                for (const auto p : pred[b.value]) {
                    if (!idom[p.value].is_valid()) {
                        continue;
                    }
                    if (!new_idom.is_valid()) {
                        new_idom = p;
                    }
                    else {
                        new_idom = intersect(p, new_idom);
                    }
                }

                if (new_idom.is_valid() && idom[b.value] != new_idom) {
                    idom[b.value] = new_idom;
                    changed = true;
                }
            }
        }

        // Build the dominator tree from the idom relation.
        NAryTree dom_tree(start_node);
        std::vector<std::vector<NodeId>> dom_children(node_cap);
        for (const auto n : rpo) {
            if (n.value != start_node.value && idom[n.value].is_valid()) {
                dom_children[idom[n.value].value].push_back(n);
            }
        }

        std::vector<NAryTree<NodeId>::TreeNode*> tree_nodes(node_cap, nullptr);
        tree_nodes[start_node.value] = dom_tree.get_root();

        std::queue<NodeId> q;
        q.push(start_node);
        while (!q.empty()) {
            const NodeId parent = q.front();
            q.pop();

            auto* parent_node = tree_nodes[parent.value];
            for (const auto child : dom_children[parent.value]) {
                auto* child_node = dom_tree.insert(parent_node, child);
                tree_nodes[child.value] = child_node;
                q.push(child);
            }
        }

        return dom_tree;
    }

    // Utility functions for dominator tree analysis
    namespace dominator_analysis {
        /**
         * @brief Check if node A dominates node B in the dominator tree
         */
        template <DominatorTreeNode NodeT>
        [[nodiscard]] constexpr bool dominates(const NAryTree<NodeT>& dom_tree,
                                               const NodeT& a, const NodeT& b) noexcept {
            auto* b_node = dom_tree.find_if([&](const auto& node) {
                return node.data.value == b.value;
            });

            if (!b_node) [[unlikely]] return false;

            // Walk up the dominator tree
            for (const auto* current = b_node; current != nullptr; current = current->parent) {
                if (current->data.value == a.value) [[likely]] return true;
            }
            return false;
        }

        /**
         * @brief Find the lowest common dominator of two nodes
         */
        template <DominatorTreeNode NodeT>
        [[nodiscard]] constexpr std::optional<NodeT> lowest_common_dominator(
            const NAryTree<NodeT>& dom_tree, const NodeT& a, const NodeT& b) noexcept {
            auto* a_node = dom_tree.find_if([&](const auto& node) { return node.data.value == a.value; });
            auto* b_node = dom_tree.find_if([&](const auto& node) { return node.data.value == b.value; });

            if (!a_node || !b_node) [[unlikely]] return std::nullopt;

            // Collect ancestors of both nodes
            std::vector<const typename NAryTree<NodeT>::TreeNode*> a_ancestors, b_ancestors;

            for (auto* current = a_node; current; current = current->parent) {
                a_ancestors.push_back(current);
            }
            for (auto* current = b_node; current; current = current->parent) {
                b_ancestors.push_back(current);
            }

            // Find common ancestors from root down
            std::ranges::reverse(a_ancestors);
            std::ranges::reverse(b_ancestors);

            const typename NAryTree<NodeT>::TreeNode* lca = nullptr;
            auto min_size = std::min(a_ancestors.size(), b_ancestors.size());

            for (size_t i = 0; i < min_size && a_ancestors[i] == b_ancestors[i]; ++i) {
                lca = a_ancestors[i];
            }

            return lca ? std::optional<NodeT>{lca->data} : std::nullopt;
        }

        /**
         * @brief Get all nodes dominated by a given node
         */
        template <DominatorTreeNode NodeT>
        [[nodiscard]] constexpr std::vector<NodeT> get_dominated_nodes(
            const NAryTree<NodeT>& dom_tree, const NodeT& dominator) {
            std::vector<NodeT> result;
            auto* dom_node = dom_tree.find_if([&](const auto& node) {
                return node.data.value == dominator.value;
            });

            if (!dom_node) [[unlikely]] return result;

            // Collect all descendants
            std::function<void(const typename NAryTree<NodeT>::TreeNode*)> collect_descendants =
                [&](const auto* node) {
                if (node != dom_node) {
                    // Don't include the dominator itself
                    result.push_back(node->data);
                }
                for (const auto* child : node->children) {
                    collect_descendants(child);
                }
            };

            collect_descendants(dom_node);
            return result;
        }
    } // namespace dominator_analysis

    // -------------------------------------------------------------------------
    // Graph-view input and result types for backend-neutral dominator queries.
    // These do not depend on LiteGraph internals; MIR CFG, LiteGraph, and other
    // backends can all construct a dominator_graph_view and call compute_dominators.
    // -------------------------------------------------------------------------

    // Graph representation consumed by compute_dominators.
    // Caller fills nodes, entry, predecessors, and successors.
    template <typename NodeId>
    struct dominator_graph_view {
        NodeId entry;
        std::vector<NodeId> nodes;
        std::unordered_map<NodeId, std::vector<NodeId>> predecessors;
        std::unordered_map<NodeId, std::vector<NodeId>> successors;
    };

    // Result of compute_dominators.  Check ok() before using the maps.
    template <typename NodeId>
    struct dominator_result {
        NodeId entry;
        std::unordered_map<NodeId, std::optional<NodeId>> immediate_dominator;
        std::unordered_map<NodeId, std::unordered_set<NodeId>> dominated_children;
        std::unordered_map<NodeId, std::unordered_set<NodeId>> dominance_frontier;
        std::vector<std::string> diagnostics;
        // Non-fatal notes (e.g. unreachable nodes).  ok() ignores these.
        std::vector<std::string> warnings;

        [[nodiscard]] bool ok() const noexcept { return diagnostics.empty(); }
    };

    struct dominator_options {
        bool compute_frontier = true;
    };

    // Compute dominators from a backend-neutral graph view.
    // Uses the same Cooper-Harvey-Kennedy iterative algorithm as compute_dominator_tree
    // but accepts any NodeId type that is hashable and equality-comparable.
    // Returns errors as diagnostics rather than throwing.
    template <typename NodeId>
    void compute_dominance_frontier(
        dominator_graph_view<NodeId> const& view,
        dominator_result<NodeId>& result
    );

    template <typename NodeId>
    auto compute_dominators(dominator_graph_view<NodeId> const& view,
                            const dominator_options options)
        -> dominator_result<NodeId> {
        dominator_result<NodeId> result;
        result.entry = view.entry;

        // --- validation ---
        if (view.nodes.empty()) {
            result.diagnostics.push_back("compute_dominators: graph has no nodes");
            return result;
        }

        // entry must be present in the node list
        bool entry_found = false;
        for (auto const& n : view.nodes) {
            if (n == view.entry) {
                entry_found = true;
                break;
            }
        }
        if (!entry_found) {
            result.diagnostics.push_back("compute_dominators: entry node is not in nodes list");
            return result;
        }

        // predecessor/successor refs must name nodes that exist
        std::unordered_set<NodeId> node_set(view.nodes.begin(), view.nodes.end());
        for (auto const& [node, preds] : view.predecessors) {
            if (!node_set.count(node)) {
                result.diagnostics.push_back("compute_dominators: predecessors key is not in nodes list");
            }
            for (auto const& p : preds) {
                if (!node_set.count(p)) {
                    result.diagnostics.push_back("compute_dominators: invalid predecessor reference");
                }
            }
        }
        for (auto const& [node, succs] : view.successors) {
            if (!node_set.count(node)) {
                result.diagnostics.push_back("compute_dominators: successors key is not in nodes list");
            }
            for (auto const& s : succs) {
                if (!node_set.count(s)) {
                    result.diagnostics.push_back("compute_dominators: invalid successor reference");
                }
            }
        }
        if (!result.ok()) return result;

        // --- RPO via iterative DFS from entry ---
        std::unordered_set<NodeId> visited;
        std::vector<NodeId> postorder;
        postorder.reserve(view.nodes.size());

        // Iterative DFS to avoid stack overflow on large CFGs.
        std::vector<std::pair<NodeId, bool>> stack; // (node, expanded)
        stack.emplace_back(view.entry, false);
        while (!stack.empty()) {
            auto& [u, expanded] = stack.back();
            if (!expanded) {
                if (visited.count(u)) {
                    stack.pop_back();
                    continue;
                }
                visited.insert(u);
                expanded = true;
                auto it = view.successors.find(u);
                if (it != view.successors.end()) {
                    for (auto const& v : it->second) {
                        if (!visited.count(v)) stack.emplace_back(v, false);
                    }
                }
            }
            else {
                postorder.push_back(u);
                stack.pop_back();
            }
        }

        std::vector<NodeId> rpo(postorder.rbegin(), postorder.rend());
        std::unordered_map<NodeId, int> rpo_index;
        for (int i = 0; i < static_cast<int>(rpo.size()); ++i) rpo_index[rpo[i]] = i;

        // Nodes in the graph but not reachable from entry get a null idom and
        // a non-fatal warning.  They do not fail the analysis (ok() stays true).
        for (auto const& n : view.nodes) {
            if (!visited.count(n)) {
                result.warnings.push_back(
                    "compute_dominators: node unreachable from entry (idom left null)");
            }
        }

        // --- iterative idom solver (Cooper-Harvey-Kennedy) ---
        std::unordered_map<NodeId, std::optional<NodeId>> idom;
        for (auto const& n : view.nodes) idom[n] = std::nullopt;
        idom[view.entry] = view.entry;

        auto intersect = [&](NodeId a, NodeId b) -> NodeId {
            while (a != b) {
                while (rpo_index.count(a) && rpo_index.count(b) &&
                    rpo_index.at(a) > rpo_index.at(b)) {
                    a = idom.at(a).value();
                }
                while (rpo_index.count(a) && rpo_index.count(b) &&
                    rpo_index.at(b) > rpo_index.at(a)) {
                    b = idom.at(b).value();
                }
            }
            return a;
        };

        bool changed = true;
        while (changed) {
            changed = false;
            for (auto const& b : rpo) {
                if (b == view.entry) continue;
                auto pred_it = view.predecessors.find(b);
                if (pred_it == view.predecessors.end()) continue;

                std::optional<NodeId> new_idom;
                for (auto const& p : pred_it->second) {
                    if (!idom.at(p).has_value()) continue;
                    if (!new_idom.has_value()) {
                        new_idom = p;
                    }
                    else {
                        new_idom = intersect(p, new_idom.value());
                    }
                }

                if (new_idom.has_value() && idom[b] != new_idom) {
                    idom[b] = new_idom;
                    changed = true;
                }
            }
        }

        result.immediate_dominator = idom;

        // --- dominated_children from idom ---
        for (auto const& n : rpo) {
            result.dominated_children[n]; // ensure key exists
            if (n == view.entry) continue;
            if (auto const& parent = idom.at(n); parent.has_value() && parent.value() != n) {
                result.dominated_children[parent.value()].insert(n);
            }
        }

        if (options.compute_frontier)
            compute_dominance_frontier(view, result);

        return result;
    }

    template <typename NodeId>
    auto compute_dominators(dominator_graph_view<NodeId> const& view)
        -> dominator_result<NodeId> {
        return compute_dominators(view, dominator_options{});
    }

    // Populate result.dominance_frontier using the standard algorithm:
    // for every join point (≥2 predecessors), walk each predecessor up the idom
    // chain until idom(join_point), adding the join point to each runner's frontier.
    // result.immediate_dominator must be populated before calling this.
    template <typename NodeId>
    void compute_dominance_frontier(dominator_graph_view<NodeId> const& view,
                                    dominator_result<NodeId>& result) {
        for (auto const& n : view.nodes) result.dominance_frontier[n];
        for (auto const& b : view.nodes) {
            auto pred_it = view.predecessors.find(b);
            if (pred_it == view.predecessors.end()) continue;
            if (pred_it->second.size() < 2) continue;
            for (auto const& p : pred_it->second) {
                auto runner = p;
                NodeId stop = result.immediate_dominator.count(b) && result.immediate_dominator.at(b).has_value()
                                  ? result.immediate_dominator.at(b).value()
                                  : view.entry;
                while (runner != stop) {
                    result.dominance_frontier[runner].insert(b);
                    auto it = result.immediate_dominator.find(runner);
                    if (it == result.immediate_dominator.end() || !it->second.has_value()) break;
                    if (it->second.value() == runner) break; // at root
                    runner = it->second.value();
                }
            }
        }
    }

    // -------------------------------------------------------------------------
    // Dominance query helpers on dominator_result.
    // All functions are safe: missing nodes return false / empty results.
    // -------------------------------------------------------------------------

    // True iff a dominates b (every path from entry to b passes through a).
    // dominates(r, x, x) is always true.
    template <typename NodeId>
    [[nodiscard]] bool dominates(dominator_result<NodeId> const& r, NodeId a, NodeId b) {
        // Walk b's idom chain upward until we hit a or the root (self-loop).
        auto it = r.immediate_dominator.find(b);
        if (it == r.immediate_dominator.end()) return false;

        NodeId current = b;
        while (true) {
            if (current == a) return true;
            auto cur_it = r.immediate_dominator.find(current);
            if (cur_it == r.immediate_dominator.end() || !cur_it->second.has_value()) return false;
            NodeId parent = cur_it->second.value();
            if (parent == current) return false; // reached root without finding a
            current = parent;
        }
    }

    // Returns the immediate dominator of node, or nullopt if node is the entry
    // (its idom is itself) or is not present in the result.
    template <typename NodeId>
    [[nodiscard]] std::optional<NodeId> immediate_dominator_of(
        dominator_result<NodeId> const& r, NodeId node) {
        auto it = r.immediate_dominator.find(node);
        if (it == r.immediate_dominator.end() || !it->second.has_value()) return std::nullopt;
        // Entry dominates itself; callers treat that as "no dominator above entry".
        if (it->second.value() == node) return std::nullopt;
        return it->second.value();
    }

    // Returns the set of nodes directly dominated by node (one tree level down).
    // Returns an empty set if node is not present or has no children.
    template <typename NodeId>
    [[nodiscard]] std::unordered_set<NodeId> const& dominated_children_of(
        dominator_result<NodeId> const& r, NodeId node) {
        static const std::unordered_set<NodeId> empty{};
        auto it = r.dominated_children.find(node);
        return it != r.dominated_children.end() ? it->second : empty;
    }

    // Returns all nodes transitively dominated by node (the full subtree below node,
    // not including node itself).  Returns an empty vector if node is not present.
    template <typename NodeId>
    [[nodiscard]] std::vector<NodeId> dominated_nodes(
        dominator_result<NodeId> const& r, NodeId node) {
        std::vector<NodeId> out;
        // Iterative BFS over the dominated_children tree.
        std::queue<NodeId> q;
        for (auto const& child : dominated_children_of(r, node)) q.push(child);
        while (!q.empty()) {
            NodeId cur = q.front();
            q.pop();
            out.push_back(cur);
            for (auto const& child : dominated_children_of(r, cur)) q.push(child);
        }
        return out;
    }

    // -------------------------------------------------------------------------
    // Back-edge and loop-header analysis.
    // -------------------------------------------------------------------------

    template <typename NodeId>
    struct back_edge {
        NodeId from;
        NodeId to;
    };

    // Returns all back edges in the CFG: an edge A->B is a back edge iff B dominates A.
    template <typename NodeId>
    [[nodiscard]] std::vector<back_edge<NodeId>> find_back_edges(
        dominator_graph_view<NodeId> const& view,
        dominator_result<NodeId> const& result) {
        std::vector<back_edge<NodeId>> out;
        for (auto const& [from, succs] : view.successors) {
            for (auto const& to : succs) {
                if (dominates(result, to, from)) {
                    out.push_back({from, to});
                }
            }
        }
        return out;
    }

    // Returns the set of loop headers: nodes that are targets of at least one back edge.
    template <typename NodeId>
    [[nodiscard]] std::unordered_set<NodeId> find_loop_headers(
        dominator_graph_view<NodeId> const& view,
        dominator_result<NodeId> const& result) {
        std::unordered_set<NodeId> headers;
        for (auto const& be : find_back_edges(view, result)) {
            headers.insert(be.to);
        }
        return headers;
    }
} // namespace litegraph

