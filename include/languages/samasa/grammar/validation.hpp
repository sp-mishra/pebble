#pragma once

// samasa/grammar/validation.hpp — Compile-time grammar static checks.
//
// C++23, header-only, no virtual, no macros. Namespace: lang::samasa
//
// grammar_diag_code          — 12 validation issue codes (0..11).
// grammar_issue_severity     — error / warning / note.
// grammar_validation_issue   — code + severity + rule name.
// grammar_validation_result  — N-issue array; ok() iff no errors.
// validate_grammar<G>()      — consteval; returns grammar_validation_result.
// grammar_valid<G>()         — pure consteval bool predicate; never hard-errors.
// require_valid_grammar<G>() — hard compile-time enforcement via static_assert.
// operator_table_valid<T>()  — standalone operator-table duplicate check.
//
// v1 checks (structural, fully computed):
//   SAMASA-GRAMMAR-EMPTY-MANY      — many<nullable>                   (error)
//   SAMASA-GRAMMAR-DUPLICATE-RULE  — two rules with same name         (error)
//
// v2 checks (consteval):
//   SAMASA-GRAMMAR-UNREACHABLE-RULE   — rule not reachable from root  (error)
//   SAMASA-GRAMMAR-LEFT-RECURSION     — direct left-recursion         (error)
//   SAMASA-GRAMMAR-DUPLICATE-OP       — operator_table duplicate      (error)
//   SAMASA-GRAMMAR-BAD-PRATT-TABLE    — malformed Pratt table         (error)
//   SAMASA-GRAMMAR-NULLABLE-ROOT      — root rule is nullable         (error)
//   SAMASA-GRAMMAR-CHOICE-SHADOW      — nullable alt shadows later    (error)
//   SAMASA-GRAMMAR-EMPTY-SEP          — sep_by with nullable sep      (error)
//   SAMASA-GRAMMAR-CHOICE-OVERLAP     — FIRST-set overlap in choice   (warning)
//   SAMASA-GRAMMAR-UNKNOWN-REF        — pattern refs undefined rule   (error)
//   SAMASA-GRAMMAR-RECOVERY-NO-PROGRESS — recover_with uses non-progressing recovery (error)

#include "grammar_ir.hpp"
#include "expected_sets.hpp"
#include "meta/meta.hpp"
#include "../dsl/combinators.hpp"
#include <array>
#include <string_view>

namespace lang::samasa {

    enum class grammar_diag_code : std::uint8_t {
        empty_many              = 0,
        duplicate_rule          = 1,
        unreachable_rule        = 2,
        left_recursion          = 3,
        unknown_ref             = 4,
        duplicate_operator      = 5,
        bad_pratt_table         = 6,
        nullable_root           = 7,
        choice_shadowing        = 8,
        empty_separator         = 9,
        choice_first_overlap    = 10, // FIRST-set overlap between alternatives (warning)
        recovery_no_progress    = 11, // recover_with<P,R> where R does not guarantee progress (error)
    };

    enum class grammar_issue_severity : std::uint8_t {
        error   = 0,
        warning = 1,
        note    = 2,
    };

    struct grammar_validation_issue {
        grammar_diag_code      code;
        grammar_issue_severity severity = grammar_issue_severity::error;
        std::string_view       rule; // static storage (rule::name_sv) — safe
    };

    template <std::size_t N>
    struct grammar_validation_result {
        std::array<grammar_validation_issue, N> issues{};
        // ok() = true iff there are no error-severity issues.
        consteval bool ok() const {
            for (std::size_t i = 0; i < N; ++i)
                if (issues[i].severity == grammar_issue_severity::error) return false;
            return true;
        }
        consteval bool has_warnings() const {
            for (std::size_t i = 0; i < N; ++i)
                if (issues[i].severity == grammar_issue_severity::warning) return true;
            return false;
        }
    };

    template <>
    struct grammar_validation_result<0> {
        consteval bool ok()           const { return true; }
        consteval bool has_warnings() const { return false; }
    };

    namespace detail {

        // ---- no_duplicate_rule_names ----------------------------------------

