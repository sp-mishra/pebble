#pragma once

// samasa/grammar/expected_sets.hpp — Compile-time FIRST-set and FOLLOW-set computation.
//
// C++23, header-only, no virtual, no macros. Namespace: lang::samasa
//
// expected_at<Rule,TK>()    — consteval std::array<TK,N> of FIRST tokens for Rule.
// first_sets<G>()           — grammar_first_sets<G> with per-rule nullability descriptors.
// follow_sets<G>()          — grammar_follow_sets<G> with per-rule FOLLOW token sets.
// follow_of<G,Rule>()       — consteval FOLLOW set for a specific rule.
// expected_after<G,Rule>()  — consteval FIRST(rule) ∪ FOLLOW(rule).
//
// PEG semantics:
//   FIRST(tok<K>)          = {K}
//   FIRST(seq<A,B,...>)    = FIRST(A) ∪ FIRST(B) only if A nullable (etc.)
//   FIRST(choice<A,B,...>) = FIRST(A) ∪ FIRST(B)
//   FIRST(opt<A>)          = FIRST(A)
//   FIRST(many<A>)         = FIRST(A)
//   FIRST(sep_by<A,Sep>)   = FIRST(A)
//   FIRST(not_followed_by) = {} (no consumption)
//   Non-tok terminals      = {} (char_lit, char_in, token_text, etc.)
//
// FOLLOW semantics (PEG, fixed-point iteration):
//   FOLLOW(root)            = {EOF sentinel}
//   For rule R appearing at pos i in seq(A0,...,Ri,...,Ak):
//     FOLLOW(R) ⊇ FIRST(A_{i+1},...,Ak)
//   If A_{i+1}...Ak are all nullable: FOLLOW(R) ⊇ FOLLOW(caller).
//   Fixed-point: iterate until no FOLLOW set grows — result is a conservative superset.
//   For recursive or mutually recursive grammars this is more precise than single-pass.

#include "grammar_ir.hpp"
#include <array>
#include <algorithm>

namespace lang::samasa {

    namespace detail {

        // ---- first_count<Pattern,TK> ----------------------------------------
        // Counts upper-bound number of distinct tok<> entries reachable.
        // Conservative: counts all elements in seq (even non-nullable ones).

        template <class Pattern, class TokenKind>
        struct first_count : std::integral_constant<std::size_t, 0> {};

        template <auto Kind, class TokenKind>
        struct first_count<tok<Kind>, TokenKind>
            : std::integral_constant<std::size_t, 1> {};

        template <class... Ms, class TokenKind>
        struct first_count<seq_t<Ms...>, TokenKind>
            : std::integral_constant<std::size_t, (first_count<Ms,TokenKind>::value + ...)> {};

        template <class... Ms, class TokenKind>
        struct first_count<choice_t<Ms...>, TokenKind>
            : std::integral_constant<std::size_t, (first_count<Ms,TokenKind>::value + ...)> {};

        template <class M, class TokenKind>
        struct first_count<many_t<M>, TokenKind> : first_count<M,TokenKind> {};

        template <class M, class TokenKind>
        struct first_count<many1_t<M>, TokenKind> : first_count<M,TokenKind> {};

        template <class M, class TokenKind>
        struct first_count<opt_t<M>, TokenKind> : first_count<M,TokenKind> {};

        template <class A, class Sep, class TokenKind>
        struct first_count<sep_by_t<A,Sep>, TokenKind> : first_count<A,TokenKind> {};

        template <class A, class Sep, class TokenKind>
        struct first_count<sep_by1_t<A,Sep>, TokenKind> : first_count<A,TokenKind> {};

        template <class M, class TokenKind>
        struct first_count<lookahead_t<M>, TokenKind> : first_count<M,TokenKind> {};

        template <class M, class TokenKind>
        struct first_count<not_followed_by_t<M>, TokenKind>
            : std::integral_constant<std::size_t, 0> {};

        template <akshara::fixed_string Name, class Pattern, class TokenKind>
        struct first_count<rule<Name,Pattern>, TokenKind>
            : first_count<Pattern,TokenKind> {};

