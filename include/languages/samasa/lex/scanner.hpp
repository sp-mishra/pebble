#pragma once

// samasa/lex/scanner.hpp — Generic scanner driver.
//
// C++23, header-only, no virtual, no macros. Namespace: lang::samasa
//
// scan_token_kinds<TK>              — token kind descriptor passed to scan().
// scanner_policy<TK>                — customization hook: feature flags + custom token hook.
// scan<KWTable,OpTrie,LP,TK,Policy> — scan source text into a token_buffer<TK>.
//
// Scanner loop:
//   1. ScannerPolicy::scan_custom_token — language hook (default returns false).
//   2. Skip trivia (whitespace/newline/comments) → trivia arena.
//   3. cs_ident_start → ident; keyword_table.lookup → keyword|identifier.
//   4. cs_digits      → integer/float literal.
//   5. '"' or '\''    → string literal (track unterminated).
//   6. operator chars → operator_trie longest match.
//   7. else           → emit SAMASA-LEX-UNKNOWN-CHAR; advance 1.
//   8. line_policy    → inject synthetic_separator on statement-terminating newline.
//   9. emit eof token.

#include <cstdint>
#include <string>
#include <string_view>
#include "token_stream.hpp"
#include "keyword_table.hpp"
#include "operator_trie.hpp"
#include "line_policy.hpp"
#include "../core/diagnostic.hpp"
#include "meta/akshara.hpp"
#include "languages/generic/core/diagnostics.hpp"

namespace lang::samasa {
    // Runtime ct_char_set::contains — the member is consteval; reimplement for runtime use.
    [[nodiscard]] inline bool cs_contains_rt(const akshara::ct_char_set& s, char c) noexcept {
        const unsigned idx = static_cast<unsigned char>(c) & 0x7Fu;
        if (idx < 64) return (s.low >> idx) & 1u;
        return (s.high >> (idx - 64)) & 1u;
    }

    // ---- scan_token_kinds<TK> -----------------------------------------------

    template <class TokenKind>
    struct scan_token_kinds {
        TokenKind eof;
        TokenKind identifier;
        TokenKind integer_literal;
        TokenKind float_literal;
        TokenKind string_literal;
        TokenKind unknown;
    };

    // ---- scanner_policy<TK> — language customization hook -------------------
    //
    // Feature flags (compile-time) and scan_custom_token hook (statically dispatched).
    // v1 wires only the ASCII path; Unicode/nested-comment modes are deferred.
    //
    // To add custom token scanning, specialize or derive from this and override
    // scan_custom_token. The hook receives a "scanner view" that exposes
    // source, pos (by ref), and an emit_tok callback. Return true if a custom
    // token was consumed.
    //
    // A minimal scanner-view API is passed to scan_custom_token at runtime:
    //   struct ScannerView {
    //       std::string_view   source;
    //       std::uint32_t&     pos;
    //       auto emit_tok(TokenKind, std::uint32_t off, std::uint32_t len) -> void;
    //       auto emit_trivia(trivia_kind, std::uint32_t off, std::uint32_t len) -> void;
    //       lang::collecting_sink<diagnostic>& sink;
    //       const scan_token_kinds<TokenKind>& kinds;
    //   };

    template <class TokenKind>
    struct scanner_policy {
        static constexpr bool nested_block_comments = false;
        static constexpr bool unicode_identifiers = false;

        template <class ScannerView>
        static bool scan_custom_token([[maybe_unused]] ScannerView& sv) { return false; }
    };

    // ---- unicode_identifier_policy -----------------------------------------
    // Opt-in policy hook for Unicode identifier characters.
    // Set scanner_policy<TK>::unicode_identifiers = true and specialize this to
    // accept non-ASCII ident-start / ident-continue code points.

    struct ascii_identifier_policy {
        [[nodiscard]] static constexpr bool is_ident_start(char32_t c) noexcept {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
        }

        [[nodiscard]] static constexpr bool is_ident_continue(char32_t c) noexcept {
            return is_ident_start(c) || (c >= '0' && c <= '9');
        }
    };

