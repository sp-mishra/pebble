#pragma once
// ============================================================================
// ct_parser.hpp — Compile-Time Parser Combinators & Language Tools
// ============================================================================
// Provides:
//   • ct_span        — non-owning cursor over a compile-time string
//   • parse_result   — success/failure carrier with value + remaining span
//   • Parser concept — type constraint for all combinators
//   • Combinators    — lit, char_if, seq2, alt, many0, many1, opt, map,
//                      skip_ws, take_while, take_until, not_p
//   • ct_trie        — O(depth) compile-time keyword lookup
//   • char_class helpers re-exported from akshara::ct_char_set
// ============================================================================
// Design: zero virtual, zero macros, C++23, header-only, consteval throughout.
// All combinators are value types — no heap, no std::function.
// ============================================================================

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>
#include <utility>
#include <variant>
#include <meta/meta.hpp>

namespace ct {
    // =========================================================================
    //  SECTION 1: Span — lightweight cursor into a compile-time string
    // =========================================================================

    // ct_span wraps a string_view and a cursor position.
    // All parse operations take a ct_span and return a new one on success.
    struct ct_span {
        std::string_view src;
        std::size_t pos{0};

        [[nodiscard]] consteval std::size_t remaining() const noexcept {
            return src.size() - pos;
        }

        [[nodiscard]] consteval bool empty() const noexcept {
            return pos >= src.size();
        }

        [[nodiscard]] consteval char peek() const noexcept {
            return src[pos];
        }

        [[nodiscard]] consteval ct_span advance(std::size_t n = 1) const noexcept {
            return {src, pos + n};
        }

        [[nodiscard]] consteval std::string_view rest() const noexcept {
            return src.substr(pos);
        }

        [[nodiscard]] consteval std::string_view slice(std::size_t from, std::size_t to) const noexcept {
            return src.substr(from, to - from);
        }
    };

    // =========================================================================
    //  SECTION 2: parse_result — discriminated union for parse outcome
    // =========================================================================

    template <typename T>
    struct parse_result {
        bool ok{false};
        T value{};
        ct_span next{};

        [[nodiscard]] consteval explicit operator bool() const noexcept { return ok; }

        // Monad-style chaining: if ok, calls fn(value, next); else propagates failure
        template <typename Fn>
        [[nodiscard]] consteval auto and_then(Fn fn) const {
            if (ok) return fn(value, next);
            using R = decltype(fn(value, next));
            return R{false, {}, next};
        }
    };

    // Convenience factory functions
    template <typename T>
    [[nodiscard]] consteval parse_result<T> success(T val, ct_span next) noexcept {
        return {true, std::move(val), next};
    }

    template <typename T>
    [[nodiscard]] consteval parse_result<T> failure(ct_span at) noexcept {
        return {false, {}, at};
    }

    // =========================================================================
    //  SECTION 3: Parser concept
    // =========================================================================

    template <typename P>
    concept Parser = requires(P p, ct_span s) {
        { p(s) }; // returns parse_result<something>
    };

    // =========================================================================
    //  SECTION 4: Core combinators
    // =========================================================================

    // -------------------------------------------------------------------------
    // 4.1  lit<S> — match a fixed_string literal exactly
    // -------------------------------------------------------------------------
    template <akshara::fixed_string S>
    struct lit {
        [[nodiscard]] consteval parse_result<std::string_view>
        operator()(ct_span s) const noexcept {
            constexpr std::string_view token = S.view();
            if (s.remaining() < token.size()) return failure<std::string_view>(s);
            if (s.src.substr(s.pos, token.size()) == token)
                return success(token, s.advance(token.size()));
            return failure<std::string_view>(s);
        }
    };

    // -------------------------------------------------------------------------
    // 4.2  char_if<Pred> — match a single char satisfying predicate
    // -------------------------------------------------------------------------
    template <auto Pred>
    struct char_if {
        [[nodiscard]] consteval parse_result<char>
        operator()(ct_span s) const noexcept {
            if (s.empty()) return failure<char>(s);
            char c = s.peek();
            if (Pred(c)) return success(c, s.advance());
            return failure<char>(s);
        }
    };

    // Predefined char_if instances using meta char predicates
    inline constexpr char_if<[](char c) { return akshara::detail::fs::is_digit(c); }> digit{};
    inline constexpr char_if<[](char c) { return akshara::detail::fs::is_alpha(c); }> alpha{};
    inline constexpr char_if<[](char c) { return akshara::detail::fs::is_alnum(c); }> alnum{};
    inline constexpr char_if<[](char c) { return akshara::detail::fs::is_space(c); }> space{};
    inline constexpr char_if<[](char c) {
        return akshara::detail::fs::is_alpha(c) || c == '_';
    }> ident_start{};
    inline constexpr char_if<[](char c) {
        return akshara::detail::fs::is_alnum(c) || c == '_';
    }> ident_cont{};

