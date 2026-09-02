#pragma once
// ============================================================================
// ecs/system.hpp — System Concept and Dependency Traits for pebble::ecs
// ============================================================================
// Pure compile-time reflection of component read/write accesses.
// Zero virtual functions, zero macros, modern C++23.
// ============================================================================

#include "entity.hpp"
#include <concepts>
#include <tuple>
#include <type_traits>

namespace pebble::ecs {
    template <typename... Cs>
    struct Reads {};

    template <typename... Cs>
    struct Writes {};

    template <typename T, typename WorldT>
    concept System = requires(T sys, WorldT& w, float dt) {
        sys.run(w, dt);
    };

    namespace detail {
        template <typename T>
        struct get_reads {
            using type = Reads<>;
        };

        template <typename T>
            requires requires { typename T::reads; }
        struct get_reads<T> {
            using type = typename T::reads;
        };

        template <typename T>
        struct get_writes {
            using type = Writes<>;
        };

        template <typename T>
            requires requires { typename T::writes; }
        struct get_writes<T> {
            using type = typename T::writes;
        };
    } // namespace detail
} // namespace pebble::ecs
