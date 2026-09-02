#pragma once

// samasa/expr/operator_table.hpp — Compile-time Pratt operator descriptor set.
//
// C++23, header-only, no virtual, no macros. Namespace: lang::samasa
//
// op<Symbol,TokenVal,BP,Assoc,Fix> — describes one operator.
// operator_table<Ops...>           — type-level set of operators.
//   prefix_bp(kind)  → binding power if prefix
//   infix_bp(kind)   → {left_bp, right_bp} if infix
//   postfix_bp(kind) → binding power if postfix

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <utility>
#include "meta/akshara.hpp"
#include "precedence.hpp"

namespace lang::samasa {
    template <akshara::fixed_string Symbol, auto TokenVal,
              std::uint8_t BP,
              associativity Assoc = associativity::left,
              fixity Fix = fixity::infix>
    struct op {
        static constexpr auto symbol = Symbol;
        static constexpr auto token = TokenVal;
        static constexpr std::uint8_t bp = BP;
        static constexpr associativity assoc = Assoc;
        static constexpr fixity fix = Fix;
    };

    template <class... Ops>
    struct operator_table {
    private:
        // Common token value type across all operators in this table.
        using token_type = std::common_type_t<decltype(Ops::token)...>;

        struct entry {
            token_type token{};
            std::uint8_t bp = 0;
            associativity assoc = associativity::left;
        };

        // Build a constexpr array (sorted by token value) of the operators whose
        // fixity matches Fix. Runtime *_bp() binary-searches it. One op per (token,
        // fixity), so lower_bound + equality is sufficient.
        template <fixity Fix>
        static constexpr auto build_sorted() {
            constexpr std::size_t n = ((Ops::fix == Fix) + ... + std::size_t{0});
            std::array<entry, n> arr{};
            std::size_t i = 0;
            ([&]<class O>() {
                if constexpr (O::fix == Fix)
                    arr[i++] = entry{static_cast<token_type>(O::token), O::bp, O::assoc};
            }.template operator()<Ops>(), ...);
            std::ranges::sort(arr, {}, &entry::token);
            return arr;
        }

        static constexpr auto prefix_ops = build_sorted<fixity::prefix>();
        static constexpr auto infix_ops = build_sorted<fixity::infix>();
        static constexpr auto postfix_ops = build_sorted<fixity::postfix>();

        template <std::size_t N>
        [[nodiscard]] static constexpr const entry*
        find(const std::array<entry, N>& arr, token_type t) noexcept {
            auto it = std::ranges::lower_bound(arr, t, {}, &entry::token);
            if (it != arr.end() && it->token == t) return &*it;
            return nullptr;
        }

    public:
        template <class TokenKind>
        [[nodiscard]] static constexpr std::optional<std::uint8_t>
        prefix_bp(TokenKind k) noexcept {
            if (const entry* e = find(prefix_ops, static_cast<token_type>(k)))
                return e->bp;
            return std::nullopt;
        }

        template <class TokenKind>
        [[nodiscard]] static constexpr std::optional<std::pair<std::uint8_t, std::uint8_t>>
        infix_bp(TokenKind k) noexcept {
            const entry* e = find(infix_ops, static_cast<token_type>(k));
            if (!e) return std::nullopt;
            const std::uint8_t lbp = e->bp;
            std::uint8_t rbp = lbp;
            if (e->assoc == associativity::left) rbp = lbp + 1;
            else if (e->assoc == associativity::none) rbp = lbp + 1;
            // right: rbp == lbp
            return std::pair<std::uint8_t, std::uint8_t>{lbp, rbp};
        }

        template <class TokenKind>
        [[nodiscard]] static constexpr std::optional<std::uint8_t>
        postfix_bp(TokenKind k) noexcept {
            if (const entry* e = find(postfix_ops, static_cast<token_type>(k)))
                return e->bp;
            return std::nullopt;
        }

        static constexpr std::size_t size = sizeof...(Ops);
    };
} // namespace lang::samasa
