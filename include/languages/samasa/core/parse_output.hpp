#pragma once

// samasa/core/parse_output.hpp — Parse result bundles and allocation policies.
//
// C++23, header-only, no virtual, no macros. Namespace: lang::samasa
//
// Allocation policies (type tags — zero storage, zero overhead):
//   fixed_buffer_policy   — std::array-backed, capacity-checked (consteval use).
//   arena_buffer_policy   — bump arena owned by caller.
//   dynamic_buffer_policy — std::vector/std::pmr (runtime default).
//
// parse_output<SK,TK>  — runtime, heap/arena backed; owns tree + tokens + diagnostics + stats.
// static_parse_output<SK,TK,MaxTokens,MaxEvents,MaxDiags> — consteval, fixed-capacity.
//   events[] element type is static_parse_event<SK> — full event log with structure.
//   event_count tracks number of emitted events.
//
// Fixed-capacity buffer types for consteval parse_static:
//   static_token_buffer<TK, MaxTokens, MaxTrivia>  — fixed-capacity token + trivia store.
//   static_event_stream<SK, MaxEvents>             — alias of lang::static_event_buffer.
//   static_diagnostic_sink<MaxDiags>               — fixed-capacity diagnostic collector.
//
// parse_options — runtime parse configuration.
//   preserve_trivia  — retain whitespace/comment trivia in tree (default: true).
//   build_red_tree   — build red_tree in parse_output (default: false, lazy by default).
//                      Call red_tree<SK>::build(output.tree) explicitly when needed.
//   budget           — node/depth/repair limits.
//
// No heap allocation inside matcher execution, cursor movement, combinator dispatch,
// or Pratt recursion. Buffer growth happens only at policy-owned boundaries.

#include "../tree/green_tree.hpp"
#include "../tree/event_stream.hpp"
#include "../lex/token_stream.hpp"
#include "diagnostic.hpp"
#include "languages/generic/core/diagnostics.hpp"
#include "languages/generic/core/parse_stats.hpp"
#include "languages/generic/tree/static_buffers.hpp"
#include "containers/static/static_vector.hpp"
#include "limits.hpp"

namespace lang::samasa {

    // ---- Allocation policy tags --------------------------------------------

    struct fixed_buffer_policy  {};   // std::array-backed, capacity-checked
    struct arena_buffer_policy  {};   // bump arena owned by caller
    struct dynamic_buffer_policy {};  // std::vector/pmr (runtime default)

    // ---- parse_options -----------------------------------------------------

    struct parse_options {
        limits budget;
        bool   preserve_trivia = true;
        bool   build_red_tree  = false; // red_tree<SK>::build(output.tree) on demand
    };

    // ---- parse_output<SK,TK> — runtime, heap/arena -------------------------

    template <class SyntaxKind, class TokenKind>
    struct parse_output {
        green_tree<SyntaxKind>              tree;
        token_buffer<TokenKind>             tokens;
        lang::collecting_sink<diagnostic>   diagnostics;
        lang::parse_tree_stats              stats;
        bool                                success = false;
    };

    // ---- static_parse_event<SK> — one record in the compile-time event log --
    // Used by static_parse_output (the consteval result bundle).
    // Separate from lang::parse_event — keeps the consteval event log
    // independent of the diag-code type.

    template <class SyntaxKind>
    struct static_parse_event {
        event_kind    kind        = event_kind::tombstone;
        SyntaxKind    node_kind   = {};         // begin_node / end_node: syntax kind
        std::uint32_t token_index = 0;          // token event: index into token array
        byte_span     span        = {};          // end_node / error: byte span
    };

    // ---- static_token_buffer<TK, MaxTokens, MaxTrivia> ---------------------
    // Fixed-capacity token + trivia store for consteval scan.
    // Backing: containers::static_vector<T,N> — overflow logic lives once in container.

    template <class TK, std::uint32_t MaxTokens, std::uint32_t MaxTrivia = MaxTokens * 2>
    struct static_token_buffer {
        containers::static_vector<token<TK>, MaxTokens> tokens;
        containers::static_vector<trivia,    MaxTrivia>  trivia_entries;

        [[nodiscard]] constexpr bool push_token(token<TK> t) noexcept {
            return tokens.push_back(t);
        }

        [[nodiscard]] constexpr bool push_trivia(trivia tv) noexcept {
            return trivia_entries.push_back(tv);
        }

        [[nodiscard]] constexpr std::uint32_t size() const noexcept {
            return static_cast<std::uint32_t>(tokens.size());
        }
        [[nodiscard]] constexpr bool overflow() const noexcept {
            return tokens.overflow() || trivia_entries.overflow();
        }
    };

    // ---- static_event_stream<SK, MaxEvents> --------------------------------
    // Fixed-capacity event log for consteval parse.
    // Stage 3: alias of lang::static_event_buffer<SK, samasa_diag_code, MaxEvents>.
    // Same marker/rollback/tombstone API; backing and overflow logic live in generic.

    template <class SK, std::uint32_t MaxEvents>
    using static_event_stream = lang::static_event_buffer<SK, samasa_diag_code, MaxEvents>;

    // ---- static_diagnostic_sink<MaxDiags> ----------------------------------
    // Fixed-capacity diagnostic collector for consteval parse.
    // Backing: containers::static_vector<diagnostic, MaxDiags>.

    template <std::uint32_t MaxDiags>
    struct static_diagnostic_sink {
        containers::static_vector<diagnostic, MaxDiags> diagnostics;

        constexpr void report(diagnostic d) noexcept {
            static_cast<void>(diagnostics.push_back(d));
        }

        constexpr void truncate(std::uint32_t new_count) noexcept {
            if (new_count < static_cast<std::uint32_t>(diagnostics.size())) {
                containers::static_vector<diagnostic, MaxDiags> tmp;
                for (std::uint32_t i = 0; i < new_count; ++i)
                    static_cast<void>(tmp.push_back(diagnostics[i]));
                diagnostics = tmp;
            }
        }

        [[nodiscard]] constexpr std::uint32_t size() const noexcept {
            return static_cast<std::uint32_t>(diagnostics.size());
        }
        [[nodiscard]] constexpr bool overflow() const noexcept { return diagnostics.overflow(); }
    };

    // ---- static_parse_output<SK,TK,...> — consteval, fixed capacity --------
    // Used by parse_static<G,Src>() for compile-time parsing.
    // Overflow at compile time sets success=false; never a hard compile error
    // unless the caller opts in with static_assert(out.success).
    // parse_static<G,Src>() is defined in samasa.hpp (the umbrella header).

    template <class SyntaxKind, class TokenKind,
              std::uint32_t MaxTokens = 4096,
              std::uint32_t MaxEvents = 4096,
              std::uint32_t MaxDiags  = 256>
    struct static_parse_output {
        std::array<token<TokenKind>,                MaxTokens> tokens{};
        std::uint32_t                                          token_count = 0;
        // Full CST event log (begin/end/token/error). Canonical name going forward.
        // Template parameter MaxEvents controls array capacity.
        std::array<static_parse_event<SyntaxKind>, MaxEvents> events{};
        std::uint32_t                                          event_count = 0;
        // Diagnostics
        std::array<diagnostic,                     MaxDiags>  diagnostics{};
        std::uint32_t                                          diag_count  = 0;
        bool                                                   success = false;
    };

} // namespace lang::samasa