        template <class G>
        struct all_rule_names_unique {
            template <class Rules, std::size_t... Is>
            static consteval bool check(std::index_sequence<Is...>) {
                constexpr std::size_t N = sizeof...(Is);
                if constexpr (N < 2) return true;
                std::array<std::string_view, N> names{{
                    Rules::template element<Is>::name_sv ...
                }};
                for (std::size_t i = 0; i < N; ++i)
                    for (std::size_t j = i + 1; j < N; ++j)
                        if (names[i] == names[j]) return false;
                return true;
            }
            static constexpr bool value = check<typename G::rules>(
                std::make_index_sequence<G::rules::size>{});
        };

        // ---- no_empty_many --------------------------------------------------

        template <class Pattern>
        struct has_empty_many : std::false_type {};

        template <class M>
        struct has_empty_many<many_t<M>> : std::bool_constant<nullable_v<M>> {};

        template <class... Ms>
        struct has_empty_many<seq_t<Ms...>>
            : std::bool_constant<(has_empty_many<Ms>::value || ...)> {};

        template <class... Ms>
        struct has_empty_many<choice_t<Ms...>>
            : std::bool_constant<(has_empty_many<Ms>::value || ...)> {};

        template <class M>
        struct has_empty_many<many1_t<M>> : has_empty_many<M> {};

        template <class M>
        struct has_empty_many<opt_t<M>> : has_empty_many<M> {};

        template <class A, class Sep>
        struct has_empty_many<sep_by_t<A,Sep>>
            : std::bool_constant<has_empty_many<A>::value || has_empty_many<Sep>::value> {};

        template <class A, class Sep>
        struct has_empty_many<sep_by1_t<A,Sep>> : has_empty_many<sep_by_t<A,Sep>> {};

        template <akshara::fixed_string Name, class Pattern>
        struct has_empty_many<rule<Name,Pattern>> : has_empty_many<Pattern> {};

        template <auto Kind, class Pattern>
        struct has_empty_many<node_t<Kind,Pattern>> : has_empty_many<Pattern> {};

        template <class G>
        struct no_empty_many_check {
            template <class Rules>
            struct over_rules;
            template <class... Rules>
            struct over_rules<meta::TypeList<Rules...>> {
                static constexpr bool value =
                    !(has_empty_many<typename Rules::pattern_type>::value || ...);
            };
            static constexpr bool value = over_rules<typename G::rules>::value;
        };

        // ---- nullable_root --------------------------------------------------

        template <class G>
        struct root_nullable_check {
            static constexpr bool nullable =
                nullable_v<typename G::root_rule::pattern_type>;
        };

        // ---- empty_separator ------------------------------------------------

        template <class Pattern>
        struct has_empty_sep : std::false_type {};

        template <class A, class Sep>
        struct has_empty_sep<sep_by_t<A,Sep>> : std::bool_constant<nullable_v<Sep>> {};

        template <class A, class Sep>
        struct has_empty_sep<sep_by1_t<A,Sep>> : std::bool_constant<nullable_v<Sep>> {};

        template <class... Ms>
        struct has_empty_sep<seq_t<Ms...>>
            : std::bool_constant<(has_empty_sep<Ms>::value || ...)> {};

        template <class... Ms>
        struct has_empty_sep<choice_t<Ms...>>
            : std::bool_constant<(has_empty_sep<Ms>::value || ...)> {};

        template <class M>
        struct has_empty_sep<many_t<M>> : has_empty_sep<M> {};

        template <class M>
        struct has_empty_sep<many1_t<M>> : has_empty_sep<M> {};

        template <class M>
        struct has_empty_sep<opt_t<M>> : has_empty_sep<M> {};

        template <akshara::fixed_string Name, class Pattern>
        struct has_empty_sep<rule<Name,Pattern>> : has_empty_sep<Pattern> {};

        template <auto Kind, class Pattern>
        struct has_empty_sep<node_t<Kind,Pattern>> : has_empty_sep<Pattern> {};