        template <auto Kind, class Pattern, class TK>
        struct first_count<node_t<Kind,Pattern>, TK>
            : first_count<Pattern,TK> {};

        template <class Pattern, class Recovery, class TK>
        struct first_count<recover_with<Pattern,Recovery>, TK>
            : first_count<Pattern,TK> {};

        // ---- fill_first_for — recursive FIRST fill ---------------------------
        // Each overload fills tok<Kind> values into arr[0..N), updates idx in place.
        // Returns new idx.

        // Default: no-op for unknown/non-tok terminals.
        template <class Pattern, class TokenKind, std::size_t N>
        consteval std::size_t fill_first_for(Pattern, std::array<TokenKind,N>&, std::size_t idx) {
            return idx;
        }

        // tok<Kind>
        template <auto Kind, class TokenKind, std::size_t N>
        consteval std::size_t fill_first_for(tok<Kind>, std::array<TokenKind,N>& arr, std::size_t idx) {
            if (idx < N) arr[idx++] = static_cast<TokenKind>(Kind);
            return idx;
        }

        // seq_t: PEG sequential — continue to next if previous is nullable.
        template <class... Ms, class TokenKind, std::size_t N>
        consteval std::size_t fill_first_for(seq_t<Ms...>, std::array<TokenKind,N>& arr, std::size_t idx);

        // choice_t: include FIRST of all alternatives.
        template <class... Ms, class TokenKind, std::size_t N>
        consteval std::size_t fill_first_for(choice_t<Ms...>, std::array<TokenKind,N>& arr, std::size_t idx);

        // many_t
        template <class M, class TokenKind, std::size_t N>
        consteval std::size_t fill_first_for(many_t<M>, std::array<TokenKind,N>& arr, std::size_t idx) {
            return fill_first_for(M{}, arr, idx);
        }

        // many1_t
        template <class M, class TokenKind, std::size_t N>
        consteval std::size_t fill_first_for(many1_t<M>, std::array<TokenKind,N>& arr, std::size_t idx) {
            return fill_first_for(M{}, arr, idx);
        }

        // opt_t
        template <class M, class TokenKind, std::size_t N>
        consteval std::size_t fill_first_for(opt_t<M>, std::array<TokenKind,N>& arr, std::size_t idx) {
            return fill_first_for(M{}, arr, idx);
        }

        // sep_by_t
        template <class A, class Sep, class TokenKind, std::size_t N>
        consteval std::size_t fill_first_for(sep_by_t<A,Sep>, std::array<TokenKind,N>& arr, std::size_t idx) {
            return fill_first_for(A{}, arr, idx);
        }

        // sep_by1_t
        template <class A, class Sep, class TokenKind, std::size_t N>
        consteval std::size_t fill_first_for(sep_by1_t<A,Sep>, std::array<TokenKind,N>& arr, std::size_t idx) {
            return fill_first_for(A{}, arr, idx);
        }

        // lookahead_t: positive lookahead contributes FIRST of inner pattern.
        template <class M, class TokenKind, std::size_t N>
        consteval std::size_t fill_first_for(lookahead_t<M>, std::array<TokenKind,N>& arr, std::size_t idx) {
            return fill_first_for(M{}, arr, idx);
        }

        // not_followed_by: contributes nothing.
        template <class M, class TokenKind, std::size_t N>
        consteval std::size_t fill_first_for(not_followed_by_t<M>, std::array<TokenKind,N>&, std::size_t idx) {
            return idx;
        }

        // rule
        template <akshara::fixed_string Name, class Pattern, class TokenKind, std::size_t N>
        consteval std::size_t fill_first_for(rule<Name,Pattern>, std::array<TokenKind,N>& arr, std::size_t idx) {
            return fill_first_for(Pattern{}, arr, idx);
        }

        // node_t
        template <auto Kind, class Pattern, class TK, std::size_t N>
        consteval std::size_t fill_first_for(node_t<Kind,Pattern>, std::array<TK,N>& arr, std::size_t idx) {
            return fill_first_for(Pattern{}, arr, idx);
        }