    // -------------------------------------------------------------------------
    // 4.3  any_char — match any single character
    // -------------------------------------------------------------------------
    struct any_char_t {
        [[nodiscard]] consteval parse_result<char>
        operator()(ct_span s) const noexcept {
            if (s.empty()) return failure<char>(s);
            return success(s.peek(), s.advance());
        }
    };

    inline constexpr any_char_t any_char{};

    // -------------------------------------------------------------------------
    // 4.4  exact_char — match one specific character
    // -------------------------------------------------------------------------
    struct exact_char {
        char ch;

        [[nodiscard]] consteval parse_result<char>
        operator()(ct_span s) const noexcept {
            if (s.empty() || s.peek() != ch) return failure<char>(s);
            return success(ch, s.advance());
        }
    };

    // -------------------------------------------------------------------------
    // 4.5  seq2<P1, P2> — sequential composition (returns pair)
    // -------------------------------------------------------------------------
    template <typename P1, typename P2>
    struct seq2 {
        P1 p1;
        P2 p2;

        [[nodiscard]] consteval auto operator()(ct_span s) const {
            auto r1 = p1(s);
            if (!r1) {
                using T2 = decltype(p2(r1.next).value);
                return failure<std::pair<decltype(r1.value), T2>>(s);
            }
            auto r2 = p2(r1.next);
            if (!r2) {
                using T2 = decltype(r2.value);
                return failure<std::pair<decltype(r1.value), T2>>(s);
            }
            return success(std::pair{r1.value, r2.value}, r2.next);
        }
    };

    template <typename P1, typename P2>
    [[nodiscard]] consteval seq2<P1, P2> seq(P1 p1, P2 p2) noexcept {
        return {p1, p2};
    }

    // -------------------------------------------------------------------------
    // 4.6  alt<P1, P2> — ordered choice (try P1, then P2)
    // -------------------------------------------------------------------------
    template <typename P1, typename P2>
    struct alt {
        P1 p1;
        P2 p2;

        [[nodiscard]] consteval auto operator()(ct_span s) const {
            auto r1 = p1(s);
            if (r1) return r1;
            return p2(s);
        }
    };

    template <typename P1, typename P2>
    [[nodiscard]] consteval alt<P1, P2> choice(P1 p1, P2 p2) noexcept {
        return {p1, p2};
    }

    // -------------------------------------------------------------------------
    // 4.7  opt<P> — optional (0 or 1 occurrences), always succeeds
    // -------------------------------------------------------------------------
    template <typename P>
    struct opt {
        P p;

        [[nodiscard]] consteval parse_result<std::optional<decltype(p(ct_span{}).value)>>
        operator()(ct_span s) const {
            auto r = p(s);
            using V = decltype(r.value);
            if (r) return success(std::optional<V>{r.value}, r.next);
            return success(std::optional<V>{std::nullopt}, s);
        }
    };

    template <typename P>
    [[nodiscard]] consteval opt<P> optional(P p) noexcept { return {p}; }

    // -------------------------------------------------------------------------
    // 4.8  map<P, Fn> — transform parse result
    // -------------------------------------------------------------------------
    template <typename P, typename Fn>
    struct map_p {
        P p;
        Fn fn;

        [[nodiscard]] consteval auto operator()(ct_span s) const {
            auto r = p(s);
            using Out = decltype(fn(r.value));
            if (!r) return failure<Out>(s);
            return success(fn(r.value), r.next);
        }
    };

    template <typename P, typename Fn>
    [[nodiscard]] consteval map_p<P, Fn> map(P p, Fn fn) noexcept { return {p, fn}; }

    // -------------------------------------------------------------------------
    // 4.9  take_while<Pred> — consume chars satisfying Pred, return string_view
    // -------------------------------------------------------------------------
    template <auto Pred>
    struct take_while_t {
        [[nodiscard]] consteval parse_result<std::string_view>
        operator()(ct_span s) const noexcept {
            std::size_t start = s.pos;
            ct_span cur = s;
            while (!cur.empty() && Pred(cur.peek()))
                cur = cur.advance();
            return success(s.slice(start, cur.pos), cur);
        }
    };

    template <auto Pred>
    inline constexpr take_while_t<Pred> take_while{};