        template <class G>
        struct no_empty_sep_check {
            template <class Rules>
            struct over_rules;
            template <class... Rules>
            struct over_rules<meta::TypeList<Rules...>> {
                static constexpr bool value =
                    !(has_empty_sep<typename Rules::pattern_type>::value || ...);
            };
            static constexpr bool value = over_rules<typename G::rules>::value;
        };

        // ---- left_recursion -------------------------------------------------
        // Direct left recursion: the leftmost non-ε position of a rule's pattern
        // references a rule with the same name. Detects immediate (non-mutual) LR.
        // We compare rule names as strings because string_view is not structural.

        // leftmost_rule_name<Pattern>: returns the name_sv of the leftmost rule<>
        // reference in Pattern, or "" if none.
        template <class Pattern>
        struct leftmost_rule_name {
            static constexpr std::string_view value = {};
        };

        template <akshara::fixed_string N, class Pat>
        struct leftmost_rule_name<rule<N, Pat>> {
            static constexpr std::string_view value = static_cast<std::string_view>(N);
        };

        template <class M0, class... Rest>
        struct leftmost_rule_name<seq_t<M0, Rest...>> {
            static constexpr std::string_view value = leftmost_rule_name<M0>::value;
        };

        template <>
        struct leftmost_rule_name<seq_t<>> {
            static constexpr std::string_view value = {};
        };

        template <class... Ms>
        struct leftmost_rule_name<choice_t<Ms...>> {
            // Any alternative may be leftmost (pessimistic).
            // We check each; if any matches the containing rule name it's LR.
            // For the type trait: return first non-empty rule name among choices.
            static consteval std::string_view first_nonempty() {
                std::string_view result{};
                ((result = result.empty() ? leftmost_rule_name<Ms>::value : result), ...);
                return result;
            }
            static constexpr std::string_view value = first_nonempty();
        };

        template <class M>
        struct leftmost_rule_name<many_t<M>> : leftmost_rule_name<M> {};

        template <class M>
        struct leftmost_rule_name<many1_t<M>> : leftmost_rule_name<M> {};

        template <class M>
        struct leftmost_rule_name<opt_t<M>> : leftmost_rule_name<M> {};

        template <auto Kind, class Pat>
        struct leftmost_rule_name<node_t<Kind,Pat>> : leftmost_rule_name<Pat> {};

        // rule_has_left_recursion: true iff pattern's leftmost rule ref has the same name.
        template <class Rule>
        struct rule_has_left_recursion {
            using Pat = typename Rule::pattern_type;
            static constexpr bool value =
                !leftmost_rule_name<Pat>::value.empty() &&
                leftmost_rule_name<Pat>::value == Rule::name_sv;
        };

        template <class G>
        struct no_left_recursion_check {
            template <class Rules>
            struct over_rules;
            template <class... Rules>
            struct over_rules<meta::TypeList<Rules...>> {
                static constexpr bool value =
                    !(rule_has_left_recursion<Rules>::value || ...);
            };
            static constexpr bool value = over_rules<typename G::rules>::value;
        };

        // ---- unreachable_rule -----------------------------------------------
        // Count rule name references in a pattern (depth-first traversal).

        template <class Pattern>
        struct ref_name_count : std::integral_constant<std::size_t, 0> {};

        template <akshara::fixed_string N, class Pat>
        struct ref_name_count<rule<N,Pat>>
            : std::integral_constant<std::size_t, 1 + ref_name_count<Pat>::value> {};

        template <class... Ms>
        struct ref_name_count<seq_t<Ms...>>
            : std::integral_constant<std::size_t, (ref_name_count<Ms>::value + ...)> {};

        template <class... Ms>
        struct ref_name_count<choice_t<Ms...>>
            : std::integral_constant<std::size_t, (ref_name_count<Ms>::value + ...)> {};