        // recover_with
        template <class Pattern, class Recovery, class TK, std::size_t N>
        consteval std::size_t fill_first_for(recover_with<Pattern,Recovery>, std::array<TK,N>& arr, std::size_t idx) {
            return fill_first_for(Pattern{}, arr, idx);
        }

        // seq_t implementation — after all overloads are declared.
        namespace seq_detail {
            template <std::size_t I, class Tuple, class TokenKind, std::size_t N>
            consteval std::size_t seq_fill(const Tuple&, std::array<TokenKind,N>& arr, std::size_t idx) {
                if constexpr (I >= std::tuple_size_v<Tuple>) {
                    return idx;
                } else {
                    using M = std::tuple_element_t<I, Tuple>;
                    idx = fill_first_for(M{}, arr, idx);
                    if constexpr (nullable_v<M>)
                        return seq_fill<I+1>(Tuple{}, arr, idx);
                    else
                        return idx;
                }
            }
        }

        template <class... Ms, class TokenKind, std::size_t N>
        consteval std::size_t fill_first_for(seq_t<Ms...>, std::array<TokenKind,N>& arr, std::size_t idx) {
            return seq_detail::seq_fill<0>(std::tuple<Ms...>{}, arr, idx);
        }

        // choice_t implementation — iterate over all alternatives.
        namespace choice_detail {
            template <std::size_t I, class Tuple, class TokenKind, std::size_t N>
            consteval std::size_t choice_fill(const Tuple&, std::array<TokenKind,N>& arr, std::size_t idx) {
                if constexpr (I >= std::tuple_size_v<Tuple>) {
                    return idx;
                } else {
                    using M = std::tuple_element_t<I, Tuple>;
                    idx = fill_first_for(M{}, arr, idx);
                    return choice_fill<I+1>(Tuple{}, arr, idx);
                }
            }
        }

        template <class... Ms, class TokenKind, std::size_t N>
        consteval std::size_t fill_first_for(choice_t<Ms...>, std::array<TokenKind,N>& arr, std::size_t idx) {
            return choice_detail::choice_fill<0>(std::tuple<Ms...>{}, arr, idx);
        }

        // ---- dedup_sorted ---------------------------------------------------
        template <class T, std::size_t N>
        consteval std::size_t dedup_sorted(std::array<T,N>& arr, std::size_t count) {
            if (count <= 1) return count;
            std::size_t out = 1;
            for (std::size_t i = 1; i < count; ++i)
                if (arr[i] != arr[out-1]) arr[out++] = arr[i];
            return out;
        }

        // ---- FOLLOW set helpers: fixed-point iteration ----------------------
        // fill_follow_for_name: walk Pattern; for every rule<Name,P> where Name ==
        // target inside a seq context, collect:
        //   (a) FIRST of the continuation after target, and
        //   (b) if the continuation is all-nullable, the caller_follow tokens.
        // Returns new idx.

        // Forward declarations.
        template <class Pattern, class TK, std::size_t N>
        consteval std::size_t fill_follow_for_name(
            std::string_view target, Pattern,
            const std::array<TK,N>& caller_follow, std::size_t caller_count,
            std::array<TK,N>& arr, std::size_t idx);

        // Default: no contribution.
        template <class Pattern, class TK, std::size_t N>
        consteval std::size_t fill_follow_for_name(
            std::string_view, Pattern,
            const std::array<TK,N>&, std::size_t,
            std::array<TK,N>&, std::size_t idx) { return idx; }

        // seq_t: main case — for each element equal to target, fill FIRST of tail,
        // and if the tail is all-nullable propagate caller_follow.
        namespace follow_seq_impl {
            // is_all_nullable<I, Tuple>: true if all elements from I onwards are nullable.
            template <std::size_t I, class Tuple>
            consteval bool is_all_nullable_from() {
                if constexpr (I >= std::tuple_size_v<Tuple>) return true;
                else {
                    using M = std::tuple_element_t<I, Tuple>;
                    if constexpr (!nullable_v<M>) return false;
                    else return is_all_nullable_from<I+1, Tuple>();
                }
            }

