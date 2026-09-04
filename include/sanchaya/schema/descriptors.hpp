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

    // Factory overloads for embedded(...)
    template <akshara::fixed_string Name, auto MemberPtr>
    [[nodiscard]] constexpr auto embedded() noexcept {
        return embedded_descriptor<Name, MemberPtr>{};
    }

    template <akshara::fixed_string Name, auto MemberPtr, akshara::fixed_string StableTag>
    [[nodiscard]] constexpr auto embedded(stable_id_tag<StableTag>) noexcept {
        return embedded_descriptor<Name, MemberPtr, stable_id_tag<StableTag>>{};
    }

    // ========================================================================
    // 3. Entity Row Descriptors
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

} // namespace sanchaya
