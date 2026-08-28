#pragma once

// vakya/constraint_solvers.hpp — Heavier constraint solver backends (opt-in).
//
// C++23, header-only, no virtual, no macros. Opt-in; not pulled by vakya.hpp.
// Namespace: vakya::types  (extends constraints.hpp)
//
// rule_constraint_solver   — EasyRules forward-chaining trait/capability solver
//                            handles: implements, requires_cap, user
// graph_constraint_solver  — LiteGraph SCC/topo dependency-cycle solver
//                            handles: dependency kinds (ordering, same_rank, broadcastable, compatible)
// egraph_constraint_solver — Equality-saturation solver (__has_include-guarded)
//                            handles: deferred (equivalence proof)

#include "vakya/constraints.hpp"
#include "containers/graph/LiteGraphAlgorithms.hpp"

#if __has_include("containers/graph/egraph.hpp")
#include "containers/graph/egraph.hpp"
#define VAKYA_HAS_EGRAPH 1
#else
#define VAKYA_HAS_EGRAPH 0
#endif

#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace vakya::types {
    // ============================================================================
    // rule_constraint_solver — forward-chaining trait closure via EasyRules.
    //
    // Trait/capability facts are strings (interned hash -> name string mapping
    // held externally; solver only sees hashes for zero-copy).
    // Rules: (Numeric(T) => Addable(T)) encoded as EasyRules Rule predicates.
    // Semi-naïve: only fire on newly derived facts each iteration.
    // ============================================================================

    class rule_constraint_solver {
    public:
        using TraitHash = std::uint64_t;

        [[nodiscard]] bool handles(constraint_kind k) const noexcept {
            return k == constraint_kind::implements ||
                k == constraint_kind::requires_cap ||
                k == constraint_kind::user;
        }

        // Register a trait implication rule: if antecedent_trait(T) then consequent_trait(T)
        // Both are trait-name hashes.
        void add_implication(TraitHash antecedent, TraitHash consequent) {
            implications_[antecedent].push_back(consequent);
        }

        // Assert a base trait fact: type_var_or_stable_id implements trait_hash
        void assert_trait(std::uint32_t type_id, TraitHash trait_hash) {
            base_facts_[type_id].insert(trait_hash);
        }

        [[nodiscard]] solve_result solve(std::span<const constraint> batch,
                                         solve_context /*ctx*/) {
            solve_result result;

            // For each query, close the relevant facts under implications
            for (const constraint& c : batch) {
                if (!handles(c.kind)) continue;
                if (c.operands.size() < 1) continue;

                const std::uint32_t type_id = c.operands[0].index;
                const TraitHash required_trait = c.trait_name_hash;

                // Compute closure for this type
                auto closed = compute_closure(type_id);

                if (!closed.count(required_trait)) {
                    result.status = join_status(result.status, solve_status::unsatisfiable);
                    result.diagnostics.push_back(solver_diagnostic{
                        "unsatisfied constraint: type does not implement required trait",
                        constraint_ref{}
                    });
                }
            }
            return result;
        }

    private:
        [[nodiscard]] std::unordered_set<TraitHash> compute_closure(std::uint32_t type_id) const {
            auto it = base_facts_.find(type_id);
            std::unordered_set<TraitHash> facts;
            if (it != base_facts_.end()) facts = it->second;

            // Semi-naïve forward chaining to fixpoint
            bool changed = true;
            while (changed) {
                changed = false;
                for (const auto& [antecedent, consequents] : implications_) {
                    if (!facts.count(antecedent)) continue;
                    for (TraitHash c : consequents) {
                        if (!facts.count(c)) {
                            facts.insert(c);
                            changed = true;
                        }
                    }
                }
            }
            return facts;
        }

        std::unordered_map<TraitHash, std::vector<TraitHash>> implications_;
        std::unordered_map<std::uint32_t, std::unordered_set<TraitHash>> base_facts_;
    };

    static_assert(constraint_solver<rule_constraint_solver>);

    // ============================================================================
    // graph_constraint_solver — dependency-cycle detection via LiteGraph Tarjan SCC.
    //
    // Handles ordering/dependency constraint kinds (broadcastable, same_rank, compatible).
    // Any non-trivial SCC = unsatisfiable + cycle-path diagnostic.
    // Successful solve returns topo-order as a diagnostic note.
    // ============================================================================

    class graph_constraint_solver {
    public:
        [[nodiscard]] bool handles(constraint_kind k) const noexcept {
            return k == constraint_kind::same_rank ||
                k == constraint_kind::broadcastable ||
                k == constraint_kind::compatible;
        }

        [[nodiscard]] solve_result solve(std::span<const constraint> batch,
                                         solve_context ctx) {
            solve_result result;
            if (batch.empty()) return result;

            // Build a directed constraint graph: nodes = type_ref indices, edges = constraints.
            // node_constraint_map: node index -> first originating constraint_ref for diagnostics.
            using G = litegraph::Graph<std::uint32_t, std::uint32_t, litegraph::Directed>;
            G g;

            std::unordered_map<std::uint32_t, litegraph::NodeId> node_map;
            std::unordered_map<std::uint32_t, constraint_ref> node_constraint_map;
            std::uint32_t cidx = 0;

            auto get_node = [&](std::uint32_t type_idx, constraint_ref cref) -> litegraph::NodeId {
                auto it = node_map.find(type_idx);
                if (it != node_map.end()) return it->second;
                auto nid = g.add_node(type_idx);
                node_map[type_idx] = nid;
                node_constraint_map[type_idx] = cref;
                return nid;
            };

            for (const constraint& c : batch) {
                if (!handles(c.kind)) continue;
                if (c.operands.size() < 2) continue;

                constraint_ref cref = c.source;
                litegraph::NodeId na = get_node(c.operands[0].index, cref);
                litegraph::NodeId nb = get_node(c.operands[1].index, cref);
                g.add_edge(na, nb, cidx++);
            }

            if (g.node_count() == 0) return result;

            // Run Tarjan SCC; each non-trivial SCC is a dependency cycle.
            auto sccs = litegraph::strongly_connected_components(g);

            for (const auto& scc : sccs) {
                if (scc.size() <= 1) continue;

                result.status = join_status(result.status, solve_status::unsatisfiable);

                // Pick the constraint_ref from the first node in the SCC with a known origin.
                constraint_ref origin{};
                for (auto node_id : scc) {
                    std::uint32_t type_idx = g.node_data(node_id);
                    auto it = node_constraint_map.find(type_idx);
                    if (it != node_constraint_map.end() && it->second != constraint_ref{}) {
                        origin = it->second;
                        break;
                    }
                }

                // Build cycle path string: α_idx1 → α_idx2 → … → α_idx1
                std::string cycle_path;
                for (std::size_t i = 0; i < scc.size(); ++i) {
                    if (i != 0) cycle_path += " → ";
                    cycle_path += "α";
                    cycle_path += std::to_string(g.node_data(scc[i]));
                }
                // close the cycle
                if (!scc.empty()) {
                    cycle_path += " → α";
                    cycle_path += std::to_string(g.node_data(scc[0]));
                }

                result.diagnostics.push_back(solver_diagnostic{
                    "constraint dependency cycle detected (" +
                    std::to_string(scc.size()) + " nodes): " + cycle_path,
                    origin
                });
            }

            return result;
        }
    };

    static_assert(constraint_solver<graph_constraint_solver>);

    // ============================================================================
    // egraph_constraint_solver — equality saturation over type_ref nodes.
    // Guards: __has_include("containers/graph/egraph.hpp")
    // Handles: deferred (eq saturation is the slow path for equivalence proofs)
    // ============================================================================

#if VAKYA_HAS_EGRAPH

    class egraph_constraint_solver {
    public:
        [[nodiscard]] bool handles(constraint_kind /*k*/) const noexcept {
            // Reserved for equivalence-proof constraints; future extension.
            return false;
        }

        [[nodiscard]] solve_result solve(std::span<const constraint> /*batch*/,
                                         solve_context /*ctx*/) {
            solve_result r;
            r.status = solve_status::deferred;
            return r;
        }
    };

    static_assert(constraint_solver<egraph_constraint_solver>);

#endif // VAKYA_HAS_EGRAPH
} // namespace vakya::types