            // fill_cont<I, Tuple>: fill FIRST(elements I..end) into arr.
            template <std::size_t I, class Tuple, class TK, std::size_t N>
            consteval std::size_t fill_cont(std::array<TK,N>& arr, std::size_t idx) {
                if constexpr (I >= std::tuple_size_v<Tuple>) return idx;
                else {
                    using M = std::tuple_element_t<I, Tuple>;
                    idx = fill_first_for(M{}, arr, idx);
                    if constexpr (nullable_v<M>)
                        return fill_cont<I+1, Tuple, TK, N>(arr, idx);
                    else return idx;
                }
            }

            template <std::size_t I, class Tuple, class TK, std::size_t N>
            consteval std::size_t seq_scan(
                std::string_view target,
                const std::array<TK,N>& caller_follow, std::size_t caller_count,
                std::array<TK,N>& arr, std::size_t idx)
            {
                if constexpr (I >= std::tuple_size_v<Tuple>) return idx;
                else {
                    using M = std::tuple_element_t<I, Tuple>;
                    if constexpr (requires { M::name_sv; }) {
                        if (M::name_sv == target) {
                            // FIRST of continuation.
                            idx = fill_cont<I+1, Tuple, TK, N>(arr, idx);
                            // If continuation is all-nullable, propagate caller FOLLOW.
                            if (is_all_nullable_from<I+1, Tuple>()) {
                                for (std::size_t k = 0; k < caller_count; ++k)
                                    if (idx < N) arr[idx++] = caller_follow[k];
                            }
                        }
                    }
                    // Recurse into M's body for nested occurrences.
                    idx = fill_follow_for_name(target, M{}, caller_follow, caller_count, arr, idx);
                    return seq_scan<I+1, Tuple, TK, N>(target, caller_follow, caller_count, arr, idx);
                }
            }
        }

        template <class... Ms, class TK, std::size_t N>
        consteval std::size_t fill_follow_for_name(
            std::string_view target, seq_t<Ms...>,
            const std::array<TK,N>& caller_follow, std::size_t caller_count,
            std::array<TK,N>& arr, std::size_t idx)
        {
            return follow_seq_impl::seq_scan<0, std::tuple<Ms...>, TK, N>(
                target, caller_follow, caller_count, arr, idx);
        }

        template <class... Ms, class TK, std::size_t N>
        consteval std::size_t fill_follow_for_name(
            std::string_view target, choice_t<Ms...>,
            const std::array<TK,N>& caller_follow, std::size_t caller_count,
            std::array<TK,N>& arr, std::size_t idx)
        {
            ((idx = fill_follow_for_name(target, Ms{}, caller_follow, caller_count, arr, idx)), ...);
            return idx;
        }

        template <class M, class TK, std::size_t N>
        consteval std::size_t fill_follow_for_name(
            std::string_view target, many_t<M>,
            const std::array<TK,N>& cf, std::size_t cc,
            std::array<TK,N>& arr, std::size_t idx)
        {
            // Loop-back: each M can be followed by another M → FOLLOW(M) ⊇ FIRST(M).
            if constexpr (requires { M::name_sv; }) {
                if (M::name_sv == target)
                    idx = fill_first_for(M{}, arr, idx);
            }
            return fill_follow_for_name(target, M{}, cf, cc, arr, idx);
        }

        template <class M, class TK, std::size_t N>
        consteval std::size_t fill_follow_for_name(
            std::string_view target, many1_t<M>,
            const std::array<TK,N>& cf, std::size_t cc,
            std::array<TK,N>& arr, std::size_t idx)
        {
            if constexpr (requires { M::name_sv; }) {
                if (M::name_sv == target)
                    idx = fill_first_for(M{}, arr, idx);
            }
            return fill_follow_for_name(target, M{}, cf, cc, arr, idx);
        }

        template <class M, class TK, std::size_t N>
        consteval std::size_t fill_follow_for_name(
            std::string_view target, opt_t<M>,
            const std::array<TK,N>& cf, std::size_t cc,
            std::array<TK,N>& arr, std::size_t idx)
        { return fill_follow_for_name(target, M{}, cf, cc, arr, idx); }

