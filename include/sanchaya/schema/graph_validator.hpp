#pragma once

// ============================================================================
// sanchaya/schema/graph_validator.hpp — Relational Graph Validation & Cycle Analysis
// ============================================================================

#include "sanchaya/fwd.hpp"
#include "sanchaya/schema/descriptors.hpp"
#include "containers/graph/LiteGraph.hpp"
#include "containers/graph/LiteGraphAlgorithms.hpp"
#include <vector>
#include <tuple>
#include <typeindex>
#include <unordered_map>

namespace sanchaya::validation {

    enum class validation_status : std::uint8_t {
        valid,
        embedded_cycle_detected,
        invalid_foreign_key
    };

    struct model_validation_result {
        validation_status status{validation_status::valid};
        bool has_reference_cycles{false};
        std::size_t embedded_cycle_count{0};
        std::string_view message{"Model validated successfully"};
    };

    template <class Model>
    class model_graph_analyzer {
    public:
        static auto analyze(const Model& model) -> model_validation_result {
            model_validation_result result{};

            // Build directed graph of entity relationships
            litegraph::SimpleGraph ref_graph;
            litegraph::SimpleGraph embed_graph;

            std::unordered_map<std::type_index, litegraph::NodeId> type_to_node;

            // 1. Register vertices for all entities in the model
            std::apply([&](const auto&... entities) {
                ([&](const auto& entity_desc) {
                    using E = typename std::decay_t<decltype(entity_desc)>::entity_type;
                    auto nid = ref_graph.add_node();
                    embed_graph.add_node();
                    type_to_node[std::type_index(typeid(E))] = nid;
                }(entities), ...);
            }, model.entities());

            // 2. Add edges for all declared relationships
            std::apply([&](const auto&... rels) {
                ([&](const auto& rel) {
                    using RelType = std::decay_t<decltype(rel)>;
                    using Src = typename RelType::source_type;
                    using Tgt = typename RelType::target_type;
                    using Kind = typename RelType::relation_kind;

                    auto src_it = type_to_node.find(std::type_index(typeid(Src)));
                    auto tgt_it = type_to_node.find(std::type_index(typeid(Tgt)));

                    if (src_it != type_to_node.end() && tgt_it != type_to_node.end()) {
                        ref_graph.add_edge(src_it->second, tgt_it->second);
                        if constexpr (std::is_same_v<Kind, embedded_relation_tag>) {
                            embed_graph.add_edge(src_it->second, tgt_it->second);
                        }
                    }
                }(rels), ...);
            }, model.relations());

            // 3. Compute SCC on embedded graph via Tarjan's algorithm
            auto embed_sccs = litegraph::strongly_connected_components(embed_graph);
            for (const auto& scc : embed_sccs) {
                if (scc.size() > 1) {
                    result.status = validation_status::embedded_cycle_detected;
                    result.embedded_cycle_count++;
                    result.message = "Cycle detected in embedded hierarchy";
                    return result;
                }
            }

            // 4. Compute SCC on reference graph
            auto ref_sccs = litegraph::strongly_connected_components(ref_graph);
            for (const auto& scc : ref_sccs) {
                if (scc.size() > 1) {
                    result.has_reference_cycles = true;
                }
            }

            return result;
        }
    };

    template <class Model>
    [[nodiscard]] auto validate_model(const Model& model) -> model_validation_result {
        return model_graph_analyzer<Model>::analyze(model);
    }

} // namespace sanchaya::validation
