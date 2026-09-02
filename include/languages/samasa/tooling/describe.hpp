#pragma once

// samasa/tooling/describe.hpp — Grammar description: structured descriptor and text summary.
//
// C++23, header-only, no virtual, no macros. Namespace: lang::samasa
//
// grammar_rule_descriptor — name + pattern type name for one rule (static storage).
// grammar_description<G>  — compile-time structured descriptor for grammar G.
// describe<G>()           — consteval; returns grammar_description<G>.
// describe_text<G>()      — consteval; returns akshara fixed_string summary (if computable).
//
// describe<G>() returns grammar_description<G> with static storage — no dangling references.
// Tooling that needs a text form should use describe_text<G>() or format grammar_description.

#include "../grammar/grammar.hpp"
#include "meta/akshara.hpp"
#include "meta/meta.hpp"

namespace lang::samasa {
    // ---- grammar_rule_descriptor ----------------------------------------

    struct grammar_rule_descriptor {
        std::string_view name; // references rule::name_sv static storage
        std::size_t index = 0; // 0-based position in the grammar rule list
    };

    // ---- grammar_description<G> -----------------------------------------

    template <class G>
    struct grammar_description {
        // Grammar name: use root rule name as grammar identifier.
        static constexpr std::string_view name = G::root_rule::name_sv;

        // Rule descriptors: one per rule in G::rules TypeList.
        // Built as a fixed array of grammar_rule_descriptor pointing to static rule names.
        static constexpr auto rules = []() {
            constexpr std::size_t N = G::rules::size;
            std::array<grammar_rule_descriptor, N> arr{};
            meta::for_each_index < typename G::rules > ([&arr](auto idx, auto rule_instance) {
                using Rule = std::remove_cvref_t<decltype(rule_instance)>;
                arr[idx.value] = grammar_rule_descriptor{Rule::name_sv, idx.value};
            });
            return arr;
        }();

        static constexpr std::size_t rule_count = G::rule_count;
    };

    // ---- describe<G>() --------------------------------------------------

    template <class G>
    [[nodiscard]] consteval grammar_description<G> describe() {
        return {};
    }

    // ---- describe_text<G>() ---------------------------------------------
    // Returns a fixed_string summary of grammar G (rule names, one per line).
    // Uses a fixed capacity of 2048; if the grammar is large, use grammar_description<G>.
    // Returns a final fixed_string value — not a builder — safe for consteval use.
    //
    // Implementation: two-pass — first compute total length, then fill.

    namespace detail {
        template <class G>
        consteval std::size_t describe_text_length() {
            std::size_t n = 0;
            // "grammar: " + root name + "\n"
            n += 9 + G::root_rule::name_sv.size() + 1;
            meta::for_each < typename G::rules > ([&n](auto rule_instance) {
                using Rule = std::remove_cvref_t<decltype(rule_instance)>;
                n += 8 + Rule::name_sv.size() + 1; // "  rule: " + name + "\n"
            });
            return n;
        }
    } // namespace detail

    template <class G>
    [[nodiscard]] consteval auto describe_text() {
        constexpr std::size_t kLen = detail::describe_text_length<G>();
        constexpr std::size_t kCap = kLen + 1; // +1 for builder internal sizing
        akshara::ct_string_builder < kCap > sb;
        sb.append("grammar: ");
        sb.append(G::root_rule::name_sv);
        sb.append("\n");
        meta::for_each < typename G::rules > ([&sb](auto rule_instance) {
            using Rule = std::remove_cvref_t<decltype(rule_instance)>;
            sb.append("  rule: ");
            sb.append(Rule::name_sv);
            sb.append("\n");
        });
        return sb.template build<kLen>();
    }
} // namespace lang::samasa
