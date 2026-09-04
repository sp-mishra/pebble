#pragma once

// ============================================================================
// sanchaya/schema/descriptors.hpp — Compile-Time Entity, Field & Relation Descriptors
// ============================================================================

#include "sanchaya/fwd.hpp"
#include <concepts>
#include <tuple>
#include <type_traits>

namespace sanchaya {

    // ========================================================================
    // 1. Identity Allocators & Concepts
    // ========================================================================
    namespace concepts {
        template <class Allocator, class KeyType>
        concept identity_allocator_for = requires(Allocator& alloc) {
            { alloc.allocate() } -> std::same_as<KeyType>;
        };
    } // namespace concepts

    struct null_identity_allocator {};

    template <class AllocatorType>
    struct identity_allocator_descriptor {
        using type = AllocatorType;
    };

    template <class AllocatorType>
    [[nodiscard]] constexpr auto allocator() noexcept {
        return identity_allocator_descriptor<AllocatorType>{};
    }

    template <akshara::fixed_string Tag>
    struct stable_id_tag {
        static constexpr auto name = Tag;
    };

    template <akshara::fixed_string Tag>
    [[nodiscard]] constexpr auto stable_id() noexcept {
        return stable_id_tag<Tag>{};
    }

    // ========================================================================
    // 2. Field & Embedded Descriptors
    // ========================================================================
    template <
        akshara::fixed_string FieldName,
        auto MemberPtr,
        class StableId = void,
        class Allocator = null_identity_allocator
    >
    struct field_descriptor {
        static constexpr auto name = FieldName;
        static constexpr auto member = MemberPtr;
        using owner_type = meta::member_owner_t<MemberPtr>;
        using value_type = meta::member_value_t<MemberPtr>;
        using stable_id_type = StableId;
        using allocator_type = Allocator;
        static constexpr bool is_embedded = false;
    };

    template <
        akshara::fixed_string FieldName,
        auto MemberPtr,
        class StableId = void
    >
    struct embedded_descriptor {
        static constexpr auto name = FieldName;
        static constexpr auto member = MemberPtr;
        using owner_type = meta::member_owner_t<MemberPtr>;
        using value_type = meta::member_value_t<MemberPtr>;
        using stable_id_type = StableId;
        static constexpr bool is_embedded = true;
    };

    // Factory overloads for field(...)
    template <akshara::fixed_string Name, auto MemberPtr>
    [[nodiscard]] constexpr auto field() noexcept {
        return field_descriptor<Name, MemberPtr>{};
    }

    template <akshara::fixed_string Name, auto MemberPtr, akshara::fixed_string StableTag>
    [[nodiscard]] constexpr auto field(stable_id_tag<StableTag>) noexcept {
        return field_descriptor<Name, MemberPtr, stable_id_tag<StableTag>>{};
    }

    template <akshara::fixed_string Name, auto MemberPtr, akshara::fixed_string StableTag, class Allocator>
    [[nodiscard]] constexpr auto field(stable_id_tag<StableTag>, identity_allocator_descriptor<Allocator>) noexcept {
        return field_descriptor<Name, MemberPtr, stable_id_tag<StableTag>, Allocator>{};
    }

    template <akshara::fixed_string Name, auto MemberPtr, class Allocator>
    [[nodiscard]] constexpr auto field(identity_allocator_descriptor<Allocator>) noexcept {
        return field_descriptor<Name, MemberPtr, void, Allocator>{};
    }

    // Single-NTTP overloads for field<&T::member>() with compile-time inferred name
    template <auto MemberPtr>
    [[nodiscard]] constexpr auto field() noexcept {
        constexpr std::string_view name_view = meta::member_name<MemberPtr>();
        constexpr auto fixed_name = akshara::make_fixed_string<name_view.size()>(name_view);
        return field_descriptor<fixed_name, MemberPtr>{};
    }