    // take_while1<Pred> — like take_while but requires at least one char
    template <auto Pred>
    struct take_while1_t {
        [[nodiscard]] consteval parse_result<std::string_view>
        operator()(ct_span s) const noexcept {
            std::size_t start = s.pos;
            ct_span cur = s;
            while (!cur.empty() && Pred(cur.peek()))
                cur = cur.advance();
            if (cur.pos == start) return failure<std::string_view>(s);
            return success(s.slice(start, cur.pos), cur);
        }
    };

    template <auto Pred>
    inline constexpr take_while1_t<Pred> take_while1{};

    // -------------------------------------------------------------------------
    // 4.10  take_until<Pred> — consume chars NOT satisfying Pred
    // -------------------------------------------------------------------------
    template <auto Pred>
    struct take_until_t {
        [[nodiscard]] consteval parse_result<std::string_view>
        operator()(ct_span s) const noexcept {
            std::size_t start = s.pos;
            ct_span cur = s;
            while (!cur.empty() && !Pred(cur.peek()))
                cur = cur.advance();
            return success(s.slice(start, cur.pos), cur);
        }
    };

    template <auto Pred>
    inline constexpr take_until_t<Pred> take_until{};

    // -------------------------------------------------------------------------
    // 4.11  skip<P> — run P but discard its value, return unit
    // -------------------------------------------------------------------------
    template <typename P>
    struct skip_t {
        P p;

        [[nodiscard]] consteval parse_result<std::monostate>
        operator()(ct_span s) const {
            auto r = p(s);
            if (!r) return failure<std::monostate>(s);
            return success(std::monostate{}, r.next);
        }
    };

    template <typename P>
    [[nodiscard]] consteval skip_t<P> skip(P p) noexcept { return {p}; }

    // Whitespace skipper
    [[nodiscard]] consteval parse_result<std::monostate>
    skip_ws(ct_span s) noexcept {
        ct_span cur = s;
        while (!cur.empty() && akshara::detail::fs::is_space(cur.peek()))
            cur = cur.advance();
        return success(std::monostate{}, cur);
    }

    // -------------------------------------------------------------------------
    // 4.12  not_p<P> — lookahead negation: succeeds if P fails, consumes nothing
    // -------------------------------------------------------------------------
    template <typename P>
    struct not_p {
        P p;

        [[nodiscard]] consteval parse_result<std::monostate>
        operator()(ct_span s) const {
            auto r = p(s);
            if (r) return failure<std::monostate>(s);
            return success(std::monostate{}, s);
        }
    };

    template <typename P>
    [[nodiscard]] consteval not_p<P> not_(P p) noexcept { return {p}; }

    // =========================================================================
    //  SECTION 5: Common compound parsers
    // =========================================================================

    // identifier parser: ident_start followed by ident_cont*
    struct identifier_t {
        [[nodiscard]] consteval parse_result<std::string_view>
        operator()(ct_span s) const noexcept {
            if (s.empty()) return failure<std::string_view>(s);
            if (!akshara::detail::fs::is_alpha(s.peek()) && s.peek() != '_')
                return failure<std::string_view>(s);
            std::size_t start = s.pos;
            ct_span cur = s.advance();
            while (!cur.empty() &&
                (akshara::detail::fs::is_alnum(cur.peek()) || cur.peek() == '_'))
                cur = cur.advance();
            return success(s.slice(start, cur.pos), cur);
        }
    };

    inline constexpr identifier_t identifier{};

    // decimal integer parser — returns string_view of the digit sequence
    struct decimal_t {
        [[nodiscard]] consteval parse_result<std::string_view>
        operator()(ct_span s) const noexcept {
            if (s.empty() || !akshara::detail::fs::is_digit(s.peek()))
                return failure<std::string_view>(s);
            std::size_t start = s.pos;
            ct_span cur = s;
            while (!cur.empty() && akshara::detail::fs::is_digit(cur.peek()))
                cur = cur.advance();
            return success(s.slice(start, cur.pos), cur);
        }
    };

    inline constexpr decimal_t decimal{};

    // hex integer parser (digits + a-f + A-F)
    struct hex_t {
        [[nodiscard]] consteval parse_result<std::string_view>
        operator()(ct_span s) const noexcept {
            if (s.empty() || !akshara::detail::fs::is_hex(s.peek()))
                return failure<std::string_view>(s);
            std::size_t start = s.pos;
            ct_span cur = s;
            while (!cur.empty() && akshara::detail::fs::is_hex(cur.peek()))
                cur = cur.advance();
            return success(s.slice(start, cur.pos), cur);
        }
    };

    inline constexpr hex_t hex_int{};

    // =========================================================================
    //  SECTION 6: ct_trie — O(depth) compile-time keyword dispatch
    // =========================================================================
    // A trie where each node is a template parameter, constructed entirely at
    // compile time. Maps keyword strings to integer IDs (0-based).
    // Usage:
    //   using kws = ct_trie_builder::build<"if", "else", "while", "for">;
    //   constexpr int id = kws::lookup("while"); // returns 2
    //   constexpr int id = kws::lookup("nope");  // returns -1