        template <class M>
        struct ref_name_count<many_t<M>> : ref_name_count<M> {};
        template <class M>
        struct ref_name_count<many1_t<M>> : ref_name_count<M> {};
        template <class M>
        struct ref_name_count<opt_t<M>> : ref_name_count<M> {};
        template <class A, class Sep>
        struct ref_name_count<sep_by_t<A,Sep>>
            : std::integral_constant<std::size_t,
                ref_name_count<A>::value + ref_name_count<Sep>::value> {};
        template <class A, class Sep>
        struct ref_name_count<sep_by1_t<A,Sep>> : ref_name_count<sep_by_t<A,Sep>> {};
        template <class M>
        struct ref_name_count<lookahead_t<M>> : ref_name_count<M> {};
        template <class M>
        struct ref_name_count<not_followed_by_t<M>> : ref_name_count<M> {};
        template <auto Kind, class Pat>
        struct ref_name_count<node_t<Kind,Pat>> : ref_name_count<Pat> {};

        // fill_ref_names: fills rule names referenced in Pattern into out[].
        template <class Pattern, std::size_t N>
        consteval std::size_t fill_ref_names(std::array<std::string_view,N>& out, std::size_t idx);

        template <class Pattern, std::size_t N>
        consteval std::size_t fill_ref_names(std::array<std::string_view,N>& out, std::size_t idx) {
            (void)out; return idx; // default: no refs
        }

        // Use helper structs for consteval dispatch (avoids function redefinition).
        template <class Pattern>
        struct ref_filler {
            template <std::size_t N>
            static consteval std::size_t fill(std::array<std::string_view,N>&, std::size_t idx) {
                return idx;
            }
        };

        template <akshara::fixed_string Nm, class Pat>
        struct ref_filler<rule<Nm,Pat>> {
            template <std::size_t N>
            static consteval std::size_t fill(std::array<std::string_view,N>& out, std::size_t idx) {
                if (idx < N) out[idx++] = static_cast<std::string_view>(Nm);
                return ref_filler<Pat>::fill(out, idx);
            }
        };

        template <class... Ms>
        struct ref_filler<seq_t<Ms...>> {
            template <std::size_t N>
            static consteval std::size_t fill(std::array<std::string_view,N>& out, std::size_t idx) {
                ((idx = ref_filler<Ms>::fill(out, idx)), ...);
                return idx;
            }
        };

        template <class... Ms>
        struct ref_filler<choice_t<Ms...>> {
            template <std::size_t N>
            static consteval std::size_t fill(std::array<std::string_view,N>& out, std::size_t idx) {
                ((idx = ref_filler<Ms>::fill(out, idx)), ...);
                return idx;
            }
        };

        template <class M>
        struct ref_filler<many_t<M>> : ref_filler<M> {};
        template <class M>
        struct ref_filler<many1_t<M>> : ref_filler<M> {};
        template <class M>
        struct ref_filler<opt_t<M>> : ref_filler<M> {};

        template <class A, class Sep>
        struct ref_filler<sep_by_t<A,Sep>> {
            template <std::size_t N>
            static consteval std::size_t fill(std::array<std::string_view,N>& out, std::size_t idx) {
                idx = ref_filler<A>::fill(out, idx);
                return ref_filler<Sep>::fill(out, idx);
            }
        };
        template <class A, class Sep>
        struct ref_filler<sep_by1_t<A,Sep>> : ref_filler<sep_by_t<A,Sep>> {};

        template <class M>
        struct ref_filler<lookahead_t<M>> : ref_filler<M> {};
        template <class M>
        struct ref_filler<not_followed_by_t<M>> : ref_filler<M> {};
        template <auto Kind, class Pat>
        struct ref_filler<node_t<Kind,Pat>> : ref_filler<Pat> {};

