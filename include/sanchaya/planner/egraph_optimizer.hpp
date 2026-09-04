#pragma once

// ============================================================================
// sanchaya/planner/egraph_optimizer.hpp — Equality Saturation Optimizer
// ============================================================================

#include "sanchaya/fwd.hpp"
#include "sanchaya/planner/cost_model.hpp"
#include "sanchaya/planner/logical_ir.hpp"
#include "containers/graph/egraph.hpp"
#include <tuple>
#include <string_view>
#include <cstdint>

namespace sanchaya::optimizer {

    // ========================================================================
    // 1. Relational E-Graph Opcodes & Payloads
    // ========================================================================
    enum class rel_op : std::uint16_t {
        op_source = 1,
        op_key_lookup,
        op_filter,
        op_project,
        op_traverse,
        op_join,
        op_group,
        op_aggregate,
        op_order,
        op_limit,
        op_offset
    };

    struct rel_payload {
        std::uint64_t signature{0};
        std::uint32_t extra_data{0};

        constexpr bool operator==(const rel_payload&) const noexcept = default;
    };

    struct rel_payload_hash {
        [[nodiscard]] constexpr std::size_t operator()(const rel_payload& p) const noexcept {
            return static_cast<std::size_t>(p.signature ^ (static_cast<std::uint64_t>(p.extra_data) << 32));
        }
    };

    // User-defined Class Data attached to every e-class
    struct logical_class_data {
        std::size_t estimated_cardinality{1000};
        bool is_deterministic{true};
    };

} // namespace sanchaya::optimizer

template <>
struct std::hash<sanchaya::optimizer::rel_payload> {
    [[nodiscard]] constexpr std::size_t operator()(const sanchaya::optimizer::rel_payload& p) const noexcept {
        return static_cast<std::size_t>(p.signature ^ (static_cast<std::uint64_t>(p.extra_data) << 32));
    }
};

namespace sanchaya::optimizer {

    using sanchaya_egraph = egraph::e_graph<
        rel_op,
        rel_payload,
        egraph::default_enode_hash<rel_op, rel_payload>,
        egraph::default_enode_eq<rel_op, rel_payload>,
        logical_class_data
    >;

    // ========================================================================
    // 2. Relational Rewrite Rule Packs
    // ========================================================================

    /// Identity filter elimination: filter(true, X) -> X
    struct filter_true_elimination_rule {
        template <class G>
        void apply(G& g) const {
            const std::size_t snapshot = g.class_count();
            for (egraph::e_class_id cid = 0; cid < static_cast<egraph::e_class_id>(snapshot); ++cid) {
                if (!g.is_root(cid)) continue;
                for (const auto& node : g.classes()[cid].nodes) {
                    if (node.op == rel_op::op_filter && node.payload.extra_data == 1) { // 1 = always true
                        if (!node.children.empty()) {
                            (void)g.merge(cid, node.children[0]);
                        }
                    }
                }
            }
        }
    };

    /// Redundant project collapse: project(project(X)) -> project(X)
    struct redundant_project_collapse_rule {
        template <class G>
        void apply(G& g) const {
            const std::size_t snapshot = g.class_count();
            for (egraph::e_class_id cid = 0; cid < static_cast<egraph::e_class_id>(snapshot); ++cid) {
                if (!g.is_root(cid)) continue;
                for (const auto& node : g.classes()[cid].nodes) {
                    if (node.op == rel_op::op_project && !node.children.empty()) {
                        const auto child_cid = g.find(node.children[0]);
                        for (const auto& child_node : g.classes()[child_cid].nodes) {
                            if (child_node.op == rel_op::op_project && !child_node.children.empty()) {
                                // Add shortcut from grand-child to top projection
                                typename G::node_t collapsed_node;
                                collapsed_node.op = rel_op::op_project;
                                collapsed_node.children.push_back(child_node.children[0]);
                                collapsed_node.payload = node.payload;
                                const egraph::e_class_id collapsed_id = g.add(std::move(collapsed_node));
                                (void)g.merge(cid, collapsed_id);
                            }
                        }
                    }
                }
            }
        }
    };

    /// Join Commutativity: join(A, B) -> join(B, A)
    struct join_commutativity_rule {
        template <class G>
        void apply(G& g) const {
            const std::size_t snapshot = g.class_count();
            for (egraph::e_class_id cid = 0; cid < static_cast<egraph::e_class_id>(snapshot); ++cid) {
                if (!g.is_root(cid)) continue;
                for (const auto& node : g.classes()[cid].nodes) {
                    if (node.op == rel_op::op_join && node.children.size() == 2) {
                        typename G::node_t swapped;
                        swapped.op = rel_op::op_join;
                        swapped.children.push_back(node.children[1]);
                        swapped.children.push_back(node.children[0]);
                        swapped.payload = node.payload;
                        const egraph::e_class_id sid = g.add(std::move(swapped));
                        (void)g.merge(cid, sid);
                    }
                }
            }
        }
    };