    // unicode_identifier_policy: override to accept full Unicode categories.
    // Users specialize this for their scanner_policy<TK>.
    template <class TokenKind>
    struct unicode_identifier_policy : ascii_identifier_policy {};

    // ---- scanner_mode_id ---------------------------------------------------
    // Opaque mode identifier for scanner mode stack (v2.2 contextual lexing).
    // Language-specific mode values are defined by the language frontend.

    using scanner_mode_id = std::uint16_t;
    inline constexpr scanner_mode_id k_scanner_mode_default = 0;

    // ---- scanner_mode_stack ------------------------------------------------
    // Opt-in scanner mode stack for contextual lexing.
    // Used by scanner_policy::scan_custom_token to push/pop modes (e.g., raw strings,
    // template interpolation, indentation modes, foreign code blocks).

    class scanner_mode_stack {
    public:
        static constexpr std::size_t kMaxDepth = 32;

        [[nodiscard]] scanner_mode_id mode() const noexcept {
            return depth_ > 0 ? stack_[depth_ - 1] : k_scanner_mode_default;
        }

        void push_mode(scanner_mode_id m) noexcept {
            if (depth_ < kMaxDepth) stack_[depth_++] = m;
        }

        void pop_mode() noexcept {
            if (depth_ > 0) --depth_;
        }

        [[nodiscard]] std::size_t depth() const noexcept { return depth_; }

    private:
        std::array<scanner_mode_id, kMaxDepth> stack_{};
        std::size_t depth_ = 0;
    };

    // ---- Internal scanner view passed to scanner_policy::scan_custom_token ---

    template <class TokenKind, class EmitTok, class EmitTrivia>
    struct scanner_view {
        std::string_view source;
        std::uint32_t& pos;
        EmitTok& emit_tok;
        EmitTrivia& emit_trivia;
        lang::collecting_sink<diagnostic>& sink;
        const scan_token_kinds<TokenKind>& kinds;
    };

    // ---- scan<KWTable, OpTrie, LinePolicyT, TokenKind, ScannerPolicy> -------