        // all_rules_reachable: every non-root rule must be referenced somewhere.
        template <class G>
        struct all_rules_reachable {
            static consteval bool check() {
                using Rules = typename G::rules;
                constexpr std::size_t NR = Rules::size;
                if constexpr (NR <= 1) return true;

                // Build the union of all rule names referenced in all patterns.
                // Conservative: counts all references, not just valid ones.
                constexpr std::size_t total_refs = [](){
                    std::size_t sum = 0;
                    meta::for_each<Rules>([&sum](auto inst) {
                        using Rule = std::remove_cvref_t<decltype(inst)>;
                        sum += ref_name_count<typename Rule::pattern_type>::value;
                    });
                    return sum;
                }();

                if constexpr (total_refs == 0) {
                    // No cross-references at all; all non-root rules are unreachable.
                    return NR <= 1;
                } else {
                    std::array<std::string_view, total_refs + 1> refs{};
                    std::size_t ri = 0;
                    meta::for_each<Rules>([&refs, &ri](auto inst) {
                        using Rule = std::remove_cvref_t<decltype(inst)>;
                        constexpr std::size_t C =
                            ref_name_count<typename Rule::pattern_type>::value;
                        if constexpr (C > 0)
                            ri = ref_filler<typename Rule::pattern_type>::fill(refs, ri);
                    });

                    bool all_ok = true;
                    meta::for_each<Rules>([&all_ok, &refs, &ri](auto inst) {
                        using Rule = std::remove_cvref_t<decltype(inst)>;
                        if (Rule::name_sv == G::root_rule::name_sv) return;
                        bool found = false;
                        for (std::size_t i = 0; i < ri; ++i)
                            if (refs[i] == Rule::name_sv) { found = true; break; }
                        if (!found) all_ok = false;
                    });
                    return all_ok;
                }
            }
            static constexpr bool value = check();
        };

        // ---- choice_shadowing -----------------------------------------------
        // choice<A, B, ...> where A is nullable: B and later alternatives unreachable.

        template <class Pattern>
        struct has_shadowed_choice : std::false_type {};

        template <class M0, class M1, class... Rest>
        struct has_shadowed_choice<choice_t<M0, M1, Rest...>>
            : std::bool_constant<nullable_v<M0>> {};

        // Single-alternative choice: no shadowing possible.
        template <class M0>
        struct has_shadowed_choice<choice_t<M0>> : std::false_type {};

        template <>
        struct has_shadowed_choice<choice_t<>> : std::false_type {};

        template <class... Ms>
        struct has_shadowed_choice<seq_t<Ms...>>
            : std::bool_constant<(has_shadowed_choice<Ms>::value || ...)> {};

        template <class M>
        struct has_shadowed_choice<many_t<M>> : has_shadowed_choice<M> {};
        template <class M>
        struct has_shadowed_choice<many1_t<M>> : has_shadowed_choice<M> {};
        template <class M>
        struct has_shadowed_choice<opt_t<M>> : has_shadowed_choice<M> {};

        template <class A, class Sep>
        struct has_shadowed_choice<sep_by_t<A,Sep>>
            : std::bool_constant<has_shadowed_choice<A>::value ||
                                  has_shadowed_choice<Sep>::value> {};
        template <class A, class Sep>
        struct has_shadowed_choice<sep_by1_t<A,Sep>>
            : has_shadowed_choice<sep_by_t<A,Sep>> {};

        template <akshara::fixed_string Name, class Pattern>
        struct has_shadowed_choice<rule<Name,Pattern>> : has_shadowed_choice<Pattern> {};

        template <auto Kind, class Pattern>
        struct has_shadowed_choice<node_t<Kind,Pattern>> : has_shadowed_choice<Pattern> {};

        template <class G>
        struct no_choice_shadowing_check {
            template <class Rules>
            struct over_rules;
            template <class... Rules>
            struct over_rules<meta::TypeList<Rules...>> {
                static constexpr bool value =
                    !(has_shadowed_choice<typename Rules::pattern_type>::value || ...);
            };
            static constexpr bool value = over_rules<typename G::rules>::value;
        };

        // ---- operator_table_valid -------------------------------------------

        template <class Table>
        struct operator_table_valid_check {
            static constexpr bool value = true;
        };

        template <template <class...> class TableTpl, class... Ops>
        struct operator_table_valid_check<TableTpl<Ops...>> {
            static consteval bool check() {
                constexpr std::size_t N = sizeof...(Ops);
                if constexpr (N == 0) return true;
                std::array<std::string_view, N> spellings{{
                    static_cast<std::string_view>(Ops::symbol) ...
                }};
                for (std::size_t i = 0; i < N; ++i)
                    for (std::size_t j = i + 1; j < N; ++j)
                        if (spellings[i] == spellings[j]) return false;
                return true;
            }
            static constexpr bool value = check();
        };

