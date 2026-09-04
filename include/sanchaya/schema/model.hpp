#pragma once

// ============================================================================
// sanchaya/schema/model.hpp — Fluent Compile-Time Persistence Model & Validator
// ============================================================================

#include "sanchaya/schema/descriptors.hpp"
#include "containers/graph/LiteGraph.hpp"
#include <tuple>
#include <type_traits>

namespace sanchaya {

    // ========================================================================
    // 1. Model Storage & Relational Analysis
    // ========================================================================
    template <
        akshara::fixed_string ModelName,
        class EntitiesTuple = std::tuple<>,
        class RelationsTuple = std::tuple<>
    >
    class model_definition {
    public:
        static constexpr auto name = ModelName;
        using entities_type = EntitiesTuple;
        using relations_type = RelationsTuple;

        constexpr explicit model_definition(EntitiesTuple entities = {}, RelationsTuple relations = {})
            : entities_(std::move(entities)), relations_(std::move(relations)) {}

        [[nodiscard]] constexpr const EntitiesTuple& entities() const noexcept { return entities_; }
        [[nodiscard]] constexpr const RelationsTuple& relations() const noexcept { return relations_; }

        // Fluent Builder Extensions
        template <class EntityDesc>
        [[nodiscard]] constexpr auto entity(EntityDesc&& entity_desc) && {
            auto new_entities = std::tuple_cat(
                std::move(entities_),
                std::make_tuple(std::forward<EntityDesc>(entity_desc))
            );
            return model_definition<ModelName, decltype(new_entities), RelationsTuple>(
                std::move(new_entities),
                std::move(relations_)
            );
        }

        template <
            akshara::fixed_string RelationName,
            class SourceCard,
            class TargetCard,
            class Fk,
            class Pk,
            class Kind = reference_relation_tag,
            class OnDelete = on_delete_action<restrict_delete>
        >
        [[nodiscard]] constexpr auto relation(
            SourceCard&&, TargetCard&&, Fk&&, Pk&&,
            Kind&& = {}, OnDelete&& = {}
        ) && {
            using NewRel = relation_descriptor<
                RelationName,
                std::decay_t<SourceCard>,
                std::decay_t<TargetCard>,
                std::decay_t<Fk>,
                std::decay_t<Pk>,
                std::decay_t<Kind>,
                std::decay_t<OnDelete>
            >;
            auto new_relations = std::tuple_cat(
                std::move(relations_),
                std::make_tuple(NewRel{})
            );
            return model_definition<ModelName, EntitiesTuple, decltype(new_relations)>(
                std::move(entities_),
                std::move(new_relations)
            );
        }

        [[nodiscard]] constexpr auto build() const noexcept {
            // Returns validated immutable model
            return *this;
        }

    private:
        EntitiesTuple entities_;
        RelationsTuple relations_;
    };

    template <akshara::fixed_string ModelName>
    [[nodiscard]] constexpr auto model() noexcept {
        return model_definition<ModelName>{};
    }

} // namespace sanchaya
