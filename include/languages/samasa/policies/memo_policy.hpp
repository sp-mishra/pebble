#pragma once

// samasa/policies/memo_policy.hpp — Memoization policies (opt-in, zero cost when disabled).
//
// C++23, header-only, no virtual, no macros. Namespace: lang::samasa
//
// no_memo         — default; zero overhead; no table.
// selective_memo  — per-rule opt-in via memoized<Rule> wrapper.
// packrat_memo    — full Earley/packrat: O(rules × input) memory.
//
// memo_key   — (rule_hash, token_pos) identifies a unique memo entry.
// memo_value — cached result: status + next_pos + furthest_error.
//
// memoized<Rule> — combinator wrapper: checks memo table before calling Rule.match();
//   stores result on first call. Works with any Ctx that exposes a memo() accessor
//   returning a reference to a MemoPolicy object. Falls through transparently when
//   no_memo policy is active (the memo() calls are constexpr-dead-eliminated).
//
// Usage:
//   using expr_rule = memoized<rule<"expr", expr_pattern>>;
//
// Memo lookup/store are no-ops for no_memo — zero overhead at call site.

#include <cstdint>
#include <string_view>
#include <unordered_map>
#include "../core/result.hpp"

namespace lang::samasa {

    // ---- memo_key ----------------------------------------------------------

    struct memo_key {
        std::uint64_t rule_hash  = 0;
        std::uint32_t token_pos  = 0;

        bool operator==(const memo_key&) const noexcept = default;
    };

    struct memo_key_hash {
        std::size_t operator()(const memo_key& k) const noexcept {
            // FNV-style mix.
            std::uint64_t h = k.rule_hash ^ (static_cast<std::uint64_t>(k.token_pos) * 0x9e37'79b9'7f4a'7c15ULL);
            h ^= h >> 30;
            h *= 0xbf58476d1ce4e5b9ULL;
            h ^= h >> 27;
            return static_cast<std::size_t>(h);
        }
    };

    // ---- memo_value --------------------------------------------------------

    struct memo_value {
        parse_status  status       = parse_status::soft_fail;
        std::uint32_t next_pos     = 0;
        std::uint32_t furthest_err = 0;
        bool          valid        = false;
    };

    // ---- no_memo -----------------------------------------------------------

    struct no_memo {
        static constexpr bool enabled = false;

        [[nodiscard]] constexpr bool lookup(memo_key, memo_value&) const noexcept {
            return false;
        }
        constexpr void store(memo_key, memo_value) noexcept {}
        constexpr void reserve(std::size_t) noexcept {}
    };

    // ---- selective_memo ----------------------------------------------------
    // Stores results only for rules explicitly wrapped with memoized<Rule>.
    // Uses an unordered_map keyed on memo_key.

    struct selective_memo {
        static constexpr bool enabled = true;

        [[nodiscard]] bool lookup(memo_key k, memo_value& out) const {
            auto it = table_.find(k);
            if (it == table_.end()) return false;
            out = it->second;
            return true;
        }

        void store(memo_key k, memo_value v) {
            table_.emplace(k, v);
        }

        // Pre-size the table to avoid rehash churn; call once when the token
        // count is known (a conservative bucket hint).
        void reserve(std::size_t n) {
            table_.reserve(n);
        }

    private:
        std::unordered_map<memo_key, memo_value, memo_key_hash> table_;
    };

    // ---- packrat_memo ------------------------------------------------------
    // Full packrat: memoizes all rules at all positions.
    // Same storage as selective_memo — distinction is who inserts entries.

    struct packrat_memo {
        static constexpr bool enabled = true;

        [[nodiscard]] bool lookup(memo_key k, memo_value& out) const {
            auto it = table_.find(k);
            if (it == table_.end()) return false;
            out = it->second;
            return true;
        }

        void store(memo_key k, memo_value v) {
            table_.insert_or_assign(k, v);
        }

        // Pre-size the table to avoid rehash churn; call once when the token
        // count is known (packrat inserts up to rules × positions).
        void reserve(std::size_t n) {
            table_.reserve(n);
        }

    private:
        std::unordered_map<memo_key, memo_value, memo_key_hash> table_;
    };

    // ---- memoized<Rule> ----------------------------------------------------
    // Wraps a Rule with memo lookup/store.
    //
    // Ctx must expose:
    //   ctx.memo()  → reference to a MemoPolicy (selective_memo or packrat_memo).
    //
    // When Ctx::memo_policy is no_memo, all memo calls are elided at compile time.
    // The rule_hash is derived from Rule::name (FNV-1a of the name string).

    namespace detail {
        // Compile-time FNV-1a64 of a string_view for use as rule_hash.
        consteval std::uint64_t memo_fnv(std::string_view s) noexcept {
            std::uint64_t h = 14695981039346656037ULL;
            for (char c : s) {
                h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
                h *= 1099511628211ULL;
            }
            return h;
        }
    }

    template <class Rule>
    struct memoized {
        Rule rule;

        template <class Ctx>
        [[nodiscard]] auto match(Ctx& ctx) const {
            using Stream = typename Ctx::stream_type;
            using R      = parse_result<Stream>;

            // Check if Ctx provides memo() — use if constexpr for zero overhead.
            if constexpr (requires { ctx.memo(); }) {
                constexpr std::uint64_t hash = detail::memo_fnv(Rule::name_sv);
                const memo_key key{hash, ctx.cursor().pos};

                memo_value cached;
                if (ctx.memo().lookup(key, cached)) {
                    // Restore cursor to cached next_pos.
                    auto next_cur = ctx.cursor();
                    next_cur.pos  = cached.next_pos;
                    if (cached.status == parse_status::success)
                        return R::success_at(next_cur, cached.furthest_err);
                    if (cached.status == parse_status::hard_fail)
                        return R::hard_failure(ctx.cursor(), cached.furthest_err);
                    return R::soft_failure(ctx.cursor(), cached.furthest_err);
                }

                auto r = rule.match(ctx);
                memo_value mv{r.status, r.ok() ? r.next.pos : ctx.cursor().pos,
                              r.furthest_error, true};
                ctx.memo().store(key, mv);
                return r;
            } else {
                return rule.match(ctx);
            }
        }
    };

    template <class Rule>
    [[nodiscard]] constexpr memoized<Rule> make_memoized(Rule r) {
        return {std::move(r)};
    }

} // namespace lang::samasa