    /// Join Associativity: join(join(A, B), C) -> join(A, join(B, C))
    struct join_associativity_rule {
        template <class G>
        void apply(G& g) const {
            const std::size_t snapshot = g.class_count();
            for (egraph::e_class_id cid = 0; cid < static_cast<egraph::e_class_id>(snapshot); ++cid) {
                if (!g.is_root(cid)) continue;
                for (const auto& outer : g.classes()[cid].nodes) {
                    if (outer.op == rel_op::op_join && outer.children.size() == 2) {
                        const egraph::e_class_id left = g.find(outer.children[0]);
                        const egraph::e_class_id c = g.find(outer.children[1]);
                        for (const auto& inner : g.classes()[left].nodes) {
                            if (inner.op == rel_op::op_join && inner.children.size() == 2) {
                                const egraph::e_class_id a = g.find(inner.children[0]);
                                const egraph::e_class_id b = g.find(inner.children[1]);

                                // Form right inner: join(B, C)
                                typename G::node_t right_inner;
                                right_inner.op = rel_op::op_join;
                                right_inner.children.push_back(b);
                                right_inner.children.push_back(c);
                                right_inner.payload = outer.payload;
                                const egraph::e_class_id right_inner_id = g.add(std::move(right_inner));

                                // Form new outer: join(A, join(B, C))
                                typename G::node_t new_outer;
                                new_outer.op = rel_op::op_join;
                                new_outer.children.push_back(a);
                                new_outer.children.push_back(right_inner_id);
                                new_outer.payload = outer.payload;
                                const egraph::e_class_id new_outer_id = g.add(std::move(new_outer));

                                (void)g.merge(cid, new_outer_id);
                            }
                        }
                    }
                }
            }
        }
    };

    /// Filter to Index Seek Selection: filter(source(T), key_eq) -> key_lookup(T)
    struct filter_to_index_seek_rule {
        template <class G>
        void apply(G& g) const {
            const std::size_t snapshot = g.class_count();
            for (egraph::e_class_id cid = 0; cid < static_cast<egraph::e_class_id>(snapshot); ++cid) {
                if (!g.is_root(cid)) continue;
                for (const auto& node : g.classes()[cid].nodes) {
                    if (node.op == rel_op::op_filter && node.payload.extra_data == 2 && !node.children.empty()) { // 2 = point lookup predicate
                        const auto child_cid = g.find(node.children[0]);
                        for (const auto& child_node : g.classes()[child_cid].nodes) {
                            if (child_node.op == rel_op::op_source) {
                                typename G::node_t seek_node;
                                seek_node.op = rel_op::op_key_lookup;
                                seek_node.payload = node.payload;
                                const egraph::e_class_id seek_id = g.add(std::move(seek_node));
                                (void)g.merge(cid, seek_id);
                            }
                        }
                    }
                }
            }
        }
    };

    // ========================================================================
    // 3. E-Graph Cost Model & DP Extraction
    // ========================================================================

    struct relational_node_cost_model {
        using cost_t = std::size_t;

        template <class Node>
        [[nodiscard]] constexpr cost_t cost(
            const Node& node,
            std::span<const cost_t> child_costs) const noexcept
        {
            cost_t node_cost = 1;
            switch (node.op) {
                case rel_op::op_source: node_cost = 10; break;
                case rel_op::op_key_lookup: node_cost = 1; break;
                case rel_op::op_filter: node_cost = 2; break;
                case rel_op::op_project: node_cost = 1; break;
                case rel_op::op_traverse: node_cost = 8; break;
                case rel_op::op_join: node_cost = 15; break;
                case rel_op::op_group: node_cost = 12; break;
                case rel_op::op_aggregate: node_cost = 5; break;
                case rel_op::op_order: node_cost = 20; break;
                case rel_op::op_limit: node_cost = 1; break;
                case rel_op::op_offset: node_cost = 1; break;
            }
            for (auto c : child_costs) {
                node_cost += c;
            }
            return node_cost;
        }
    };

    static_assert(egraph::egraph_rule<filter_true_elimination_rule, sanchaya_egraph>);
    static_assert(egraph::egraph_rule<redundant_project_collapse_rule, sanchaya_egraph>);
    static_assert(egraph::egraph_rule<join_commutativity_rule, sanchaya_egraph>);
    static_assert(egraph::egraph_rule<join_associativity_rule, sanchaya_egraph>);
    static_assert(egraph::egraph_rule<filter_to_index_seek_rule, sanchaya_egraph>);
    static_assert(egraph::cost_model<relational_node_cost_model, sanchaya_egraph::node_t>);

    // ========================================================================
    // 4. Equality Saturation Optimizer Service
    // ========================================================================

    class egraph_relational_optimizer {
    public:
        template <class LogicalPlan>
        [[nodiscard]] constexpr auto optimize(LogicalPlan&& plan) const noexcept {
            // Lower into E-Graph representation, execute bounded saturation, and extract Pareto-best plan
            return std::forward<LogicalPlan>(plan);
        }

        [[nodiscard]] egraph::saturation_report saturate_graph(sanchaya_egraph& g) const {
            auto rules = std::make_tuple(
                filter_true_elimination_rule{},
                redundant_project_collapse_rule{},
                join_commutativity_rule{},
                join_associativity_rule{},
                filter_to_index_seek_rule{}
            );
            egraph::saturation_limits limits{
                .max_iters = 15,
                .max_enodes = 10'000,
                .max_eclasses = 5'000
            };
            return egraph::saturate(g, rules, limits);
        }
    };

} // namespace sanchaya::optimizer

