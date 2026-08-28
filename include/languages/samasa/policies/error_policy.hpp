#pragma once

// samasa/policies/error_policy.hpp — Error recovery on/off policy.
//
// C++23, header-only, no virtual, no macros. Namespace: lang::samasa
//
// error_policy_off — no recovery; first hard_fail stops the parse.
// error_policy_on  — recovery enabled; uses skip/insert/delete strategies.

#include <cstdint>

namespace lang::samasa {

    struct error_policy_off {
        static constexpr bool recovery_enabled = false;
    };

    struct error_policy_on {
        static constexpr bool recovery_enabled = true;
        std::uint32_t max_repairs = 64;
    };

} // namespace lang::samasa