        // ---- choice_first_overlap -------------------------------------------
        // Detects choice<A,B,...> where any two alternatives share a FIRST token.
        // Warning-severity: order matters in PEG but overlap is not always a bug.

        // For a choice_t<Ms...>, check if any pair of alternatives (i,j) share a
        // FIRST token. Returns the index of the first alternative that overlaps a
        // previous one, or ~0 if no overlap.
        template <class TK, class... Ms>
        consteval bool choice_alts_overlap(choice_t<Ms...>) {
            constexpr std::size_t NAlt = sizeof...(Ms);
            if constexpr (NAlt < 2) return false;

            constexpr std::size_t kTotal =
                (first_count<Ms, TK>::value + ... + std::size_t{0});
            if constexpr (kTotal == 0) return false;

            // Flat buffer of all FIRST tokens; alt_end[i] = one-past-last index for alt i.
            std::array<TK, kTotal>         flat{};
            std::array<std::size_t, NAlt>  alt_end{};
            std::size_t pos = 0;

            [&]<std::size_t... I>(std::index_sequence<I...>) {
                ([&](){
                    using M = std::tuple_element_t<I, std::tuple<Ms...>>;
                    constexpr std::size_t NI = first_count<M, TK>::value;
                    if constexpr (NI > 0) {
                        std::array<TK, NI> buf{};
                        std::size_t n = fill_first_for(M{}, buf, std::size_t{0});
                        for (std::size_t k = 0; k < n; ++k) flat[pos + k] = buf[k];
                        pos += n;
                    }
                    alt_end[I] = pos;
                }(), ...);
            }(std::make_index_sequence<NAlt>{});

            // alt i covers flat[alt_start(i) .. alt_end[i]).
            auto alt_start = [&](std::size_t i) -> std::size_t {
                return i == 0 ? 0 : alt_end[i - 1];
            };

            for (std::size_t i = 0; i < NAlt; ++i)
                for (std::size_t j = i + 1; j < NAlt; ++j)
                    for (std::size_t a = alt_start(i); a < alt_end[i]; ++a)
                        for (std::size_t b = alt_start(j); b < alt_end[j]; ++b)
                            if (flat[a] == flat[b]) return true;
            return false;
        }

        // Walk all patterns in G and check for choice_first_overlap.
        template <class Pattern, class TK>
        struct has_choice_overlap : std::false_type {};

        template <class... Ms, class TK>
        struct has_choice_overlap<choice_t<Ms...>, TK>
            : std::bool_constant<choice_alts_overlap<TK>(choice_t<Ms...>{})> {};

        template <class... Ms, class TK>
        struct has_choice_overlap<seq_t<Ms...>, TK>
            : std::bool_constant<(has_choice_overlap<Ms, TK>::value || ...)> {};

        template <class M, class TK>
        struct has_choice_overlap<many_t<M>, TK> : has_choice_overlap<M, TK> {};

        template <class M, class TK>
        struct has_choice_overlap<many1_t<M>, TK> : has_choice_overlap<M, TK> {};

        template <class M, class TK>
        struct has_choice_overlap<opt_t<M>, TK> : has_choice_overlap<M, TK> {};

        template <class A, class Sep, class TK>
        struct has_choice_overlap<sep_by_t<A,Sep>, TK>
            : std::bool_constant<has_choice_overlap<A,TK>::value ||
                                  has_choice_overlap<Sep,TK>::value> {};

        template <class A, class Sep, class TK>
        struct has_choice_overlap<sep_by1_t<A,Sep>, TK>
            : has_choice_overlap<sep_by_t<A,Sep>, TK> {};

        template <akshara::fixed_string Name, class Pattern, class TK>
        struct has_choice_overlap<rule<Name,Pattern>, TK>
            : has_choice_overlap<Pattern, TK> {};

