#pragma once

// samasa/core/limits.hpp — Parse depth and budget constants.
//
// C++23, header-only, no virtual, no macros. Namespace: lang::samasa
//
// limits — constexpr struct controlling resource budgets during a parse run.
// Exceeding max_depth/max_nodes terminates with hard_fail + diagnostic.
// max_repairs caps error-recovery iterations.

#include <cstdint>

namespace lang::samasa {

    struct limits {
        std::uint32_t max_depth   = 512;
        std::uint32_t max_nodes   = 1'048'576; // 1 M nodes
        std::uint32_t max_repairs = 64;

        [[nodiscard]] static constexpr limits tight() noexcept {
            return {128, 65536, 16};
        }
        [[nodiscard]] static constexpr limits relaxed() noexcept {
            return {1024, 4'000'000, 256};
        }
    };

} // namespace lang::samasa