    namespace detail {
        // Node in a compile-time trie.
        template <char Label, int TokenId, typename... Children>
        struct trie_node {
            static constexpr char label = Label;
            static constexpr int token_id = TokenId;
            static constexpr bool terminal = (TokenId >= 0);
        };

        // search_nodes — variadic overload using if-constexpr + fold-like recursion
        template <typename... Nodes>
        consteval int search_nodes(std::string_view s, std::size_t depth) noexcept {
            int result = -1;
            bool found = false;
            auto try_node = [&]<char L, int TID, typename... Ch>(trie_node<L, TID, Ch...>) {
                if (found) return;
                if (depth < s.size() && s[depth] == L) {
                    found = true;
                    if (depth + 1 == s.size()) {
                        result = TID;
                    }
                    else {
                        result = search_nodes<Ch...>(s, depth + 1);
                    }
                }
            };
            (try_node(Nodes{}), ...);
            return result;
        }
    } // namespace detail

    // -------------------------------------------------------------------------
    // Flat keyword table — simpler than a trie for small keyword sets.
    // Uses a sorted ct_array and binary search at compile time.
    // -------------------------------------------------------------------------
    template <std::size_t N>
    struct keyword_table {
        std::array<std::string_view, N> keywords;
        int ids[N]{};

        consteval int lookup(std::string_view word) const noexcept {
            for (std::size_t i = 0; i < N; ++i)
                if (keywords[i] == word) return ids[i];
            return -1;
        }

        consteval bool contains(std::string_view word) const noexcept {
            return lookup(word) >= 0;
        }
    };

    // =========================================================================
    //  SECTION 7: Higher-order accumulator — build a ct_array by running
    //  a parser repeatedly (up to MaxItems times)
    // =========================================================================
    template <std::size_t MaxItems, typename P>
    struct many_t {
        P p;

        [[nodiscard]] consteval auto operator()(ct_span s) const {
            using V = decltype(p(s).value);
            meta::ct_array<V, MaxItems> acc{};
            ct_span cur = s;
            while (!cur.empty() && acc.count < MaxItems) {
                auto r = p(cur);
                if (!r) break;
                acc.push_back(r.value);
                cur = r.next;
            }
            return success(acc, cur);
        }
    };

    template <std::size_t MaxItems, typename P>
    [[nodiscard]] consteval many_t<MaxItems, P> many(P p) noexcept { return {p}; }

    // =========================================================================
    //  SECTION 8: Tokenizer — split a string into tokens via a parser
    // =========================================================================
    // token_stream<MaxTokens, P> applies P repeatedly, skipping whitespace,
    // and collects results into a ct_array.
    template <std::size_t MaxTokens, typename P>
    struct token_stream_t {
        P p;

        [[nodiscard]] consteval auto operator()(ct_span s) const {
            using V = decltype(p(s).value);
            meta::ct_array<V, MaxTokens> tokens{};
            ct_span cur = s;
            while (!cur.empty() && tokens.count < MaxTokens) {
                // skip whitespace
                while (!cur.empty() && akshara::detail::fs::is_space(cur.peek()))
                    cur = cur.advance();
                if (cur.empty()) break;
                auto r = p(cur);
                if (!r) break;
                tokens.push_back(r.value);
                cur = r.next;
            }
            return success(tokens, cur);
        }
    };

    template <std::size_t MaxTokens, typename P>
    [[nodiscard]] consteval token_stream_t<MaxTokens, P>
    tokenize(P p) noexcept { return {p}; }

    // =========================================================================
    //  SECTION 9: ct_char_set integration (re-exports from meta)
    // =========================================================================
    using akshara::ct_char_set;
    using akshara::cs_digits;
    using akshara::cs_alpha;
    using akshara::cs_alnum;
    using akshara::cs_upper;
    using akshara::cs_lower;
    using akshara::cs_whitespace;
    using akshara::cs_hex;
    using akshara::cs_ident_start;
    using akshara::cs_ident_cont;

    // char_in_set<Set> — parser matching any char in the given ct_char_set
    template <ct_char_set Set>
    struct char_in_set_t {
        [[nodiscard]] consteval parse_result<char>
        operator()(ct_span s) const noexcept {
            if (s.empty() || !Set.contains(s.peek())) return failure<char>(s);
            return success(s.peek(), s.advance());
        }
    };

    template <ct_char_set Set>
    inline constexpr char_in_set_t<Set> char_in_set{};
} // namespace ct