        template <auto Kind, class Pattern, class TK>
        struct has_choice_overlap<node_t<Kind,Pattern>, TK>
            : has_choice_overlap<Pattern, TK> {};

        template <class G>
        struct choice_overlap_check {
            using TK = typename G::token_kind;
            template <class Rules>
            struct over_rules;
            template <class... Rules>
            struct over_rules<meta::TypeList<Rules...>> {
                static constexpr bool value =
                    (has_choice_overlap<typename Rules::pattern_type, TK>::value || ...);
            };
            static constexpr bool value = over_rules<typename G::rules>::value;
        };

        // ---- recovery_no_progress -------------------------------------------
        // Detect recover_with<P,R> where R does not guarantee cursor progress.
        // This is an error: a non-progressing recovery inside recover_with can loop.

        template <class Pattern>
        struct has_no_progress_recovery : std::false_type {};

        template <class P, class R>
        struct has_no_progress_recovery<recover_with<P,R>>
            : std::bool_constant<!recovery_makes_progress_v<R>> {};

        template <class... Ms>
        struct has_no_progress_recovery<seq_t<Ms...>>
            : std::bool_constant<(has_no_progress_recovery<Ms>::value || ...)> {};

        template <class... Ms>
        struct has_no_progress_recovery<choice_t<Ms...>>
            : std::bool_constant<(has_no_progress_recovery<Ms>::value || ...)> {};

        template <class M>
        struct has_no_progress_recovery<many_t<M>> : has_no_progress_recovery<M> {};

        template <class M>
        struct has_no_progress_recovery<many1_t<M>> : has_no_progress_recovery<M> {};

        template <class M>
        struct has_no_progress_recovery<opt_t<M>> : has_no_progress_recovery<M> {};

        template <class A, class Sep>
        struct has_no_progress_recovery<sep_by_t<A,Sep>>
            : std::bool_constant<has_no_progress_recovery<A>::value ||
                                  has_no_progress_recovery<Sep>::value> {};

        template <class A, class Sep>
        struct has_no_progress_recovery<sep_by1_t<A,Sep>>
            : has_no_progress_recovery<sep_by_t<A,Sep>> {};

        template <akshara::fixed_string Name, class Pattern>
        struct has_no_progress_recovery<rule<Name,Pattern>>
            : has_no_progress_recovery<Pattern> {};

        template <auto Kind, class Pattern>
        struct has_no_progress_recovery<node_t<Kind,Pattern>>
            : has_no_progress_recovery<Pattern> {};

        template <class G>
        struct no_recovery_no_progress_check {
            template <class Rules>
            struct over_rules;
            template <class... Rules>
            struct over_rules<meta::TypeList<Rules...>> {
                static constexpr bool value =
                    !(has_no_progress_recovery<typename Rules::pattern_type>::value || ...);
            };
            static constexpr bool value = over_rules<typename G::rules>::value;
        };

    } // namespace detail

    // -------------------------------------------------------------------------
    // validate_grammar<G>()
    // -------------------------------------------------------------------------

