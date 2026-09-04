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
    namespace detail {
        template <class T>
        struct is_relation_descriptor : std::false_type {};

        template <
            akshara::fixed_string N,
            class S, class T, class F, class P, class K, class D
        >
        struct is_relation_descriptor<relation_descriptor<N, S, T, F, P, K, D>> : std::true_type {};

        template <class T>
        inline constexpr bool is_relation_descriptor_v = is_relation_descriptor<std::decay_t<T>>::value;

        template <class Item>
        constexpr auto normalize_model_entity(Item&& item) {
            using Clean = std::decay_t<Item>;
            if constexpr (requires { item.build(); }) {
                return item.build();
            } else {
                return std::forward<Item>(item);
            }
        }

        template <class... Items>
        constexpr auto filter_and_materialize_entities(Items&&... items) {
            return std::tuple_cat([&](auto&& item) {
                using Clean = std::decay_t<decltype(item)>;
                if constexpr (!is_relation_descriptor_v<Clean>) {
                    return std::make_tuple(normalize_model_entity(std::forward<decltype(item)>(item)));
                } else {
                    return std::tuple<>{};
                }
            }(std::forward<Items>(items))...);
        }

        template <class... Items>
        constexpr auto filter_relations(Items&&... items) {
            return std::tuple_cat([&](auto&& item) {
                using Clean = std::decay_t<decltype(item)>;
                if constexpr (is_relation_descriptor_v<Clean>) {
                    return std::make_tuple(std::forward<decltype(item)>(item));
                } else {
                    return std::tuple<>{};
                }
            }(std::forward<Items>(items))...);
        }
    } // namespace detail

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

        // Fluent Builder Extensions (Existing Level 4 descriptor or entity_builder)
        template <class EntityDesc>
        [[nodiscard]] constexpr auto entity(EntityDesc&& entity_desc) && {
            auto normalized = detail::normalize_model_entity(std::forward<EntityDesc>(entity_desc));
            auto new_entities = std::tuple_cat(
                std::move(entities_),
                std::make_tuple(std::move(normalized))
            );
            return model_definition<ModelName, decltype(new_entities), RelationsTuple>(
                std::move(new_entities),
                std::move(relations_)
            );
        }

        // Scoped Fluent Entity Construction: .entity<T>(Descriptors&&...)
        template <class EntityType, class... Descriptors>
            requires(sizeof...(Descriptors) > 0)
        [[nodiscard]] constexpr auto entity(Descriptors&&... descriptors) && {
            auto b = sanchaya::entity<EntityType>(std::forward<Descriptors>(descriptors)...).build();
            auto new_entities = std::tuple_cat(
                std::move(entities_),
                std::make_tuple(std::move(b))
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

        template <class RelDesc>
            requires detail::is_relation_descriptor_v<RelDesc>
        [[nodiscard]] constexpr auto relation(RelDesc&& rel) && {
            auto new_relations = std::tuple_cat(
                std::move(relations_),
                std::make_tuple(std::forward<RelDesc>(rel))
            );
            return model_definition<ModelName, EntitiesTuple, decltype(new_relations)>(
                std::move(entities_),
                std::move(new_relations)
            );
        }

        // Fluent Concise Relationship Builders
        template <
            akshara::fixed_string RelationName,
            auto FkMember,
            auto PkMember,
            class Kind = reference_relation_tag,
            class OnDelete = on_delete_action<restrict_delete>
        >
        [[nodiscard]] constexpr auto many_to_one(Kind&& k = {}, OnDelete&& d = {}) && {
            auto rel = sanchaya::many_to_one<RelationName, FkMember, PkMember>(std::forward<Kind>(k), std::forward<OnDelete>(d));
            return std::move(*this).relation(std::move(rel));
        }

        template <
            akshara::fixed_string RelationName,
            auto PkMember,
            auto FkMember,
            class Kind = reference_relation_tag,
            class OnDelete = on_delete_action<restrict_delete>
        >
        [[nodiscard]] constexpr auto one_to_many(Kind&& k = {}, OnDelete&& d = {}) && {
            auto rel = sanchaya::one_to_many<RelationName, PkMember, FkMember>(std::forward<Kind>(k), std::forward<OnDelete>(d));
            return std::move(*this).relation(std::move(rel));
        }

        template <
            akshara::fixed_string RelationName,
            auto FkMember,
            auto PkMember,
            class Kind = reference_relation_tag,
            class OnDelete = on_delete_action<restrict_delete>
        >
        [[nodiscard]] constexpr auto one_to_one(Kind&& k = {}, OnDelete&& d = {}) && {
            auto rel = sanchaya::one_to_one<RelationName, FkMember, PkMember>(std::forward<Kind>(k), std::forward<OnDelete>(d));
            return std::move(*this).relation(std::move(rel));
        }

        [[nodiscard]] constexpr auto build() const noexcept {
            return *this;
        }

    private:
        EntitiesTuple entities_;
        RelationsTuple relations_;
    };

    // Variadic Model Constructor: model<"name">(entity<T>(...), many_to_one<...>())
    template <akshara::fixed_string ModelName, class... Items>
        requires(sizeof...(Items) > 0)
    [[nodiscard]] constexpr auto model(Items&&... items) {
        auto entities_tuple = detail::filter_and_materialize_entities(std::forward<Items>(items)...);
        auto relations_tuple = detail::filter_relations(std::forward<Items>(items)...);
        return model_definition<ModelName, decltype(entities_tuple), decltype(relations_tuple)>(
            std::move(entities_tuple),
            std::move(relations_tuple)
        );
    }

    // Default 0-argument factory for fluent chaining: model<"name">().entity(...)...
    template <akshara::fixed_string ModelName>
    [[nodiscard]] constexpr auto model() noexcept {
        return model_definition<ModelName>{};
    }

} // namespace sanchaya