        template <class A, class Sep, class TK, std::size_t N>
        consteval std::size_t fill_follow_for_name(
            std::string_view target, sep_by_t<A,Sep>,
            const std::array<TK,N>& cf, std::size_t cc,
            std::array<TK,N>& arr, std::size_t idx)
        {
            idx = fill_follow_for_name(target, A{},   cf, cc, arr, idx);
            return fill_follow_for_name(target, Sep{}, cf, cc, arr, idx);
        }

        template <class A, class Sep, class TK, std::size_t N>
        consteval std::size_t fill_follow_for_name(
            std::string_view target, sep_by1_t<A,Sep>,
            const std::array<TK,N>& cf, std::size_t cc,
            std::array<TK,N>& arr, std::size_t idx)
        { return fill_follow_for_name(target, sep_by_t<A,Sep>{}, cf, cc, arr, idx); }

        template <akshara::fixed_string Name, class Pattern, class TK, std::size_t N>
        consteval std::size_t fill_follow_for_name(
            std::string_view target, rule<Name,Pattern>,
            const std::array<TK,N>& cf, std::size_t cc,
            std::array<TK,N>& arr, std::size_t idx)
        { return fill_follow_for_name(target, Pattern{}, cf, cc, arr, idx); }

        template <auto Kind, class Pattern, class TK, std::size_t N>
        consteval std::size_t fill_follow_for_name(
            std::string_view target, node_t<Kind,Pattern>,
            const std::array<TK,N>& cf, std::size_t cc,
            std::array<TK,N>& arr, std::size_t idx)
        { return fill_follow_for_name(target, Pattern{}, cf, cc, arr, idx); }

        template <class Pattern, class Recovery, class TK, std::size_t N>
        consteval std::size_t fill_follow_for_name(
            std::string_view target, recover_with<Pattern,Recovery>,
            const std::array<TK,N>& cf, std::size_t cc,
            std::array<TK,N>& arr, std::size_t idx)
        { return fill_follow_for_name(target, Pattern{}, cf, cc, arr, idx); }

        // upper bound for FOLLOW buffer: sum of FIRST counts across all rule bodies + 1 for EOF.
        template <class G>
        struct follow_upper_bound_for_grammar {
            using TK = typename G::token_kind;
            static consteval std::size_t count() {
                std::size_t n = 0;
                meta::for_each<typename G::rules>([&n](auto rule_inst) {
                    using Rule = std::remove_cvref_t<decltype(rule_inst)>;
                    n += first_count<typename Rule::pattern_type, TK>::value;
                });
                return n > 0 ? n : 1;
            }
            static constexpr std::size_t value = count();
        };

    } // namespace detail

    // -------------------------------------------------------------------------
    // expected_at<Rule,TK>() — consteval FIRST tokens for a rule.
    // Returns std::array<TK,N> sorted and deduplicated.
    // -------------------------------------------------------------------------

    template <class Rule, class TokenKind>
    [[nodiscard]] consteval auto expected_at() {
        using Pat = typename Rule::pattern_type;
        constexpr std::size_t N = detail::first_count<Pat,TokenKind>::value;
        if constexpr (N == 0) {
            return std::array<TokenKind,0>{};
        } else {
            std::array<TokenKind,N> arr{};
            std::size_t filled = detail::fill_first_for(Pat{}, arr, std::size_t{0});
            std::sort(arr.begin(), arr.begin() + static_cast<std::ptrdiff_t>(filled),
                [](TokenKind a, TokenKind b) {
                    using U = std::underlying_type_t<TokenKind>;
                    return static_cast<U>(a) < static_cast<U>(b);
                });
            filled = detail::dedup_sorted(arr, filled);
            for (std::size_t i = filled; i < N; ++i) arr[i] = TokenKind{};
            return arr;
        }
    }

    // -------------------------------------------------------------------------
    // rule_first_descriptor — per-rule name + index + nullable flag.
    // -------------------------------------------------------------------------

    struct rule_first_descriptor {
        std::string_view name;
        std::size_t      index    = 0;
        bool             nullable = false;
    };

    // -------------------------------------------------------------------------
    // grammar_first_sets<G> — per-rule nullability + name descriptors.
    // Per-rule FIRST token arrays: use expected_at<Rule,TK>() directly.
    // -------------------------------------------------------------------------

