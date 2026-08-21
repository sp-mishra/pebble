#pragma once

// samasa/core/result.hpp — Three-state parse result.
//
// C++23, header-only, no virtual, no macros. Namespace: lang::samasa
//
// parse_status  — success | soft_fail (backtrack) | hard_fail (cut committed).
// parse_result  — trivially copyable {status, next cursor, furthest_error byte offset}.
//
// furthest_error tracks the rightmost position reached for best-error diagnostics.

#include <cstdint>
#include "cursor.hpp"

namespace lang::samasa {

    enum class parse_status : std::uint8_t {
        success   = 0,
        soft_fail = 1,  // backtrackable
        hard_fail = 2,  // committed; cut was hit upstream
    };

    template <class Stream>
    struct parse_result {
        parse_status    status         = parse_status::soft_fail;
        cursor<Stream>  next           = {};
        std::uint32_t   furthest_error = 0; // byte offset of rightmost error seen

        [[nodiscard]] constexpr bool ok()        const noexcept { return status == parse_status::success;   }
        [[nodiscard]] constexpr bool soft_fail() const noexcept { return status == parse_status::soft_fail; }
        [[nodiscard]] constexpr bool hard_fail() const noexcept { return status == parse_status::hard_fail; }
        [[nodiscard]] constexpr bool failed()    const noexcept { return status != parse_status::success;   }

        [[nodiscard]] static constexpr parse_result
        success_at(cursor<Stream> c, std::uint32_t fe = 0) noexcept {
            return {parse_status::success, c, fe};
        }
        [[nodiscard]] static constexpr parse_result
        soft_failure(cursor<Stream> c, std::uint32_t fe = 0) noexcept {
            return {parse_status::soft_fail, c, fe};
        }
        [[nodiscard]] static constexpr parse_result
        hard_failure(cursor<Stream> c, std::uint32_t fe = 0) noexcept {
            return {parse_status::hard_fail, c, fe};
        }

        // Promote soft fail to hard fail (used by cut combinator).
        [[nodiscard]] constexpr parse_result harden() const noexcept {
            if (status == parse_status::soft_fail)
                return {parse_status::hard_fail, next, furthest_error};
            return *this;
        }
    };

} // namespace lang::samasa
