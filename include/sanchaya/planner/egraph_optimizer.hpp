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

    /// Common Subexpression Elimination: merges equivalent nodes across classes
    struct common_subexpression_elimination_rule {
        template <class G>
        void apply(G& g) const {
            const std::size_t snapshot = g.class_count();
            containers::SmallVector<std::pair<egraph::e_class_id, egraph::e_class_id>, 16> deferred_merges;
            for (egraph::e_class_id cid1 = 0; cid1 < static_cast<egraph::e_class_id>(snapshot); ++cid1) {
                if (!g.is_root(cid1)) continue;
                for (egraph::e_class_id cid2 = cid1 + 1; cid2 < static_cast<egraph::e_class_id>(snapshot); ++cid2) {
                    if (!g.is_root(cid2)) continue;
                    for (const auto& n1 : g.classes()[cid1].nodes) {
                        for (const auto& n2 : g.classes()[cid2].nodes) {
                            if (n1.op == n2.op && n1.payload == n2.payload && n1.children.size() == n2.children.size()) {
                                bool match = true;
                                for (std::size_t i = 0; i < n1.children.size(); ++i) {
                                    if (g.find(n1.children[i]) != g.find(n2.children[i])) {
                                        match = false;
                                        break;
                                    }
                                }
                                if (match) {
                                    deferred_merges.push_back({cid1, cid2});
                                }
                            }
                        }
                    }
                }
            }
            for (const auto& [c1, c2] : deferred_merges) {
                (void)g.merge(c1, c2);
            }
        }
    };

    /// Identity filter elimination: filter(true, X) -> X
    struct filter_true_elimination_rule {
        template <class G>
        void apply(G& g) const {
            const std::size_t snapshot = g.class_count();
            containers::SmallVector<std::pair<egraph::e_class_id, egraph::e_class_id>, 16> deferred_merges;
            for (egraph::e_class_id cid = 0; cid < static_cast<egraph::e_class_id>(snapshot); ++cid) {
                if (!g.is_root(cid)) continue;
                for (const auto& node : g.classes()[cid].nodes) {
                    if (node.op == rel_op::op_filter && node.payload.extra_data == 1) { // 1 = always true
                        if (!node.children.empty()) {
                            deferred_merges.push_back({cid, node.children[0]});
                        }
                    }
                }
            }
            for (const auto& [cid, child] : deferred_merges) {
                (void)g.merge(cid, child);
            }
        }
    };

    /// Redundant project collapse: project(project(X)) -> project(X)
    struct redundant_project_collapse_rule {
        template <class G>
        void apply(G& g) const {
            const std::size_t snapshot = g.class_count();
            containers::SmallVector<std::pair<egraph::e_class_id, typename G::node_t>, 16> deferred_adds;
            for (egraph::e_class_id cid = 0; cid < static_cast<egraph::e_class_id>(snapshot); ++cid) {
                if (!g.is_root(cid)) continue;
                for (const auto& node : g.classes()[cid].nodes) {
                    if (node.op == rel_op::op_project && !node.children.empty()) {
                        const auto child_cid = g.find(node.children[0]);
                        for (const auto& child_node : g.classes()[child_cid].nodes) {
                            if (child_node.op == rel_op::op_project && !child_node.children.empty()) {
                                typename G::node_t collapsed_node;
                                collapsed_node.op = rel_op::op_project;
                                collapsed_node.children.push_back(child_node.children[0]);
                                collapsed_node.payload = node.payload;
                                deferred_adds.push_back({cid, std::move(collapsed_node)});
                            }
                        }
                    }
                }
            }
            for (auto& [cid, node] : deferred_adds) {
                const egraph::e_class_id collapsed_id = g.add(std::move(node));
                (void)g.merge(cid, collapsed_id);
            }
        }
    };

    /// Join Commutativity: join(A, B) -> join(B, A)
    struct join_commutativity_rule {
        template <class G>
        void apply(G& g) const {
            const std::size_t snapshot = g.class_count();
            containers::SmallVector<std::pair<egraph::e_class_id, typename G::node_t>, 16> deferred_adds;
            for (egraph::e_class_id cid = 0; cid < static_cast<egraph::e_class_id>(snapshot); ++cid) {
                if (!g.is_root(cid)) continue;
                for (const auto& node : g.classes()[cid].nodes) {
                    if (node.op == rel_op::op_join && node.children.size() == 2) {
                        typename G::node_t swapped;
                        swapped.op = rel_op::op_join;
                        swapped.children.push_back(node.children[1]);
                        swapped.children.push_back(node.children[0]);
                        swapped.payload = node.payload;
                        deferred_adds.push_back({cid, std::move(swapped)});
                    }
                }
            }
            for (auto& [cid, node] : deferred_adds) {
                const egraph::e_class_id sid = g.add(std::move(node));
                (void)g.merge(cid, sid);
            }
        }
    };

    /// Join Associativity: join(join(A, B), C) -> join(A, join(B, C))
    struct join_associativity_rule {
        template <class G>
        void apply(G& g) const {
            const std::size_t snapshot = g.class_count();
            containers::SmallVector<std::tuple<egraph::e_class_id, egraph::e_class_id, egraph::e_class_id, egraph::e_class_id, rel_payload>, 16> matches;
            for (egraph::e_class_id cid = 0; cid < static_cast<egraph::e_class_id>(snapshot); ++cid) {
                if (!g.is_root(cid)) continue;
                // Snapshot outer nodes
                containers::SmallVector<typename G::node_t, 8> outer_nodes(g.classes()[cid].nodes.begin(), g.classes()[cid].nodes.end());
                for (const auto& outer : outer_nodes) {
                    if (outer.op == rel_op::op_join && outer.children.size() == 2) {
                        const egraph::e_class_id left = g.find(outer.children[0]);
                        const egraph::e_class_id c = g.find(outer.children[1]);
                        // Snapshot inner nodes
                        containers::SmallVector<typename G::node_t, 8> inner_nodes(g.classes()[left].nodes.begin(), g.classes()[left].nodes.end());
                        for (const auto& inner : inner_nodes) {
                            if (inner.op == rel_op::op_join && inner.children.size() == 2) {
                                const egraph::e_class_id a = g.find(inner.children[0]);
                                const egraph::e_class_id b = g.find(inner.children[1]);
                                matches.push_back({cid, a, b, c, outer.payload});
                            }
                        }
                    }
                }
            }
            for (const auto& [cid, a, b, c, payload] : matches) {
                typename G::node_t right_inner;
                right_inner.op = rel_op::op_join;
                right_inner.children.push_back(b);
                right_inner.children.push_back(c);
                right_inner.payload = payload;
                const egraph::e_class_id right_inner_id = g.add(std::move(right_inner));

                typename G::node_t new_outer;
                new_outer.op = rel_op::op_join;
                new_outer.children.push_back(a);
                new_outer.children.push_back(right_inner_id);
                new_outer.payload = payload;
                const egraph::e_class_id new_outer_id = g.add(std::move(new_outer));

                (void)g.merge(cid, new_outer_id);
            }
        }
    };

    /// Filter to Index Seek Selection: filter(source(T), key_eq) -> key_lookup(T)
    struct filter_to_index_seek_rule {
        template <class G>
        void apply(G& g) const {
            const std::size_t snapshot = g.class_count();
            containers::SmallVector<std::pair<egraph::e_class_id, typename G::node_t>, 16> deferred_adds;
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
                                deferred_adds.push_back({cid, std::move(seek_node)});
                            }
                        }
                    }
                }
            }
            for (auto& [cid, node] : deferred_adds) {
                const egraph::e_class_id seek_id = g.add(std::move(node));
                (void)g.merge(cid, seek_id);
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

    static_assert(egraph::egraph_rule<common_subexpression_elimination_rule, sanchaya_egraph>);
    static_assert(egraph::egraph_rule<filter_true_elimination_rule, sanchaya_egraph>);
    static_assert(egraph::egraph_rule<redundant_project_collapse_rule, sanchaya_egraph>);
    static_assert(egraph::egraph_rule<join_commutativity_rule, sanchaya_egraph>);
    static_assert(egraph::egraph_rule<join_associativity_rule, sanchaya_egraph>);
    static_assert(egraph::egraph_rule<filter_to_index_seek_rule, sanchaya_egraph>);
    static_assert(egraph::cost_model<relational_node_cost_model, sanchaya_egraph::node_t>);

    // ========================================================================
    // 4. Equality Saturation Optimizer Service
    // ========================================================================

    namespace detail {
        template <class Plan>
        egraph::e_class_id lower_to_egraph(sanchaya_egraph& g, const Plan& plan) {
            using P = std::decay_t<Plan>;
            sanchaya_egraph::node_t node;
            if constexpr (requires { typename P::entity_type; } && !requires { plan.input; }) {
                // Base source / key lookup
                if constexpr (requires { plan.key; }) {
                    node.op = rel_op::op_key_lookup;
                    node.payload = rel_payload{.signature = 2, .extra_data = 0};
                } else {
                    node.op = rel_op::op_source;
                    node.payload = rel_payload{.signature = 1, .extra_data = 0};
                }
            } else if constexpr (requires { plan.input; }) {
                auto child_cid = lower_to_egraph(g, plan.input);
                node.children.push_back(child_cid);

                if constexpr (requires { plan.predicate; } && !requires { plan.left; }) {
                    node.op = rel_op::op_filter;
                    node.payload = rel_payload{.signature = 3 ^ (static_cast<std::uint64_t>(node.children[0]) << 16), .extra_data = 0};
                } else if constexpr (requires { plan.expressions; }) {
                    node.op = rel_op::op_project;
                    node.payload = rel_payload{.signature = 4 ^ (static_cast<std::uint64_t>(std::tuple_size_v<decltype(plan.expressions)>) << 32), .extra_data = 0};
                } else if constexpr (requires { plan.direction; }) {
                    node.op = rel_op::op_order;
                    node.payload = rel_payload{.signature = 5, .extra_data = static_cast<std::uint32_t>(plan.direction)};
                } else if constexpr (requires { plan.limit_count; }) {
                    node.op = rel_op::op_limit;
                    node.payload = rel_payload{.signature = 6, .extra_data = static_cast<std::uint32_t>(plan.limit_count)};
                } else if constexpr (requires { plan.group_keys; }) {
                    node.op = rel_op::op_group;
                    node.payload = rel_payload{.signature = 7 ^ (static_cast<std::uint64_t>(std::tuple_size_v<decltype(plan.group_keys)>) << 32), .extra_data = 0};
                } else if constexpr (requires { plan.aggregates; }) {
                    node.op = rel_op::op_aggregate;
                    node.payload = rel_payload{.signature = 8 ^ (static_cast<std::uint64_t>(std::tuple_size_v<decltype(plan.aggregates)>) << 32), .extra_data = 0};
                } else {
                    node.op = rel_op::op_traverse;
                    node.payload = rel_payload{.signature = 9, .extra_data = 0};
                }
            } else if constexpr (requires { plan.left; plan.right; }) {
                auto left_cid = lower_to_egraph(g, plan.left);
                auto right_cid = lower_to_egraph(g, plan.right);
                node.children.push_back(left_cid);
                node.children.push_back(right_cid);
                node.op = rel_op::op_join;
                node.payload = rel_payload{.signature = 10, .extra_data = static_cast<std::uint32_t>(P::kind)};
            } else {
                node.op = rel_op::op_source;
                node.payload = rel_payload{.signature = 1, .extra_data = 0};
            }
            return g.add(std::move(node));
        }
    } // namespace detail

    template <class Plan>
    struct optimization_result {
        Plan plan;
        egraph::saturation_report report;
    };

    class egraph_relational_optimizer {
    public:
        template <class LogicalPlan>
        [[nodiscard]] auto optimize(LogicalPlan&& plan) const {
            sanchaya_egraph graph;
            (void)detail::lower_to_egraph(graph, plan);

            auto report = saturate_graph(graph);
            return optimization_result<std::decay_t<LogicalPlan>>{
                .plan = std::forward<LogicalPlan>(plan),
                .report = report
            };
        }

        [[nodiscard]] egraph::saturation_report saturate_graph(sanchaya_egraph& g) const {
            auto rules = std::make_tuple(
                common_subexpression_elimination_rule{},
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

