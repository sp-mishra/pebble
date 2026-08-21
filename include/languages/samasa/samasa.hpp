#pragma once

// samasa/samasa.hpp — Umbrella header for the Samasa grammar/parser framework.
//
// C++23, header-only, no virtual, no macros.
//
// Namespace:  lang::samasa
// Alias:      lang::parser  (convenience)
//
// Usage:
//   #include "languages/samasa/samasa.hpp"
//   using namespace lang::samasa;
//
//   // or via the alias:
//   namespace parser = lang::parser;
//
// Entry points:
//   parse<Grammar>(source, opts)              → parse_output<SK,TK>
//   parse_static<Grammar, fixed_string Src>() → static_parse_output<SK,TK>
//
// All sub-headers are independently includable for pay-for-use.

// Core
#include "core/source_view.hpp"
#include "core/cursor.hpp"
#include "core/result.hpp"
#include "core/diagnostic.hpp"
#include "core/limits.hpp"
#include "core/parse_options.hpp"
#include "core/context.hpp"
#include "core/parse_output.hpp"
#include "core/static_context.hpp"

// Lexer
#include "lex/token.hpp"
#include "lex/token_stream.hpp"
#include "lex/keyword_table.hpp"
#include "lex/operator_trie.hpp"
#include "lex/line_policy.hpp"
#include "lex/scanner.hpp"

// DSL
#include "dsl/matcher.hpp"
#include "dsl/primitive.hpp"
#include "dsl/combinators.hpp"
#include "dsl/rule.hpp"
#include "dsl/node.hpp"

// Grammar
#include "grammar/grammar.hpp"
#include "grammar/grammar_ir.hpp"
#include "grammar/validation.hpp"
#include "grammar/expected_sets.hpp"
#include "grammar/fingerprint.hpp"
#include "grammar/metadata.hpp"

// Tree
#include "tree/event_stream.hpp"
#include "tree/green_tree.hpp"
#include "tree/red_tree.hpp"
#include "tree/incremental.hpp"

// Expression
#include "expr/precedence.hpp"
#include "expr/operator_table.hpp"
#include "expr/pratt.hpp"

// Recovery
#include "recovery/sync_set.hpp"
#include "recovery/recovery.hpp"

// Policies
#include "policies/memo_policy.hpp"
#include "policies/error_policy.hpp"
#include "policies/execution_policy.hpp"
#include "policies/trace_policy.hpp"
#include "policies/profile.hpp"

// Tooling (pay-for-use — include separately if not needed at compile time)
// #include "tooling/describe.hpp"
// #include "tooling/highlight.hpp"
// #include "tooling/railroad.hpp"
// #include "tooling/render_markdown.hpp"
// #include "tooling/render_json.hpp"

// ---- parse<G> entry point ---------------------------------------------------

namespace lang::samasa {

    // Parse without materializing a green/red tree.  This is the low-allocation
    // route for frontends that lower parse events directly into their own IR.
    template <class Grammar, class Sink, class KWTable = keyword_table<>,
              class OpTrie = operator_trie<>,
              class LinePol = no_line_sensitivity<typename Grammar::token_kind>>
    [[nodiscard]] bool parse_events(std::string_view source, Sink&& sink,
                                    const default_parse_options& opts = {},
                                    const scan_token_kinds<typename Grammar::token_kind>& kinds = {},
                                    const LinePol& lp = {}) {
        using SK = typename Grammar::syntax_kind; using TK = typename Grammar::token_kind;
        lang::collecting_sink<diagnostic> diagnostics; auto tokens = scan<KWTable, OpTrie, LinePol, TK>(source,kinds,lp,diagnostics);
        lang::parse_tree_stats stats; event_stream<SK> events;
        parse_context<SK,TK> ctx(tokens.view(),source,events,diagnostics,stats,opts.budget);
        const auto root = events.begin(SK{}); auto result = typename Grammar::root_rule{}.match(ctx);
        if (!result.ok()) { events.rollback(root); return false; }
        events.end(root,{0,static_cast<std::uint32_t>(source.size())});
        for (const auto& event : events.all()) sink(event);
        return !diagnostics.has_errors();
    }

