#pragma once

// samasa/lex/keyword_table.hpp — Compile-time keyword lookup.
//
// C++23, header-only, no virtual, no macros. Namespace: lang::samasa
//
// keyword<Name,Kind>      — maps a compile-time string to a TokenKind at hash-first compare.
// keyword_table<KWs...>   — compile-time keyword set; lookup(sv) → optional<TokenKind>.
//
// Lookup: a constexpr entry array sorted by FNV-1a64 hash is built once per table
// instantiation; runtime lookup hashes the word, binary-searches the sorted hashes
// (std::ranges::lower_bound), then probes only the (tiny) equal-hash run doing a
// string compare. O(log N + collisions) per identifier instead of O(N) over all
// keywords. Hashes are 64-bit FNV-1a, so equal-hash runs are ~1 in practice.

#include <algorithm>
#include <array>
#include <optional>
#include <string_view>
#include "meta/akshara.hpp"

namespace lang::samasa { namespace detail {
        [[nodiscard]] constexpr std::uint64_t fnv1a_rt(std::string_view sv) noexcept {
            std::uint64_t h = 14695981039346656037ULL;
            for (unsigned char c : sv) {
                h ^= static_cast<std::uint64_t>(c);
                h *= 1099511628211ULL;
            }
            return h;
        }
    } // namespace detail

    template <akshara::fixed_string Name, auto Kind>
    struct keyword {
        static constexpr auto name = Name;
        static constexpr auto kind = Kind;
        static constexpr std::uint64_t hash = akshara::fnv1a64(Name);
    };

    template <class... KWs>
    struct keyword_table {
    private:
        // Common kind type across all keywords in this table.
        using kind_type = std::common_type_t<decltype(KWs::kind)...>;

        struct entry {
            std::uint64_t hash = 0;
            std::string_view name;
            kind_type kind{};
        };

        // Sorted-by-hash entry table, built once at compile time.
        static constexpr auto sorted_entries = [] {
            std::array<entry, sizeof...(KWs)> arr{
                entry{
                    KWs::hash, static_cast<std::string_view>(KWs::name),
                    static_cast<kind_type>(KWs::kind)
                }...
            };
            std::ranges::sort(arr, {}, &entry::hash);
            return arr;
        }();

    public:
        // Returns the TokenKind for sv if it matches any keyword, else nullopt.
        template <class TokenKind>
        [[nodiscard]] static constexpr std::optional<TokenKind> lookup(std::string_view sv) noexcept {
            const std::uint64_t h = detail::fnv1a_rt(sv);
            // Binary-search the first entry with hash >= h, then walk the equal-hash run.
            auto it = std::ranges::lower_bound(sorted_entries, h, {}, &entry::hash);
            for (; it != sorted_entries.end() && it->hash == h; ++it)
                if (it->name == sv)
                    return static_cast<TokenKind>(it->kind);
            return std::nullopt;
        }

        // Returns true if sv is a keyword.
        [[nodiscard]] static constexpr bool is_keyword(std::string_view sv) noexcept {
            const std::uint64_t h = detail::fnv1a_rt(sv);
            auto it = std::ranges::lower_bound(sorted_entries, h, {}, &entry::hash);
            for (; it != sorted_entries.end() && it->hash == h; ++it)
                if (it->name == sv)
                    return true;
            return false;
        }

        static constexpr std::size_t size = sizeof...(KWs);
    };

    // Empty keyword table specialization.
    template <>
    struct keyword_table<> {
        template <class TokenKind>
        [[nodiscard]] static constexpr std::optional<TokenKind> lookup(std::string_view) noexcept {
            return std::nullopt;
        }

        [[nodiscard]] static constexpr bool is_keyword(std::string_view) noexcept { return false; }
        static constexpr std::size_t size = 0;
    };
} // namespace lang::samasa