    template <auto MemberPtr, akshara::fixed_string StableTag>
    [[nodiscard]] constexpr auto field(stable_id_tag<StableTag>) noexcept {
        constexpr std::string_view name_view = meta::member_name<MemberPtr>();
        constexpr auto fixed_name = akshara::make_fixed_string<name_view.size()>(name_view);
        return field_descriptor<fixed_name, MemberPtr, stable_id_tag<StableTag>>{};
    }

    template <auto MemberPtr, akshara::fixed_string StableTag, class Allocator>
    [[nodiscard]] constexpr auto field(stable_id_tag<StableTag>, identity_allocator_descriptor<Allocator>) noexcept {
        constexpr std::string_view name_view = meta::member_name<MemberPtr>();
        constexpr auto fixed_name = akshara::make_fixed_string<name_view.size()>(name_view);
        return field_descriptor<fixed_name, MemberPtr, stable_id_tag<StableTag>, Allocator>{};
    }

    // Factory overloads for embedded(...)
    template <akshara::fixed_string Name, auto MemberPtr>
    [[nodiscard]] constexpr auto embedded() noexcept {
        return embedded_descriptor<Name, MemberPtr>{};
    }

    template <akshara::fixed_string Name, auto MemberPtr, akshara::fixed_string StableTag>
    [[nodiscard]] constexpr auto embedded(stable_id_tag<StableTag>) noexcept {
        return embedded_descriptor<Name, MemberPtr, stable_id_tag<StableTag>>{};
    }

    template <auto MemberPtr>
    [[nodiscard]] constexpr auto embedded() noexcept {
        constexpr std::string_view name_view = meta::member_name<MemberPtr>();
        constexpr auto fixed_name = akshara::make_fixed_string<name_view.size()>(name_view);
        return embedded_descriptor<fixed_name, MemberPtr>{};
    }

    template <auto MemberPtr, akshara::fixed_string StableTag>
    [[nodiscard]] constexpr auto embedded(stable_id_tag<StableTag>) noexcept {
        constexpr std::string_view name_view = meta::member_name<MemberPtr>();
        constexpr auto fixed_name = akshara::make_fixed_string<name_view.size()>(name_view);
        return embedded_descriptor<fixed_name, MemberPtr, stable_id_tag<StableTag>>{};
    }

    // ========================================================================
    // 2b. Key & Ignore Modifiers for Progressive Disclosure
    // ========================================================================
    template <auto MemberPtr, class StableId = void, class Allocator = null_identity_allocator>
    struct key_descriptor {
        static constexpr auto member = MemberPtr;
        using owner_type = meta::member_owner_t<MemberPtr>;
        using value_type = meta::member_value_t<MemberPtr>;
        static constexpr std::string_view name_view = meta::member_name<MemberPtr>();
        static constexpr auto name = akshara::make_fixed_string<name_view.size()>(name_view);
        using stable_id_type = StableId;
        using allocator_type = Allocator;
        static constexpr bool is_key = true;
        static constexpr bool is_embedded = false;
        static constexpr bool is_ignored = false;

        [[nodiscard]] constexpr auto to_field() const noexcept {
            return field_descriptor<name, MemberPtr, StableId, Allocator>{};
        }
    };

    template <auto MemberPtr>
    [[nodiscard]] constexpr auto key() noexcept {
        return key_descriptor<MemberPtr>{};
    }

    template <auto MemberPtr, akshara::fixed_string StableTag>
    [[nodiscard]] constexpr auto key(stable_id_tag<StableTag>) noexcept {
        return key_descriptor<MemberPtr, stable_id_tag<StableTag>>{};
    }

    template <auto MemberPtr, akshara::fixed_string StableTag, class Allocator>
    [[nodiscard]] constexpr auto key(stable_id_tag<StableTag>, identity_allocator_descriptor<Allocator>) noexcept {
        return key_descriptor<MemberPtr, stable_id_tag<StableTag>, Allocator>{};
    }

    template <auto MemberPtr>
    struct ignore_descriptor {
        static constexpr auto member = MemberPtr;
        using owner_type = meta::member_owner_t<MemberPtr>;
        static constexpr bool is_ignored = true;
    };

