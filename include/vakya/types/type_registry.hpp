#pragma once

// =============================================================================
// vakya/types/type_registry.hpp — runtime type-descriptor registry (opt-in)
//
// C++23, header-only, no virtual, no macros.
// Namespace: vakya::types
//
// Adds a runtime metadata layer over the compile-time type_descriptor<Ctor>
// seam so types can be enumerated, named, and looked up dynamically.
// Arena interning stays the hot path; the registry is metadata-only.
//
// type_registry_entry = {
//   type_descriptor snapshot  (stable_id, arity, symbol)
//   type_kind                 (primitive | tensor | effect | capability | language)
//   capability_mask           (optional — see capability.hpp)
//   effect_mask               (optional — see effect.hpp)
// }
//
// Backed by containers::descriptor_registry<type_registry_entry>.
// Populated opt-in: register_type_descriptor<Ctor>() publishes metadata;
// arena interning never consults the registry (zero-cost when unused).
//
// Dependencies: vakya/types.hpp, containers/descriptor_registry.hpp
// =============================================================================

#include "vakya/types.hpp"
#include "containers/descriptor_registry.hpp"

#include <cstdint>
#include <string_view>

namespace vakya::types {
    // ============================================================================
    // type_registry_category — category enum for type_registry entries
    // ============================================================================

    enum class type_registry_category : std::uint32_t {
        primitive = 0,
        tensor = 1,
        effect = 2,
        capability = 3,
        language = 4,
    };

    // ============================================================================
    // type_registry_entry — the registered descriptor (satisfies RegistrableDescriptor)
    // ============================================================================

    struct type_registry_entry {
        // RegistrableDescriptor fields
        std::uint32_t stable_id = 0;
        std::uint64_t name_hash = 0;
        type_registry_category category = type_registry_category::primitive;

        // Snapshot of compile-time metadata
        std::string_view symbol{};
        std::uint8_t arity = 0;

        // Runtime classification
        type_kind kind = type_kind::primitive;

        // Bitmasks for capability and effect (populated via capability/effect headers)
        std::uint64_t capability_mask = 0;
        std::uint64_t effect_mask = 0;
    };

    static_assert(containers::RegistrableDescriptor<type_registry_entry>);

    // ============================================================================
    // type_registry — descriptor_registry specialisation for type entries
    // ============================================================================

    using type_registry = containers::descriptor_registry<type_registry_entry>;

    // ============================================================================
    // register_type_descriptor<Ctor> — publish compile-time metadata at runtime
    // ============================================================================

    template <class Ctor>
    void register_type_descriptor(type_registry& reg,
                                  type_registry_category cat = type_registry_category::primitive,
                                  type_kind kind = type_kind::primitive) {
        using D = type_descriptor<Ctor>;
        type_registry_entry entry;
        entry.stable_id = D::stable_id;
        entry.name_hash = containers::desc_name_hash(D::symbol);
        entry.category = cat;
        entry.symbol = D::symbol;
        entry.arity = D::arity;
        entry.kind = kind;
        reg.register_desc(entry);
    }

    // ============================================================================
    // make_builtin_type_registry — pre-loads all built-in type descriptors
    // ============================================================================

    [[nodiscard]] inline type_registry make_builtin_type_registry() {
        type_registry reg;

        // Primitives
        register_type_descriptor<integer_type_tag>(reg);
        register_type_descriptor<float_type_tag>(reg);
        register_type_descriptor<bool_type_tag>(reg);
        register_type_descriptor<char_type_tag>(reg);
        register_type_descriptor<string_type_tag>(reg);
        register_type_descriptor<void_type_tag>(reg);
        register_type_descriptor<dynamic_type_tag>(reg);

        // Composite / constructed types
        register_type_descriptor<array_type_tag>(
            reg, type_registry_category::language, type_kind::constructor);
        register_type_descriptor<vector_type_tag>(
            reg, type_registry_category::language, type_kind::constructor);
        register_type_descriptor<tuple_type_tag>(
            reg, type_registry_category::language, type_kind::constructor);
        register_type_descriptor<struct_type_tag>(
            reg, type_registry_category::language, type_kind::constructor);
        register_type_descriptor<union_type_tag>(
            reg, type_registry_category::language, type_kind::constructor);
        register_type_descriptor<function_type_tag>(
            reg, type_registry_category::language, type_kind::callable);
        register_type_descriptor<optional_type_tag>(
            reg, type_registry_category::language, type_kind::constructor);
        register_type_descriptor<result_type_tag>(
            reg, type_registry_category::language, type_kind::constructor);
        register_type_descriptor<map_type_tag>(
            reg, type_registry_category::language, type_kind::constructor);
        register_type_descriptor<list_type_tag>(
            reg, type_registry_category::language, type_kind::constructor);
        register_type_descriptor<tensor_type_tag>(
            reg, type_registry_category::tensor, type_kind::tensor);

        return reg;
    }
} // namespace vakya::types
