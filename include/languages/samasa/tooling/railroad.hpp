#pragma once

// samasa/tooling/railroad.hpp — Structured railroad diagram model.
//
// C++23, header-only, no virtual, no macros. Namespace: lang::samasa
//
// railroad_node_kind   — type tag for a railroad diagram node.
// railroad_node        — one node in a railroad model (kind + name + children).
// railroad_rule_model  — diagram for a single grammar rule.
// railroad_model<G>()  — consteval; returns grammar_description<G> + rule models.
//
// The model is renderer-neutral. Optional renderers in separate headers:
//   tooling/render_markdown.hpp  — Markdown text rendering
//   tooling/render_json.hpp      — JSON rendering
//
// Consumers produce Markdown, DOT, Mermaid, or HTML from the structured model;
// core Samasa does not depend on any rendering format.

#include "describe.hpp"
#include "meta/akshara.hpp"
#include "meta/meta.hpp"
#include "../grammar/grammar_ir.hpp"
#include "../dsl/rule.hpp"
#include "../dsl/combinators.hpp"
#include "../dsl/primitive.hpp"
#include "../dsl/node.hpp"

namespace lang::samasa {

    // ---- railroad_node_kind ------------------------------------------------

    enum class railroad_node_kind : std::uint8_t {
        terminal,     // tok<K> or char_lit or token_text
        rule_ref,     // reference to a named rule
        sequence,     // seq(A, B, ...)
        choice,       // choice(A, B, ...)
        optional,     // opt(A)
        repetition,   // many(A) or many1(A)
        separated,    // sep_by(A, Sep)
        lookahead,    // lookahead(A)
        negative,     // not_followed_by(A)
        group,        // node_t wrapper
        cut_marker,   // cut
        eof_marker,   // eof
    };

    // ---- railroad_text_node ------------------------------------------------
    // Compact, fixed-size node carrying its name as a string_view to static storage.

    struct railroad_text_node {
        railroad_node_kind kind  = railroad_node_kind::terminal;
        std::string_view   label;           // token name / rule name / literal text
        bool               nullable = false;
    };

    // ---- pattern_to_rr_label -----------------------------------------------
    // Produces a single-line human-readable label for a pattern.
    // Used for the railroad model text output.

    namespace rr_detail {

        template <class Pattern>
        consteval std::string_view label_of() { return "<pattern>"; }

        template <auto Kind>
        consteval std::string_view label_of_tok() { return "tok"; }  // generic fallback

        // Helpers for common terminal types.
        template <akshara::fixed_string S>
        consteval std::string_view label_char_lit() {
            return static_cast<std::string_view>(S);
        }

        template <akshara::fixed_string S>
        consteval std::string_view label_token_text() {
            return static_cast<std::string_view>(S);
        }

        template <akshara::fixed_string W>
        consteval std::string_view label_ckw() {
            return static_cast<std::string_view>(W);
        }

    } // namespace rr_detail

    // ---- railroad_rule_entry -----------------------------------------------
    // Describes one rule in the railroad model as a text pattern summary.

    struct railroad_rule_entry {
        std::string_view rule_name;
        std::string_view shape;   // coarse shape tag: "sequence" | "choice" | "repetition" | "terminal" | "optional" | "other"
        bool             nullable = false;
    };

    // Coarse shape from type.
    template <class Pattern>
    consteval std::string_view rr_shape() { return "other"; }

    template <class... Ms>
    consteval std::string_view rr_shape_seq() { return "sequence"; }

    template <class... Ms>
    consteval std::string_view rr_shape_choice() { return "choice"; }

    template <class M>
    consteval std::string_view rr_shape_many() { return "repetition"; }

    template <class M>
    consteval std::string_view rr_shape_opt() { return "optional"; }

    template <auto Kind>
    consteval std::string_view rr_shape_tok() { return "terminal"; }

    template <class Pattern>
    struct rr_shape_of { static constexpr std::string_view value = "other"; };

    template <class... Ms>
    struct rr_shape_of<seq_t<Ms...>> { static constexpr std::string_view value = "sequence"; };

    template <class... Ms>
    struct rr_shape_of<choice_t<Ms...>> { static constexpr std::string_view value = "choice"; };

    template <class M>
    struct rr_shape_of<many_t<M>> { static constexpr std::string_view value = "repetition"; };

    template <class M>
    struct rr_shape_of<many1_t<M>> { static constexpr std::string_view value = "repetition"; };

    template <class M>
    struct rr_shape_of<opt_t<M>> { static constexpr std::string_view value = "optional"; };

    template <auto Kind>
    struct rr_shape_of<tok<Kind>> { static constexpr std::string_view value = "terminal"; };

    template <class A, class Sep>
    struct rr_shape_of<sep_by_t<A,Sep>> { static constexpr std::string_view value = "separated"; };

    template <class A, class Sep>
    struct rr_shape_of<sep_by1_t<A,Sep>> { static constexpr std::string_view value = "separated"; };

    template <akshara::fixed_string Name, class Pattern>
    struct rr_shape_of<rule<Name,Pattern>> : rr_shape_of<Pattern> {};

    template <auto Kind, class Pattern>
    struct rr_shape_of<node_t<Kind,Pattern>> : rr_shape_of<Pattern> {};

    // ---- grammar_railroad_model<G> -----------------------------------------

    template <class G>
    struct grammar_railroad_model {
        static constexpr std::size_t rule_count = G::rule_count;

        static constexpr auto entries = [](){
            constexpr std::size_t N = G::rule_count;
            std::array<railroad_rule_entry, N> arr{};
            meta::for_each_index<typename G::rules>([&arr](auto idx, auto rule_inst) {
                using Rule = std::remove_cvref_t<decltype(rule_inst)>;
                using Pat  = typename Rule::pattern_type;
                arr[idx.value] = railroad_rule_entry{
                    Rule::name_sv,
                    rr_shape_of<Pat>::value,
                    nullable_v<Pat>
                };
            });
            return arr;
        }();
    };

    template <class G>
    [[nodiscard]] consteval grammar_railroad_model<G> railroad_model() { return {}; }

} // namespace lang::samasa
