#pragma once

// generic/reflection.hpp — Callable traits, type_descriptor seam, HostCallable concept.
//
// C++23, header-only, no virtual, no macros. Namespace: lang
//
// Extracts the callable introspection infrastructure from crank/host.hpp into a
// language-agnostic form. Any language frontend includes this for:
//   - callable_traits<F>   — return type, parameter pack, arity, is_noexcept
//   - type_descriptor<T>   — seam for C++ type → language type reflection
//   - has_type_descriptor<T> — SFINAE/concept detection
//   - HostCallable concept — accepts free fns + types with operator()
//   - field<Name, Ptr>     — compile-time field member-pointer descriptor
//
// Depends on: meta/meta.hpp (meta::fixed_string)
//
// Usage:
//   // Specialise for a host type:
//   template<> struct lang::type_descriptor<Vec3> {
//     static constexpr std::string_view name = "Vec3";
//     static constexpr auto fields = std::tuple{
//         lang::field<"x", &Vec3::x>{},
//         lang::field<"y", &Vec3::y>{},
//         lang::field<"z", &Vec3::z>{}
//     };
//   };
//   static_assert(lang::has_type_descriptor<Vec3>);

#include "meta/meta.hpp"

#include <cstddef>
#include <tuple>
#include <type_traits>

namespace lang {
    // =========================================================================
    // callable_traits — introspect free functions, noexcept variants,
    //                   const member operator() (non-capturing lambdas).
    // =========================================================================

    namespace detail {
        template <class F, class = void>
        struct callable_traits : callable_traits<decltype(&std::decay_t<F>::operator())> {};

        // Free function pointer
        template <class R, class... Args>
        struct callable_traits<R(*)(Args...), void> {
            using return_type = R;
            using param_types = std::tuple<std::decay_t<Args>...>;
            static constexpr std::size_t arity = sizeof...(Args);
            static constexpr bool is_noexcept = false;
        };

        // Free function pointer noexcept
        template <class R, class... Args>
        struct callable_traits<R(*)(Args...) noexcept, void> {
            using return_type = R;
            using param_types = std::tuple<std::decay_t<Args>...>;
            static constexpr std::size_t arity = sizeof...(Args);
            static constexpr bool is_noexcept = true;
        };

        // Const member operator() (lambda / functor)
        template <class C, class R, class... Args>
        struct callable_traits<R(C::*)(Args...) const, void> {
            using return_type = R;
            using param_types = std::tuple<std::decay_t<Args>...>;
            static constexpr std::size_t arity = sizeof...(Args);
            static constexpr bool is_noexcept = false;
        };

        // Const member operator() noexcept
        template <class C, class R, class... Args>
        struct callable_traits<R(C::*)(Args...) const noexcept, void> {
            using return_type = R;
            using param_types = std::tuple<std::decay_t<Args>...>;
            static constexpr std::size_t arity = sizeof...(Args);
            static constexpr bool is_noexcept = true;
        };
    } // namespace detail

    // Public alias — skips the detail:: prefix at call sites.
    template <class F>
    using callable_traits = detail::callable_traits<F>;

    // =========================================================================
    // HostCallable concept
    // =========================================================================

    template <class F>
    concept HostCallable =
        std::is_function_v<std::remove_pointer_t<F>> ||
        requires(F& f) { &std::decay_t<F>::operator(); };

    // =========================================================================
    // field<Name, Ptr> — compile-time field descriptor for type_descriptor
    // =========================================================================

    template <meta::fixed_string Name, auto Ptr>
    struct field {
        static constexpr auto name = Name;
        static constexpr auto pointer = Ptr;
    };

    // =========================================================================
    // type_descriptor<T> — seam for C++ type → language type reflection.
    // Users specialise this template to expose T to a language frontend:
    //
    //   template<> struct lang::type_descriptor<MyType> {
    //       static constexpr std::string_view name = "MyType";
    //       static constexpr auto fields = std::tuple{
    //           lang::field<"x", &MyType::x>{}, ...
    //       };
    //   };
    //
    // Default: undefined (no reflection = not exposed).
    // =========================================================================

    template <class T>
    struct type_descriptor; // user specialises

    // =========================================================================
    // has_type_descriptor<T> — true when T has a lang::type_descriptor
    // =========================================================================

    template <class T, class = void>
    inline constexpr bool has_type_descriptor = false;

    template <class T>
    inline constexpr bool has_type_descriptor<
        T, std::void_t<decltype(lang::type_descriptor<T>::name)>> = true;

    // =========================================================================
    // typed_thunk helpers — unpack const void** args, call Fn, write result.
    // Shared by any language that needs a typed low-overhead dispatch thunk.
    // =========================================================================

    namespace detail {
        template <auto Fn, class Params, std::size_t... Is>
        void invoke_typed_impl(const void* const* args, void* result,
                               std::index_sequence<Is...>) {
            using Traits = callable_traits<decltype(Fn)>;
            using R = typename Traits::return_type;
            if constexpr (std::is_void_v<R>) {
                Fn(*static_cast<const std::tuple_element_t<Is, Params>*>(args[Is])...);
            }
            else {
                *static_cast<R*>(result) =
                    Fn(*static_cast<const std::tuple_element_t<Is, Params>*>(args[Is])...);
            }
        }

        template <auto Fn>
        [[nodiscard]] constexpr auto make_typed_thunk() noexcept {
            using Traits = callable_traits<decltype(Fn)>;
            return +[](const void* const* args, void* result) {
                invoke_typed_impl<Fn, typename Traits::param_types>(
                    args, result, std::make_index_sequence < Traits::arity >
                {});
            };
        }
    } // namespace detail
} // namespace lang
