#pragma once

// ============================================================================
// sanchaya/schema/graph_validator.hpp — Relational Graph Validation & Cycle Analysis
// ============================================================================
//
// RTTI-free: entity types are mapped to LiteGraph NodeIds via their 0-based
// position in the Model entity tuple, computed at compile time with
// type_pack_index_v. No <typeindex>, no typeid(), no std::type_index.
// Compiles cleanly with -fno-rtti.
// ============================================================================

#include "sanchaya/fwd.hpp"
#include "sanchaya/schema/descriptors.hpp"
#include "containers/graph/LiteGraph.hpp"
#include "containers/graph/LiteGraphAlgorithms.hpp"
#include <cstddef>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace sanchaya::validation {

    enum class validation_status : std::uint8_t {
        valid,
        embedded_cycle_detected,
        invalid_foreign_key
    };

    struct model_validation_result {
        validation_status status{validation_status::valid};
        bool              has_reference_cycles{false};
        std::size_t       embedded_cycle_count{0};
        std::string_view  message{"Model validated successfully"};
    };

    // ── Compile-time type → pack index ────────────────────────────────────────
    // Returns the 0-based index of type T in the parameter pack Ts..., or
    // sizeof...(Ts) when T is not found.  Evaluated entirely at compile time;
    // requires no RTTI.
    namespace detail {
        template <class T, class... Ts>
        inline constexpr std::size_t type_pack_index_v =
            []<std::size_t... Is>(std::index_sequence<Is...>) constexpr -> std::size_t {
                std::size_t result = sizeof...(Ts); // sentinel: not found
                ((std::is_same_v<T, Ts> ? (result = Is, true) : false) || ...);
                return result;
            }(std::make_index_sequence<sizeof...(Ts)>{});

        // Helper: apply type_pack_index_v against a tuple's type list
        template <class T, class Tuple>
        struct tuple_type_index;

        template <class T, class... Ts>
        struct tuple_type_index<T, std::tuple<Ts...>> {
            // Each Ts here is an entity descriptor; extract entity_type from it
            static constexpr std::size_t value =
                type_pack_index_v<T, typename std::decay_t<Ts>::entity_type...>;
        };

        template <class T, class Tuple>
        inline constexpr std::size_t tuple_type_index_v = tuple_type_index<T, Tuple>::value;
    } // namespace detail

    template <class Model>
    class model_graph_analyzer {
    public:
        static auto analyze(const Model& model) -> model_validation_result {
            model_validation_result result{};

            // Build directed graphs of entity relationships
            litegraph::SimpleGraph ref_graph;
            litegraph::SimpleGraph embed_graph;

            // 1. Register one vertex per entity type — NodeId == entity tuple index.
            //    We use std::apply with an index sequence so NodeIds are assigned
            //    in deterministic order without any runtime map.
            const auto& entities = model.entities();
            std::apply([&](const auto&... entity_descs) {
                (([&](const auto&) {
                    ref_graph.add_node();
                    embed_graph.add_node();
                }(entity_descs)), ...);
            }, entities);

            // Capture the entity descriptor tuple type for index lookup below
            using EntitiesTuple = std::decay_t<decltype(entities)>;

            // 2. Add edges for all declared relationships — NodeId for entity E
            //    is its compile-time index in EntitiesTuple, looked up with
            //    detail::tuple_type_index_v<E, EntitiesTuple>. No runtime map.
            std::apply([&](const auto&... rels) {
                ([&](const auto& rel) {
                    using RelType = std::decay_t<decltype(rel)>;
                    using Src     = typename RelType::source_type;
                    using Tgt     = typename RelType::target_type;
                    using Kind    = typename RelType::relation_kind;

                    constexpr auto src_id =
                        static_cast<litegraph::NodeId>(
                            detail::tuple_type_index_v<Src, EntitiesTuple>);
                    constexpr auto tgt_id =
                        static_cast<litegraph::NodeId>(
                            detail::tuple_type_index_v<Tgt, EntitiesTuple>);

                    ref_graph.add_edge(src_id, tgt_id);
                    if constexpr (std::is_same_v<Kind, embedded_relation_tag>) {
                        embed_graph.add_edge(src_id, tgt_id);
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