    template <class G>
    consteval auto validate_grammar() {
        constexpr bool dup_ok     = detail::all_rule_names_unique<G>::value;
        constexpr bool many_ok    = detail::no_empty_many_check<G>::value;
        constexpr bool sep_ok     = detail::no_empty_sep_check<G>::value;
        constexpr bool lrec_ok    = detail::no_left_recursion_check<G>::value;
        constexpr bool reach_ok   = detail::all_rules_reachable<G>::value;
        constexpr bool shadow_ok  = detail::no_choice_shadowing_check<G>::value;
        constexpr bool nroot_ok   = !detail::root_nullable_check<G>::nullable;
        constexpr bool overlap_ok = !detail::choice_overlap_check<G>::value; // warning only
        constexpr bool recov_ok   = detail::no_recovery_no_progress_check<G>::value;

        constexpr std::size_t N =
            (dup_ok    ? 0 : 1) + (many_ok   ? 0 : 1) + (sep_ok    ? 0 : 1) +
            (lrec_ok   ? 0 : 1) + (reach_ok  ? 0 : 1) + (shadow_ok ? 0 : 1) +
            (nroot_ok  ? 0 : 1) + (overlap_ok? 0 : 1) + (recov_ok  ? 0 : 1);

        if constexpr (N == 0) {
            return grammar_validation_result<0>{};
        } else {
            grammar_validation_result<N> res{};
            std::size_t i = 0;
            using S = grammar_issue_severity;
            if (!dup_ok)
                res.issues[i++] = {grammar_diag_code::duplicate_rule,         S::error,   G::root_rule::name_sv};
            if (!many_ok)
                res.issues[i++] = {grammar_diag_code::empty_many,             S::error,   G::root_rule::name_sv};
            if (!sep_ok)
                res.issues[i++] = {grammar_diag_code::empty_separator,        S::error,   G::root_rule::name_sv};
            if (!lrec_ok)
                res.issues[i++] = {grammar_diag_code::left_recursion,         S::error,   G::root_rule::name_sv};
            if (!reach_ok)
                res.issues[i++] = {grammar_diag_code::unreachable_rule,       S::error,   G::root_rule::name_sv};
            if (!shadow_ok)
                res.issues[i++] = {grammar_diag_code::choice_shadowing,       S::error,   G::root_rule::name_sv};
            if (!nroot_ok)
                res.issues[i++] = {grammar_diag_code::nullable_root,          S::error,   G::root_rule::name_sv};
            if (!overlap_ok)
                res.issues[i++] = {grammar_diag_code::choice_first_overlap,   S::warning, G::root_rule::name_sv};
            if (!recov_ok)
                res.issues[i++] = {grammar_diag_code::recovery_no_progress,   S::error,   G::root_rule::name_sv};
            return res;
        }
    }

    // -------------------------------------------------------------------------
    // grammar_valid<G>() — pure consteval predicate, never hard-errors.
    // require_valid_grammar<G>() — hard compile-time enforcement via static_assert.
    // -------------------------------------------------------------------------

    template <class G>
    consteval bool grammar_valid() {
        return validate_grammar<G>().ok();
    }

    template <class G>
    consteval void require_valid_grammar() {
        static_assert(detail::all_rule_names_unique<G>::value,
            "SAMASA-GRAMMAR-DUPLICATE-RULE: two rules share the same name.");

        static_assert(detail::no_empty_many_check<G>::value,
            "SAMASA-GRAMMAR-EMPTY-MANY: many<M> where M is nullable — infinite loop. "
            "Use opt(many1(...)) instead.");

        static_assert(detail::no_empty_sep_check<G>::value,
            "SAMASA-GRAMMAR-EMPTY-SEP: sep_by with nullable separator — infinite loop.");

        static_assert(detail::no_left_recursion_check<G>::value,
            "SAMASA-GRAMMAR-LEFT-RECURSION: direct left recursion detected. "
            "Refactor to right-recursion or use Pratt for expressions.");

        static_assert(detail::all_rules_reachable<G>::value,
            "SAMASA-GRAMMAR-UNREACHABLE-RULE: one or more rules are not reachable from root.");

        static_assert(detail::no_choice_shadowing_check<G>::value,
            "SAMASA-GRAMMAR-CHOICE-SHADOW: a nullable alternative shadows all later alternatives.");

        static_assert(detail::no_recovery_no_progress_check<G>::value,
            "SAMASA-GRAMMAR-RECOVERY-NO-PROGRESS: recover_with<P,R> where R does not guarantee "
            "cursor progress or token insertion. Pair wrap_error_node with a sync or insert strategy.");

        static_assert(grammar_valid<G>(),
            "SAMASA-GRAMMAR-INVALID: grammar failed compile-time validation. "
            "Use validate_grammar<G>() to inspect individual issues.");
    }

    template <class Table>
    consteval bool operator_table_valid() {
        static_assert(detail::operator_table_valid_check<Table>::value,
            "SAMASA-GRAMMAR-DUPLICATE-OP: operator_table has duplicate operator spellings.");
        return detail::operator_table_valid_check<Table>::value;
    }

} // namespace lang::samasa