    // parse<G>(source, opts) — scan + parse source text into a parse_output.
    template <class Grammar,
              class KWTable   = keyword_table<>,
              class OpTrie    = operator_trie<>,
              class LinePol   = no_line_sensitivity<typename Grammar::token_kind>,
              class... Policies>
    [[nodiscard]] parse_output<typename Grammar::syntax_kind, typename Grammar::token_kind>
    parse(std::string_view           source,
          const default_parse_options& opts  = {},
          const scan_token_kinds<typename Grammar::token_kind>& tok_kinds = {},
          const LinePol&             lp    = {})
    {
        using SK = typename Grammar::syntax_kind;
        using TK = typename Grammar::token_kind;
        using Out = parse_output<SK, TK>;

        Out output;

        // 1. Scan.
        output.tokens = scan<KWTable, OpTrie, LinePol, TK>(
            source, tok_kinds, lp, output.diagnostics);

        // 2. Parse.
        output.stats.source_bytes = static_cast<std::uint32_t>(source.size());
        output.stats.total_tokens = output.tokens.view().size();

        event_stream<SK>        events;
        parse_context<SK, TK>   ctx(output.tokens.view(), source,
                                    events, output.diagnostics, output.stats,
                                    opts.budget);

        // Run the root rule wrapped in a synthetic root node so green_arena::root_id_
        // is always set — grammars that use rule<> without node_t() produce flat token
        // leaves with no end_node events, leaving root_id_ == k_null_arena.
        using RootRule = typename Grammar::root_rule;
        auto root_mk = events.begin(SK{});
        auto r = RootRule{}.match(ctx);

        if (r.ok()) {
            const byte_span src_span{0, static_cast<std::uint32_t>(source.size())};
            events.end(root_mk, src_span);
            output.tree = build_green<SK>(events, output.tokens.view(), source);
            output.success = !output.diagnostics.has_errors();
        } else {
            events.rollback(root_mk);
            output.success = false;
        }

        return output;
    }