    template <auto MemberPtr>
    [[nodiscard]] constexpr auto ignore() noexcept {
        return ignore_descriptor<MemberPtr>{};
    }

    // ========================================================================
    // 3. Entity Row Descriptors & Fluent Entity Builder
    // ========================================================================
    template <class Entity, class... Fields>
    struct entity_descriptor {
        using entity_type = Entity;
        using fields_tuple = std::tuple<Fields...>;
        static constexpr std::size_t field_count = sizeof...(Fields);
        fields_tuple fields{};

        constexpr explicit entity_descriptor(Fields... f) : fields(std::move(f)...) {}
    };

    template <class Entity, class... Fields>
    [[nodiscard]] constexpr auto describe_row(Fields&&... fields) {
        return entity_descriptor<Entity, std::decay_t<Fields>...>(std::forward<Fields>(fields)...);
    }

    // Entity normalization helpers
    namespace detail {
        template <class T>
        struct is_entity_descriptor : std::false_type {};

        template <class E, class... Fs>
        struct is_entity_descriptor<entity_descriptor<E, Fs...>> : std::true_type {};

        template <class T>
        inline constexpr bool is_entity_descriptor_v = is_entity_descriptor<std::decay_t<T>>::value;

        template <class Desc>
        constexpr auto normalize_entity_field(Desc&& d) {
            if constexpr (requires { d.to_field(); }) {
                return d.to_field();
            } else {
                return std::forward<Desc>(d);
            }
        }
    } // namespace detail

    template <class Entity, class... Descriptors>
    struct entity_builder {
        using entity_type = Entity;
        using descriptors_tuple = std::tuple<Descriptors...>;
        descriptors_tuple descriptors{};

        constexpr explicit entity_builder(Descriptors... d) : descriptors(std::move(d)...) {}

        // Materialize to canonical entity_descriptor
        [[nodiscard]] constexpr auto build() const {
            return std::apply([](const auto&... ds) {
                return entity_descriptor<Entity, decltype(detail::normalize_entity_field(ds))...>(
                    detail::normalize_entity_field(ds)...
                );
            }, descriptors);
        }

        // Fluent modifier chaining
        template <auto MemberPtr, class... Args>
        [[nodiscard]] constexpr auto key(Args&&... args) && {
            auto k = sanchaya::key<MemberPtr>(std::forward<Args>(args)...);
            auto new_tuple = std::tuple_cat(std::move(descriptors), std::make_tuple(std::move(k)));
            return entity_builder<Entity, Descriptors..., decltype(k)>{std::move(new_tuple)};
        }

        template <auto MemberPtr, class... Args>
        [[nodiscard]] constexpr auto field(Args&&... args) && {
            auto f = sanchaya::field<MemberPtr>(std::forward<Args>(args)...);
            auto new_tuple = std::tuple_cat(std::move(descriptors), std::make_tuple(std::move(f)));
            return entity_builder<Entity, Descriptors..., decltype(f)>{std::move(new_tuple)};
        }

        template <auto MemberPtr>
        [[nodiscard]] constexpr auto ignore() && {
            auto ig = sanchaya::ignore<MemberPtr>();
            auto new_tuple = std::tuple_cat(std::move(descriptors), std::make_tuple(std::move(ig)));
            return entity_builder<Entity, Descriptors..., decltype(ig)>{std::move(new_tuple)};
        }

        template <auto MemberPtr, class... Args>
        [[nodiscard]] constexpr auto embedded(Args&&... args) && {
            auto em = sanchaya::embedded<MemberPtr>(std::forward<Args>(args)...);
            auto new_tuple = std::tuple_cat(std::move(descriptors), std::make_tuple(std::move(em)));
            return entity_builder<Entity, Descriptors..., decltype(em)>{std::move(new_tuple)};
        }
    };

    template <class Entity, class... Descriptors>
    [[nodiscard]] constexpr auto entity(Descriptors&&... descriptors) {
        return entity_builder<Entity, std::decay_t<Descriptors>...>(std::forward<Descriptors>(descriptors)...);
    }