    template <class G>
    struct grammar_first_sets {
        static constexpr std::size_t rule_count = G::rule_count;

        static constexpr auto descriptors = [](){
            constexpr std::size_t N = G::rule_count;
            std::array<rule_first_descriptor, N> arr{};
            meta::for_each_index<typename G::rules>([&arr](auto idx, auto rule_instance) {
                using Rule = std::remove_cvref_t<decltype(rule_instance)>;
                arr[idx.value] = rule_first_descriptor{
                    Rule::name_sv, idx.value, nullable_v<typename Rule::pattern_type>
                };
            });
            return arr;
        }();
    };

    template <class G>
    [[nodiscard]] consteval grammar_first_sets<G> first_sets() { return {}; }

    // -------------------------------------------------------------------------
    // grammar_follow_sets<G> — per-rule FOLLOW token sets.
    //
    // Algorithm (fixed-point iteration):
    //   1. FOLLOW(root) = {EOF sentinel (TK{})}.
    //   2. For each rule body seq(..., R, cont, ...):
    //        FOLLOW(R) ⊇ FIRST(cont)
    //        If cont is all-nullable: FOLLOW(R) ⊇ FOLLOW(caller).
    //   3. Iterate until no FOLLOW set grows (fixed-point).
    //   Result: conservative superset of the true FOLLOW set that is correct for
    //   recursive and mutually recursive grammars.
    // -------------------------------------------------------------------------

    template <class G>
    struct grammar_follow_sets {
        using TK = typename G::token_kind;

        struct follow_entry {
            std::string_view name;
            std::size_t      index      = 0;
            bool             has_eof    = false; // true for root rule always

            static constexpr std::size_t kMaxFollow =
                detail::follow_upper_bound_for_grammar<G>::value + 1;

            std::array<TK, kMaxFollow> tokens{};
            std::size_t                token_count = 0;

            constexpr bool add_token(TK t) noexcept {
                for (std::size_t i = 0; i < token_count; ++i)
                    if (tokens[i] == t) return false; // already present
                if (token_count < kMaxFollow) { tokens[token_count++] = t; return true; }
                return false; // overflow
            }
        };

        static constexpr std::size_t rule_count = G::rule_count;

        static constexpr auto entries = [](){
            std::array<follow_entry, G::rule_count> arr{};

            // Initialize names and indices.
            meta::for_each_index<typename G::rules>([&arr](auto idx, auto rule_inst) {
                using Rule = std::remove_cvref_t<decltype(rule_inst)>;
                arr[idx.value].name  = Rule::name_sv;
                arr[idx.value].index = idx.value;
            });

            // Root (index 0) always has EOF in FOLLOW.
            arr[0].has_eof = true;
            arr[0].add_token(TK{});

            // Fixed-point: keep iterating until no FOLLOW set grows.
            bool changed = true;
            while (changed) {
                changed = false;

                // For each caller rule, scan its body for rule<R> in seq context.
                meta::for_each_index<typename G::rules>([&arr, &changed](auto caller_idx, auto caller_inst) {
                    using CallerRule = std::remove_cvref_t<decltype(caller_inst)>;
                    using Pat        = typename CallerRule::pattern_type;

                    // Caller's current FOLLOW set (for nullable-tail propagation).
                    const auto& caller_entry = arr[caller_idx.value];
                    constexpr std::size_t kBuf = follow_entry::kMaxFollow;

                    meta::for_each_index<typename G::rules>([&arr, &changed, caller_idx,
                                                              &caller_entry](auto target_idx, auto target_inst) {
                        if constexpr (caller_idx.value == target_idx.value) return;

                        using TargetRule = std::remove_cvref_t<decltype(target_inst)>;
                        std::array<TK, kBuf> buf{};
                        std::size_t count = detail::fill_follow_for_name(
                            TargetRule::name_sv, Pat{},
                            caller_entry.tokens, caller_entry.token_count,
                            buf, std::size_t{0});

                        for (std::size_t i = 0; i < count; ++i)
                            if (arr[target_idx.value].add_token(buf[i]))
                                changed = true;

                        // If caller has EOF, propagate to any target that appears in
                        // a nullable-tail context (already handled via caller_follow above,
                        // but also propagate has_eof flag).
                        if (caller_entry.has_eof) {
                            // Check if target appears at end of caller (filled via TK{} in buf).
                            // has_eof is set separately below.
                        }
                    });
                });

                // Propagate has_eof: if target rule's FOLLOW contains TK{} (EOF sentinel),
                // mark has_eof = true.
                meta::for_each_index<typename G::rules>([&arr, &changed](auto idx, auto) {
                    if (!arr[idx.value].has_eof) {
                        for (std::size_t k = 0; k < arr[idx.value].token_count; ++k) {
                            if (arr[idx.value].tokens[k] == TK{}) {
                                arr[idx.value].has_eof = true;
                                changed = true;
                                break;
                            }
                        }
                    }
                });
            }

            return arr;
        }();
    };

