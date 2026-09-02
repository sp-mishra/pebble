#pragma once

// samasa/policies/execution_policy.hpp — Runtime vs consteval execution traits.
//
// C++23, header-only, no virtual, no macros. Namespace: lang::samasa
//
// runtime_execution  — default; dynamic allocation, std::vector, runtime sink.
// consteval_execution — compile-time fixed-capacity arrays; for parse_static<G, Src>().

#include <cstdint>

namespace lang::samasa {
    struct runtime_execution {
        static constexpr bool is_consteval = false;
    };

    struct consteval_execution {
        static constexpr bool is_consteval = true;
        std::uint32_t max_tokens = 4096;
        std::uint32_t max_nodes = 4096;
        std::uint32_t max_diags = 64;
    };
} // namespace lang::samasa
