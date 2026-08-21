#pragma once
// =============================================================================
// medha/retry.hpp — retry_policy concept + retry_state
//
// C++23, header-only, no virtual, no macros.
//
// retry::none     — no retry; return conflict immediately
// retry::bounded  — retry up to max times
// retry::backoff  — retry with exponential backoff up to max times
//
// retry_state: tracks attempt count; knows if exhausted.
// =============================================================================

#include "medha/options.hpp"

#include <chrono>
#include <cstdint>
#include <variant>

namespace medha {
    // ============================================================================
    // retry_state — tracks attempt count for the retry loop (§20.5)
    // ============================================================================

    class retry_state {
    public:
        explicit retry_state(const std::variant<retry::none,
                                                retry::bounded,
                                                retry::backoff>& policy)
            : policy_(policy) {}

        [[nodiscard]] std::uint32_t attempt() const noexcept { return attempt_; }

        // Returns true if another retry is allowed; false if exhausted.
        [[nodiscard]] bool can_retry() const noexcept {
            return std::visit([&](const auto& p) -> bool {
                using T = std::decay_t<decltype(p)>;
                if constexpr (std::is_same_v<T, retry::none>) {
                    return false;
                }
                else {
                    return attempt_ < p.max;
                }
            }, policy_);
        }

        // Returns delay for this retry (0 for bounded; backoff for backoff policy).
        [[nodiscard]] std::chrono::nanoseconds next_delay() const noexcept {
            return std::visit([&](const auto& p) -> std::chrono::nanoseconds {
                using T = std::decay_t<decltype(p)>;
                if constexpr (std::is_same_v<T, retry::backoff>) {
                    double d = p.base.count();
                    for (std::uint32_t i = 0; i < attempt_; ++i) d *= p.factor;
                    return std::chrono::nanoseconds(static_cast<std::int64_t>(d));
                }
                return std::chrono::nanoseconds{0};
            }, policy_);
        }

        void advance() noexcept { ++attempt_; }

    private:
        std::variant<retry::none, retry::bounded, retry::backoff> policy_;
        std::uint32_t attempt_ = 0;
    };
} // namespace medha