    // ========================================================================
    // 4. Relationships & Cardinality Tags
    // ========================================================================
    template <class T> struct many_cardinality { using entity_type = T; };
    template <class T> struct one_cardinality  { using entity_type = T; };

    template <class T> [[nodiscard]] constexpr auto many() noexcept { return many_cardinality<T>{}; }
    template <class T> [[nodiscard]] constexpr auto one() noexcept  { return one_cardinality<T>{}; }

    template <auto FkMember>
    struct foreign_key_descriptor {
        static constexpr auto member = FkMember;
        using owner_type = meta::member_owner_t<FkMember>;
        using value_type = meta::member_value_t<FkMember>;
    };

    template <auto PkMember>
    struct principal_key_descriptor {
        static constexpr auto member = PkMember;
        using owner_type = meta::member_owner_t<PkMember>;
        using value_type = meta::member_value_t<PkMember>;
    };

    template <auto FkMember>
    [[nodiscard]] constexpr auto foreign_key() noexcept {
        return foreign_key_descriptor<FkMember>{};
    }

    template <auto PkMember>
    [[nodiscard]] constexpr auto principal_key() noexcept {
        return principal_key_descriptor<PkMember>{};
    }

    template <auto FkMember, auto PkMember>
    struct by_keys_descriptor {
        static constexpr auto foreign_key = FkMember;
        static constexpr auto principal_key = PkMember;
        using source_type = meta::member_owner_t<FkMember>;
        using target_type = meta::member_owner_t<PkMember>;
    };

    template <auto FkMember, auto PkMember>
    [[nodiscard]] constexpr auto by() noexcept {
        return by_keys_descriptor<FkMember, PkMember>{};
    }

    struct reference_relation_tag {};
    struct embedded_relation_tag  {};

    [[nodiscard]] constexpr auto reference() noexcept { return reference_relation_tag{}; }
    [[nodiscard]] constexpr auto embedded_relation() noexcept { return embedded_relation_tag{}; }

    struct cascade_delete {};
    struct restrict_delete {};
    struct set_null_delete {};

    template <class Action>
    struct on_delete_action {
        using type = Action;
    };

    template <class Action>
    [[nodiscard]] constexpr auto on_delete() noexcept {
        return on_delete_action<Action>{};
    }

    template <
        akshara::fixed_string RelationName,
        class SourceCardinality,
        class TargetCardinality,
        class ForeignKey,
        class PrincipalKey,
        class Kind = reference_relation_tag,
        class OnDelete = on_delete_action<restrict_delete>
    >
    struct relation_descriptor {
        static constexpr auto name = RelationName;
        using source_type = typename SourceCardinality::entity_type;
        using target_type = typename TargetCardinality::entity_type;
        using source_cardinality = SourceCardinality;
        using target_cardinality = TargetCardinality;
        using foreign_key_type = ForeignKey;
        using principal_key_type = PrincipalKey;
        using relation_kind = Kind;
        using delete_action = OnDelete;
    };

    // Concise Relation Factories with Automatic Endpoint Deduction
    template <
        akshara::fixed_string RelationName,
        auto FkMember,
        auto PkMember,
        class Kind = reference_relation_tag,
        class OnDelete = on_delete_action<restrict_delete>
    >
    [[nodiscard]] constexpr auto many_to_one(Kind = {}, OnDelete = {}) noexcept {
        using SourceType = meta::member_owner_t<FkMember>;
        using TargetType = meta::member_owner_t<PkMember>;
        return relation_descriptor<
            RelationName,
            many_cardinality<SourceType>,
            one_cardinality<TargetType>,
            foreign_key_descriptor<FkMember>,
            principal_key_descriptor<PkMember>,
            Kind,
            OnDelete
        >{};
    }