    template <class G>
    [[nodiscard]] consteval grammar_follow_sets<G> follow_sets() { return {}; }

    // ---- follow_of<G, Rule>() -----------------------------------------------
    // Returns the FOLLOW set for Rule as a sorted+deduped std::array<TK, MaxTok>.
    // For the root rule: also includes the EOF sentinel (TK{}).
    // Always returns the same array size (MaxTok = kMaxFollow + 1) for consistent
    // auto deduction. Unused slots are zeroed (TK{}).

    template <class G, class Rule>
    [[nodiscard]] consteval auto follow_of() {
        using TK = typename G::token_kind;
        constexpr std::size_t MaxTok =
            grammar_follow_sets<G>::follow_entry::kMaxFollow + 1;

        std::array<TK, MaxTok> arr{};
        std::size_t count = 0;

        constexpr auto& all_entries = grammar_follow_sets<G>::entries;
        std::size_t found_idx = all_entries.size();
        for (std::size_t i = 0; i < all_entries.size(); ++i)
            if (all_entries[i].name == Rule::name_sv) { found_idx = i; break; }

        if (found_idx < all_entries.size()) {
            const auto& entry = all_entries[found_idx];
            count = entry.token_count;
            for (std::size_t i = 0; i < count; ++i) arr[i] = entry.tokens[i];
            if (entry.has_eof && count < MaxTok) arr[count++] = TK{};
        } else {
            // Rule not found — return just EOF sentinel.
            arr[count++] = TK{};
        }

        std::sort(arr.begin(), arr.begin() + static_cast<std::ptrdiff_t>(count),
            [](TK a, TK b) {
                using U = std::underlying_type_t<TK>;
                return static_cast<U>(a) < static_cast<U>(b);
            });
        count = detail::dedup_sorted(arr, count);
        for (std::size_t i = count; i < MaxTok; ++i) arr[i] = TK{};
        return arr;
    }

    // ---- expected_after<G, Rule>() ------------------------------------------
    // Returns FIRST(Rule) ∪ FOLLOW(Rule): complete set of tokens expected
    // at and after Rule in any context. Useful for recovery sync sets.

    template <class G, class Rule>
    [[nodiscard]] consteval auto expected_after() {
        using TK = typename G::token_kind;
        constexpr auto first  = expected_at<Rule, TK>();
        constexpr auto follow = follow_of<G, Rule>();

        constexpr std::size_t NFirst  = first.size();
        constexpr std::size_t NFollow = follow.size();
        constexpr std::size_t NTotal  = NFirst + NFollow;

        if constexpr (NTotal == 0) {
            return std::array<TK, 0>{};
        } else {
            std::array<TK, NTotal> arr{};
            std::size_t count = 0;
            for (std::size_t i = 0; i < NFirst;  ++i) arr[count++] = first[i];
            for (std::size_t i = 0; i < NFollow; ++i) arr[count++] = follow[i];
            std::sort(arr.begin(), arr.begin() + static_cast<std::ptrdiff_t>(count),
                [](TK a, TK b) {
                    using U = std::underlying_type_t<TK>;
                    return static_cast<U>(a) < static_cast<U>(b);
                });
            count = detail::dedup_sorted(arr, count);
            for (std::size_t i = count; i < NTotal; ++i) arr[i] = TK{};
            return arr;
        }
    }

} // namespace lang::samasa