    template <class KWTable, class OpTrie, class LinePolicyT, class TokenKind,
              class ScannerPolicy = scanner_policy<TokenKind>>
    [[nodiscard]] token_buffer<TokenKind> scan(
        std::string_view source,
        const scan_token_kinds<TokenKind>& kinds,
        const LinePolicyT& lp,
        ::lang::collecting_sink<diagnostic>& sink,
        [[maybe_unused]] const ScannerPolicy& policy = {}) {
        token_buffer<TokenKind> buf;
        buf.data.reserve(source.size() / 4 + 8);
        buf.trivia_arena.reserve(source.size() / 8 + 4);

        auto emit_tok = [&](TokenKind k, std::uint32_t off, std::uint32_t len) {
            buf.data.push_back({k, off, len, 0, 0});
        };
        auto emit_trivia_fn = [&](trivia_kind tk, std::uint32_t off, std::uint32_t len) {
            buf.trivia_arena.push_back({tk, {off, len}});
        };

        static constexpr akshara::ct_char_set kIdentStart = akshara::cs_ident_start();
        static constexpr akshara::ct_char_set kIdentCont = akshara::cs_ident_cont();
        static constexpr akshara::ct_char_set kDigits = akshara::cs_digits();

        const std::uint32_t N = static_cast<std::uint32_t>(source.size());
        std::uint32_t pos = 0;
        TokenKind prev_kind = kinds.eof;

        while (pos < N) {
            const std::uint32_t start = pos;
            const char c = source[pos];

            // Language hook — checked first so policies can intercept any character.
            if constexpr (!std::is_same_v<ScannerPolicy, scanner_policy<TokenKind>>) {
                scanner_view<TokenKind, decltype(emit_tok), decltype(emit_trivia_fn)> sv{
                    source, pos, emit_tok, emit_trivia_fn, sink, kinds
                };
                if (ScannerPolicy::scan_custom_token(sv)) {
                    if (pos > start) prev_kind = buf.data.back().kind;
                    continue;
                }
            }

            // Whitespace trivia
            if (c == ' ' || c == '\t' || c == '\r') {
                while (pos < N && (source[pos] == ' ' || source[pos] == '\t' || source[pos] == '\r'))
                    ++pos;
                emit_trivia_fn(trivia_kind::whitespace, start, pos - start);
                continue;
            }

            // Newline + line_policy synthetic separator
            if (c == '\n') {
                ++pos;
                emit_trivia_fn(trivia_kind::newline, start, 1);
                if (!lp.line_continues(prev_kind)) {
                    const TokenKind sep = lp.synthetic_separator();
                    if (sep != kinds.eof && !lp.suppress_separator(prev_kind, sep)) {
                        emit_tok(sep, start, 0);
                        prev_kind = sep;
                    }
                }
                continue;
            }

            // Line comment
            if (c == '/' && pos + 1 < N && source[pos + 1] == '/') {
                while (pos < N && source[pos] != '\n') ++pos;
                emit_trivia_fn(trivia_kind::line_comment, start, pos - start);
                continue;
            }

            // Block comment
            if (c == '/' && pos + 1 < N && source[pos + 1] == '*') {
                pos += 2;
                while (pos + 1 < N && !(source[pos] == '*' && source[pos + 1] == '/')) ++pos;
                if (pos + 1 < N) {
                    pos += 2;
                }
                else {
                    sink.on_diagnostic({
                        samasa_diag_code::lex_unterminated_comment, {},
                        "unterminated block comment", ::lang::severity::error
                    });
                }
                emit_trivia_fn(trivia_kind::block_comment, start, pos - start);
                continue;
            }

            // Identifier / keyword
            if (cs_contains_rt(kIdentStart, c)) {
                while (pos < N && cs_contains_rt(kIdentCont, source[pos])) ++pos;
                const std::string_view word = source.substr(start, pos - start);
                const auto kw = KWTable::template lookup<TokenKind>(word);
                const TokenKind k = kw.value_or(kinds.identifier);
                emit_tok(k, start, pos - start);
                prev_kind = k;
                continue;
            }

            // Integer / float literal
            if (cs_contains_rt(kDigits, c)) {
                while (pos < N && cs_contains_rt(kDigits, source[pos])) ++pos;
                bool is_float = false;
                if (pos < N && source[pos] == '.') {
                    is_float = true;
                    ++pos;
                    while (pos < N && cs_contains_rt(kDigits, source[pos])) ++pos;
                }
                if (pos < N && (source[pos] == 'e' || source[pos] == 'E')) {
                    is_float = true;
                    ++pos;
                    if (pos < N && (source[pos] == '+' || source[pos] == '-')) ++pos;
                    while (pos < N && cs_contains_rt(kDigits, source[pos])) ++pos;
                }
                const TokenKind k = is_float ? kinds.float_literal : kinds.integer_literal;
                emit_tok(k, start, pos - start);
                prev_kind = k;
                continue;
            }

            // String literal
            if (c == '"' || c == '\'') {
                const char delim = c;
                ++pos;
                bool terminated = false;
                while (pos < N) {
                    if (source[pos] == '\\') {
                        pos += 2;
                        continue;
                    }
                    if (source[pos] == delim) {
                        ++pos;
                        terminated = true;
                        break;
                    }
                    ++pos;
                }
                if (!terminated)
                    sink.on_diagnostic({
                        samasa_diag_code::lex_unterminated_string, {},
                        "unterminated string literal", ::lang::severity::error
                    });
                emit_tok(kinds.string_literal, start, pos - start);
                prev_kind = kinds.string_literal;
                continue;
            }

            // Operator (longest match)
            {
                const auto op = OpTrie::template match<TokenKind>(source, pos);
                if (op) {
                    const auto len = static_cast<std::uint32_t>(op->second);
                    emit_tok(op->first, start, len);
                    prev_kind = op->first;
                    pos += len;
                    continue;
                }
            }

            // Unknown character
            sink.on_diagnostic({
                samasa_diag_code::lex_unknown_char, {},
                std::string("unknown character: ") + c, ::lang::severity::error
            });
            emit_tok(kinds.unknown, start, 1);
            prev_kind = kinds.unknown;
            ++pos;
        }

        emit_tok(kinds.eof, N, 0);
        return buf;
    }
} // namespace lang::samasa