    template <
        akshara::fixed_string RelationName,
        auto PkMember,
        auto FkMember,
        class Kind = reference_relation_tag,
        class OnDelete = on_delete_action<restrict_delete>
    >
    [[nodiscard]] constexpr auto one_to_many(Kind = {}, OnDelete = {}) noexcept {
        using SourceType = meta::member_owner_t<PkMember>;
        using TargetType = meta::member_owner_t<FkMember>;
        return relation_descriptor<
            RelationName,
            one_cardinality<SourceType>,
            many_cardinality<TargetType>,
            foreign_key_descriptor<FkMember>,
            principal_key_descriptor<PkMember>,
            Kind,
            OnDelete
        >{};
    }

    template <
        akshara::fixed_string RelationName,
        auto FkMember,
        auto PkMember,
        class Kind = reference_relation_tag,
        class OnDelete = on_delete_action<restrict_delete>
    >
    [[nodiscard]] constexpr auto one_to_one(Kind = {}, OnDelete = {}) noexcept {
        using SourceType = meta::member_owner_t<FkMember>;
        using TargetType = meta::member_owner_t<PkMember>;
        return relation_descriptor<
            RelationName,
            one_cardinality<SourceType>,
            one_cardinality<TargetType>,
            foreign_key_descriptor<FkMember>,
            principal_key_descriptor<PkMember>,
            Kind,
            OnDelete
        >{};
    }

    // Shorthand relation<"name">(many<T>(), one<U>(), by<&T::fk, &U::pk>(), ...)
    template <
        akshara::fixed_string RelationName,
        class SourceCard,
        class TargetCard,
        auto FkMember,
        auto PkMember,
        class Kind = reference_relation_tag,
        class OnDelete = on_delete_action<restrict_delete>
    >
    [[nodiscard]] constexpr auto relation(
        SourceCard, TargetCard, by_keys_descriptor<FkMember, PkMember>,
        Kind = {}, OnDelete = {}
    ) noexcept {
        return relation_descriptor<
            RelationName,
            SourceCard,
            TargetCard,
            foreign_key_descriptor<FkMember>,
            principal_key_descriptor<PkMember>,
            Kind,
            OnDelete
        >{};
    }

    // Untagged concise relation builders for relation<"name">(many_to_one<&T::fk, &U::pk>())
    template <
        auto FkMember,
        auto PkMember,
        class Kind = reference_relation_tag,
        class OnDelete = on_delete_action<restrict_delete>
    >
    [[nodiscard]] constexpr auto many_to_one(Kind = {}, OnDelete = {}) noexcept {
        return many_to_one<"unnamed", FkMember, PkMember, Kind, OnDelete>();
    }

    template <
        auto PkMember,
        auto FkMember,
        class Kind = reference_relation_tag,
        class OnDelete = on_delete_action<restrict_delete>
    >
    [[nodiscard]] constexpr auto one_to_many(Kind = {}, OnDelete = {}) noexcept {
        return one_to_many<"unnamed", PkMember, FkMember, Kind, OnDelete>();
    }

    template <
        auto FkMember,
        auto PkMember,
        class Kind = reference_relation_tag,
        class OnDelete = on_delete_action<restrict_delete>
    >
    [[nodiscard]] constexpr auto one_to_one(Kind = {}, OnDelete = {}) noexcept {
        return one_to_one<"unnamed", FkMember, PkMember, Kind, OnDelete>();
    }

    // Shorthand relation<"name">(many_to_one<&T::fk, &U::pk>(), ...)
    template <
        akshara::fixed_string RelationName,
        akshara::fixed_string Tag,
        auto FkMember,
        auto PkMember,
        class Kind = reference_relation_tag,
        class OnDelete = on_delete_action<restrict_delete>
    >
    [[nodiscard]] constexpr auto relation(
        relation_descriptor<Tag, many_cardinality<meta::member_owner_t<FkMember>>, one_cardinality<meta::member_owner_t<PkMember>>, foreign_key_descriptor<FkMember>, principal_key_descriptor<PkMember>, Kind, OnDelete>
    ) noexcept {
        return many_to_one<RelationName, FkMember, PkMember, Kind, OnDelete>();
    }

} // namespace sanchaya
