#pragma once

// generic/descriptors.hpp — Language-agnostic function/type/field/resource descriptors.
//
// C++23, header-only, no virtual, no macros. Namespace: lang
//
// Provides the generic descriptor structs shared across all language frontends.
// Language-specific fields (blocking_class, source_boundary_policy, etc.) stay
// in the language's own headers. These descriptors form the stable ABI of the
// generic layer.
//
// Depends on: generic/identity.hpp, generic/reflection.hpp
//
// function_descriptor_base — generic function descriptor (arity, effects, thunk).
// type_descriptor_base     — generic type descriptor (name, size, alignment).
// field_descriptor_base    — generic field descriptor (name, offset, type_name).
// resource_descriptor_base — generic resource descriptor (name, lifetime hint).
//
// Usage:
//   // Language-specific descriptors extend these:
//   struct my_function_descriptor : lang::function_descriptor_base {
//       my_blocking_class blocking = my_blocking_class::non_blocking;
//   };

#include "languages/generic/core/identity.hpp"
#include "languages/generic/core/reflection.hpp"

#include <any>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>

namespace lang {
    // =========================================================================
    // function_flag — attribute bits common to all language functions
    // =========================================================================

    enum class function_flag : std::uint32_t {
        pure = 1u << 0, // no observable side-effects
        thread_safe = 1u << 1, // safe to call from multiple threads
        deterministic = 1u << 2, // same inputs → same output (no hidden state)
        blocking = 1u << 3, // may block on I/O or locks
        asynchronous = 1u << 4, // returns a future/coroutine handle
        gpu_compatible = 1u << 5, // can run on GPU backend
    };

    using function_flags = std::uint32_t;

    // =========================================================================
    // function_descriptor_base — generic function descriptor
    // =========================================================================

    struct function_descriptor_base {
        stable_function_id id;
        std::string name; // qualified name, e.g. "math.dot"
        std::size_t arity = 0;

        std::uint64_t effect_mask = 0; // language-defined effect bits
        std::uint64_t capability_mask = 0; // language-defined capability bits
        function_flags flags = 0;

        // Direct typed thunk: args[i] are const void* pointing to typed C++ values.
        // result is void* pointing to storage for the return value.
        // nullptr for capturing callables (use trampoline instead).
        void (*typed_thunk)(const void* const* args, void* result) = nullptr;

        // Erased trampoline (explicit dynamic boundary — not for hot paths).
        std::function<std::any(std::span<const std::any>)> trampoline;

        descriptor_fingerprint fingerprint = 0;
    };

    // =========================================================================
    // type_descriptor_base — generic type descriptor
    // =========================================================================

    struct type_descriptor_base {
        stable_type_id id;
        std::string name;
        std::size_t byte_size = 0;
        std::size_t alignment = 0;
        descriptor_fingerprint fingerprint = 0;
    };

    // =========================================================================
    // field_descriptor_base — generic field descriptor
    // =========================================================================

    struct field_descriptor_base {
        stable_field_id id;
        std::string name;
        std::string type_name; // qualified type name of the field
        std::size_t offset = 0;
        std::size_t byte_size = 0;
        bool is_mutable = true;
    };

    // =========================================================================
    // resource_descriptor_base — generic resource descriptor
    // =========================================================================

    enum class resource_lifetime_hint : std::uint8_t {
        owned, // resource is owned by the context; freed on context teardown
        borrowed, // caller retains ownership; context holds a non-owning view
        shared, // reference-counted; freed when last handle drops
    };

    struct resource_descriptor_base {
        stable_resource_id id;
        std::string name;
        resource_lifetime_hint lifetime = resource_lifetime_hint::owned;
        descriptor_fingerprint fingerprint = 0;
    };

    // =========================================================================
    // make_function_descriptor<Name, Fn> — build a function_descriptor_base
    // with a direct typed thunk derived from the function pointer/lambda.
    // =========================================================================

    template <meta::fixed_string Name, auto Fn>
    [[nodiscard]] function_descriptor_base make_function_descriptor() {
        using Traits = callable_traits<decltype(Fn)>;
        static_assert(Traits::arity <= 16,
                      "make_function_descriptor: functions with >16 parameters are not supported");

        function_descriptor_base d;
        d.name = std::string(Name.value);
        d.arity = Traits::arity;
        d.typed_thunk = detail::make_typed_thunk<Fn>();
        d.id = detail::make_id(d.name, kKindFunction);
        d.fingerprint = detail::fp_combine(
            d.id.namespace_hash,
            detail::fp_with_scalar(d.id.name_hash, static_cast<std::uint64_t>(d.arity)));
        return d;
    }
} // namespace lang