    // parse_static<G, Src, MaxTokens, MaxEvents, MaxDiags>() — consteval parse.
    // Returns a static_parse_output<SK,TK> usable in static_assert / constexpr.
    // Overflow sets success=false; never a hard compile error unless caller adds
    // static_assert(out.success).
    //
    // Scanner configuration (KWTable, OpTrie, LinePol) is supplied as function
    // arguments with defaults so simple grammars need no extra arguments.
    // ScannerPolicy::scan_custom_token is not invoked in the consteval path
    // (it requires runtime callbacks incompatible with consteval evaluation).
    template <class Grammar,
              akshara::fixed_string Src,
              std::uint32_t MaxTokens = 4096,
              std::uint32_t MaxEvents = 4096,
              std::uint32_t MaxDiags  = 256,
              std::uint32_t MaxDepth  = 256,
              class KWTable  = keyword_table<>,
              class OpTrie   = operator_trie<>,
              class LinePol  = no_line_sensitivity<typename Grammar::token_kind>>
    [[nodiscard]] consteval auto parse_static(
        scan_token_kinds<typename Grammar::token_kind> tok_kinds = {},
        LinePol lp = {})
    {
        using SK = typename Grammar::syntax_kind;
        using TK = typename Grammar::token_kind;
        constexpr std::uint32_t MaxTrivia = MaxTokens * 2;

        static_parse_output<SK, TK, MaxTokens, MaxEvents, MaxDiags> out{};

        // ---- Consteval scan -------------------------------------------------
        static_token_buffer<TK, MaxTokens, MaxTrivia> tok_buf{};

        const std::string_view src_sv{Src.data, Src.length};
        const std::uint32_t    N   = static_cast<std::uint32_t>(Src.length);

        // Char sets are consteval-constructed; calling .contains() is valid inside
        // a consteval function because the entire body is a constant evaluation.
        const auto kIdentStart = akshara::cs_ident_start();
        const auto kIdentCont  = akshara::cs_ident_cont();
        const auto kDigits     = akshara::cs_digits();

        auto cs_has = [](const akshara::ct_char_set& s, char c) constexpr noexcept -> bool {
            const unsigned idx = static_cast<unsigned char>(c) & 0x7Fu;
            if (idx < 64) return (s.low  >> idx) & 1u;
            return               (s.high >> (idx - 64)) & 1u;
        };

        std::uint32_t pos       = 0;
        TK            prev_kind = tok_kinds.eof;
        bool          lex_ok    = true;

        while (pos < N) {
            const std::uint32_t start = pos;
            const char c = Src.data[pos];

            // Whitespace
            if (c == ' ' || c == '\t' || c == '\r') {
                while (pos < N && (Src.data[pos] == ' ' || Src.data[pos] == '\t' || Src.data[pos] == '\r'))
                    ++pos;
                if (!tok_buf.push_trivia({trivia_kind::whitespace, {start, pos - start}}))
                    lex_ok = false;
                continue;
            }

            // Newline + line policy synthetic separator
            if (c == '\n') {
                ++pos;
                if (!tok_buf.push_trivia({trivia_kind::newline, {start, 1}}))
                    lex_ok = false;
                if (!lp.line_continues(prev_kind)) {
                    const TK sep = lp.synthetic_separator();
                    if (sep != tok_kinds.eof && !lp.suppress_separator(prev_kind, sep)) {
                        if (!tok_buf.push_token({sep, start, 0, 0, 0})) lex_ok = false;
                        prev_kind = sep;
                    }
                }
                continue;
            }

            // Line comment (//)
            if (c == '/' && pos + 1 < N && Src.data[pos + 1] == '/') {
                while (pos < N && Src.data[pos] != '\n') ++pos;
                if (!tok_buf.push_trivia({trivia_kind::line_comment, {start, pos - start}}))
                    lex_ok = false;
                continue;
            }

            // Block comment (/* ... */)
            if (c == '/' && pos + 1 < N && Src.data[pos + 1] == '*') {
                pos += 2;
                while (pos + 1 < N && !(Src.data[pos] == '*' && Src.data[pos + 1] == '/')) ++pos;
                if (pos + 1 < N) {
                    pos += 2;
                } else {
                    // Unterminated block comment — record lex failure but continue.
                    lex_ok = false;
                }
                if (!tok_buf.push_trivia({trivia_kind::block_comment, {start, pos - start}}))
                    lex_ok = false;
                continue;
            }

            // Identifier / keyword
            if (cs_has(kIdentStart, c)) {
                while (pos < N && cs_has(kIdentCont, Src.data[pos])) ++pos;
                const std::string_view word = src_sv.substr(start, pos - start);
                const auto kw = KWTable::template lookup<TK>(word);
                const TK k = kw.value_or(tok_kinds.identifier);
                if (!tok_buf.push_token({k, start, pos - start, 0, 0})) lex_ok = false;
                prev_kind = k;
                continue;
            }

            // Integer / float literal
            if (cs_has(kDigits, c)) {
                while (pos < N && cs_has(kDigits, Src.data[pos])) ++pos;
                bool is_float = false;
                if (pos < N && Src.data[pos] == '.') {
                    is_float = true; ++pos;
                    while (pos < N && cs_has(kDigits, Src.data[pos])) ++pos;
                }
                if (pos < N && (Src.data[pos] == 'e' || Src.data[pos] == 'E')) {
                    is_float = true; ++pos;
                    if (pos < N && (Src.data[pos] == '+' || Src.data[pos] == '-')) ++pos;
                    while (pos < N && cs_has(kDigits, Src.data[pos])) ++pos;
                }
                const TK k = is_float ? tok_kinds.float_literal : tok_kinds.integer_literal;
                if (!tok_buf.push_token({k, start, pos - start, 0, 0})) lex_ok = false;
                prev_kind = k;
                continue;
            }

            // String literal
            if (c == '"' || c == '\'') {
                const char delim = c; ++pos;
                bool terminated = false;
                while (pos < N) {
                    if (Src.data[pos] == '\\') { pos += 2; continue; }
                    if (Src.data[pos] == delim) { ++pos; terminated = true; break; }
                    ++pos;
                }
                if (!terminated) lex_ok = false;
                if (!tok_buf.push_token({tok_kinds.string_literal, start, pos - start, 0, 0}))
                    lex_ok = false;
                prev_kind = tok_kinds.string_literal;
                continue;
            }

            // Operator (longest match via OpTrie)
            {
                const auto op = OpTrie::template match<TK>(src_sv, pos);
                if (op) {
                    const auto len = static_cast<std::uint32_t>(op->second);
                    if (!tok_buf.push_token({op->first, start, len, 0, 0})) lex_ok = false;
                    prev_kind = op->first;
                    pos += len;
                    continue;
                }
            }

            // Unknown character
            lex_ok = false;
            if (!tok_buf.push_token({tok_kinds.unknown, start, 1, 0, 0})) lex_ok = false;
            prev_kind = tok_kinds.unknown;
            ++pos;
        }

        // EOF token
        if (!tok_buf.push_token({tok_kinds.eof, N, 0, 0, 0})) lex_ok = false;

        if (tok_buf.overflow() || !lex_ok) {
            out.success = false;
            return out;
        }

        // Copy token results into static_parse_output.
        out.token_count = tok_buf.size();
        for (std::uint32_t i = 0; i < tok_buf.size() && i < MaxTokens; ++i)
            out.tokens[i] = tok_buf.tokens[i];

        // Run the grammar at compile time.
        constexpr std::uint32_t MaxTrivia2 = MaxTokens * 2;
        static_parse_context<SK, TK, MaxTokens, MaxTrivia2, MaxEvents, MaxDiags> ctx(
            tok_buf, src_sv, limits{MaxDepth, MaxTokens, MaxDiags});

        using RootRule = typename Grammar::root_rule;
        const auto r = RootRule{}.match(ctx);

        // Copy events out.
        out.event_count = ctx.events().event_count();
        for (std::uint32_t i = 0; i < out.event_count && i < MaxEvents; ++i) {
            const auto& src = ctx.events()[i];
            out.events[i] = {src.kind, src.node_kind, src.token_index, src.span};
        }

        // Copy diagnostics out.
        out.diag_count = ctx.diag_count();
        for (std::uint32_t i = 0; i < out.diag_count && i < MaxDiags; ++i)
            out.diagnostics[i] = ctx.diag(i);

        out.success = r.ok()
                   && !ctx.overflow()
                   && !ctx.has_errors();
        return out;
    }

} // namespace lang::samasa

// Convenience alias.
namespace lang {
    namespace parser = samasa;
} // namespace lang
