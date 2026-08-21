#pragma once

// samasa/dsl/primitive.hpp — Terminal matchers.
//
// C++23, header-only, no virtual, no macros. Namespace: lang::samasa
//
// char_lit<fixed_string>      — match fixed char sequence in a char stream.
// char_in<ct_char_set>        — match single char belonging to a charset.
// tok<Kind>                   — match a token of the given TokenKind.
// token_text<fixed_string>    — match token whose source spelling equals S.
// contextual_keyword<Word>    — match identifier token whose text equals Word.
// eof                         — match end-of-stream.
//
// Stream concepts separate char-stream matchers from token-stream matchers:
//   char_stream_like<S>  — S exposes peek_char(uint32_t)
//   token_stream_like<S> — S has token_kind and exposes peek_token(uint32_t)
//
// All are empty structs — zero storage, pure type-level composition.

#include <string_view>
#include "matcher.hpp"
#include "../core/result.hpp"
#include "../core/diagnostic.hpp"
#include "meta/akshara.hpp"

namespace lang::samasa {

    // -------------------------------------------------------------------------
    // Stream concepts
    // -------------------------------------------------------------------------

    template <class Stream>
    concept char_stream_like = requires(Stream s) {
        { s.peek_char(std::uint32_t{}) };
    };

    template <class Stream>
    concept token_stream_like = requires(Stream s) {
        typename Stream::token_kind;
        { s.peek_token(std::uint32_t{}) };
    };

    // -------------------------------------------------------------------------
    // tok<Kind> — match a single token of type Kind
    // -------------------------------------------------------------------------

    template <auto Kind>
    struct tok {
        static constexpr auto token_kind = Kind;
        template <class Ctx>
        [[nodiscard]] constexpr auto match(Ctx& ctx) const {
            using Stream = std::remove_cvref_t<decltype(ctx.stream())>;
            using R      = parse_result<Stream>;
            auto cur = ctx.cursor();
            if (cur.at_end()) return R::soft_failure(cur);
            if (cur.peek().kind == Kind) {
                ctx.events().token(cur.pos);
                ctx.stats().total_tokens++;
                return R::success_at(cur.advance());
            }
            ctx.update_furthest(cur.peek().offset);
            return R::soft_failure(cur);
        }
    };

    // -------------------------------------------------------------------------
    // char_in<Set> — match one char satisfying a ct_char_set (char streams)
    // -------------------------------------------------------------------------

    template <akshara::ct_char_set Set>
    struct char_in {
        template <class Ctx>
        [[nodiscard]] constexpr auto match(Ctx& ctx) const {
            using Stream = std::remove_cvref_t<decltype(ctx.stream())>;
            using R      = parse_result<Stream>;
            auto cur = ctx.cursor();
            if (cur.at_end()) return R::soft_failure(cur);
            const char c = static_cast<char>(cur.peek());
            if (Set.contains(c))
                return R::success_at(cur.advance());
            return R::soft_failure(cur);
        }
    };

    // Convenience char_in instances mirroring akshara charsets.
    inline constexpr char_in<akshara::cs_digits()>      digit{};
    inline constexpr char_in<akshara::cs_alpha()>       alpha{};
    inline constexpr char_in<akshara::cs_alnum()>       alnum{};
    inline constexpr char_in<akshara::cs_whitespace()>  whitespace{};
    inline constexpr char_in<akshara::cs_ident_start()> ident_start{};
    inline constexpr char_in<akshara::cs_ident_cont()>  ident_cont{};

    // -------------------------------------------------------------------------
    // char_lit<S> — match fixed string literal in a char stream
    // (renamed from lit<S> to avoid ambiguity with token-stream matchers)
    // -------------------------------------------------------------------------

    template <akshara::fixed_string S>
    struct char_lit {
        template <class Ctx>
        [[nodiscard]] constexpr auto match(Ctx& ctx) const {
            using Stream = std::remove_cvref_t<decltype(ctx.stream())>;
            using R      = parse_result<Stream>;
            auto cur = ctx.cursor();
            constexpr std::size_t N = S.length;
            for (std::size_t i = 0; i < N; ++i) {
                if (cur.at_end() || static_cast<char>(cur.peek()) != S[i])
                    return R::soft_failure(ctx.cursor());
                cur = cur.advance();
            }
            return R::success_at(cur);
        }
    };

    // -------------------------------------------------------------------------
    // token_text<S> — match token whose source spelling equals S.
    // Compares source.substr(token.offset, token.length) against S.
    // No text stored in the matcher; reads from parse_context::source_text().
    // -------------------------------------------------------------------------

    template <akshara::fixed_string S>
    struct token_text {
        template <class Ctx>
        [[nodiscard]] constexpr auto match(Ctx& ctx) const {
            using Stream = std::remove_cvref_t<decltype(ctx.stream())>;
            using R      = parse_result<Stream>;
            auto cur = ctx.cursor();
            if (cur.at_end()) return R::soft_failure(cur);
            const auto& t = cur.peek();
            if (ctx.source_text(t.offset, t.length) == static_cast<std::string_view>(S)) {
                ctx.events().token(cur.pos);
                return R::success_at(cur.advance());
            }
            return R::soft_failure(cur);
        }
    };

    // -------------------------------------------------------------------------
    // contextual_keyword<Word> — match identifier token with text == Word
    // -------------------------------------------------------------------------

    template <akshara::fixed_string Word>
    struct contextual_keyword {
        template <class Ctx>
        [[nodiscard]] constexpr auto match(Ctx& ctx) const {
            using Stream = std::remove_cvref_t<decltype(ctx.stream())>;
            using R      = parse_result<Stream>;
            auto cur = ctx.cursor();
            if (cur.at_end()) return R::soft_failure(cur);
            const auto& tok_ref = cur.peek();
            if (ctx.source_text(tok_ref.offset, tok_ref.length) == static_cast<std::string_view>(Word)) {
                ctx.events().token(cur.pos);
                return R::success_at(cur.advance());
            }
            return R::soft_failure(cur);
        }
    };

    // -------------------------------------------------------------------------
    // eof — match end of token stream
    // -------------------------------------------------------------------------

    struct eof {
        template <class Ctx>
        [[nodiscard]] constexpr auto match(Ctx& ctx) const {
            using Stream = std::remove_cvref_t<decltype(ctx.stream())>;
            using R      = parse_result<Stream>;
            auto cur = ctx.cursor();
            return cur.at_end() ? R::success_at(cur) : R::soft_failure(cur);
        }
    };

} // namespace lang::samasa
