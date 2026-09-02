#pragma once

// samasa/recovery/sync_set.hpp — Synchronisation token sets for error recovery.
//
// C++23, header-only, no virtual, no macros. Namespace: lang::samasa
//
// sync_set<TokenKinds...> — compile-time set of synchronisation tokens.
//   contains(k) — true if k is a sync token.
//
// Membership is a binary search over a constexpr array of the token values,
// sorted once at compile time — O(log N) per recovery check instead of the
// linear fold. Empty set is a constant false.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace lang::samasa {
    template <auto... TokenKinds>
    struct sync_set {
    private:
        using token_type = std::common_type_t<decltype(TokenKinds)...>;

        static constexpr auto sorted = [] {
            std::array<token_type, sizeof...(TokenKinds)> arr{
                static_cast<token_type>(TokenKinds)...
            };
            std::ranges::sort(arr);
            return arr;
        }();

    public:
        template <class TokenKind>
        [[nodiscard]] static constexpr bool contains(TokenKind k) noexcept {
            return std::ranges::binary_search(sorted, static_cast<token_type>(k));
        }

        static constexpr std::size_t size = sizeof...(TokenKinds);
    };

    // Empty sync set — contains nothing.
    template <>
    struct sync_set<> {
        template <class TokenKind>
        [[nodiscard]] static constexpr bool contains(TokenKind) noexcept { return false; }

        static constexpr std::size_t size = 0;
    };
} // namespace lang::samasa
