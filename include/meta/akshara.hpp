#pragma once
// ============================================================================
// akshara.hpp — Compile-Time String Library (C++23 / C++26)
// ============================================================================
// "Akshara" (Sanskrit: अक्षर) — letter, character, syllable; that which
// does not perish. A zero-overhead, header-only compile-time string
// library built for language tools, parsers, diagnostics, and metaprogramming.
//
// Provides:
//   fixed_string<N>       — NTTP-capable null-terminated string literal type
//   ""_fs                 — User-defined literal operator (akshara::literals)
//   ct_string_builder<C>  — compile-time mutable string buffer
//   String algorithms     — substr, find, rfind, contains, starts/ends_with,
//                           to_upper/lower, replace_char, repeat, trim_view,
//                           uint_to_str, str_to_uint, concat
//   KMP algorithms        — kmp_find, kmp_count (O(N+M))
//   join                  — join two fixed_strings with separator
//   ct_char_set           — 128-bit compile-time ASCII character set
//   Char set factories    — cs_digits, cs_alpha, cs_alnum, cs_upper, cs_lower,
//                           cs_whitespace, cs_hex, cs_ident_start, cs_ident_cont
//   fnv1a64               — consteval FNV-1a 64-bit hash
//   pad_right / pad_left  — compile-time string padding to fixed width
//   intern_tag<S>         — type-level string identity for O(1) equality
//   format_static_error   — C++26 rich compile-time diagnostic string generator
//   std::formatter        — std::format / std::print interop for fixed_string
//
// No macros. No virtual dispatch. No heap. No runtime cost.
// ============================================================================

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string_view>

namespace akshara {
    // =========================================================================
    //  SECTION 1: fixed_string — NTTP-capable compile-time string
    // =========================================================================

    template <std::size_t N>
    struct fixed_string {
        char data[N]{};
        static constexpr std::size_t length = N - 1;

        consteval fixed_string() = default;

        consteval fixed_string(const char (&str)[N]) noexcept {
            for (std::size_t i = 0; i < N; ++i)
                data[i] = str[i];
        }

        // STL/ranges compliance — behaves like std::array<char,N> / std::string_view
        [[nodiscard]] constexpr const char* data_ptr() const noexcept { return data; }
        [[nodiscard]] constexpr const char* begin() const noexcept { return data; }
        [[nodiscard]] constexpr const char* end() const noexcept { return data + length; }
        [[nodiscard]] constexpr const char* cbegin() const noexcept { return data; }
        [[nodiscard]] constexpr const char* cend() const noexcept { return data + length; }
        [[nodiscard]] constexpr std::size_t size() const noexcept { return length; }
        [[nodiscard]] constexpr bool empty() const noexcept { return length == 0; }

        // Implicit conversion — enables: std::string_view sv = str;
        [[nodiscard]] constexpr operator std::string_view() const noexcept {
            return {data, length};
        }

        // view() kept for backward compatibility
        [[nodiscard]] constexpr std::string_view view() const noexcept {
            return {data, length};
        }

        [[nodiscard]] constexpr char operator[](std::size_t i) const noexcept {
            return data[i];
        }

        template <std::size_t M>
        [[nodiscard]] consteval bool operator==(const fixed_string<M>& other) const noexcept {
            if constexpr (N != M) return false;
            else {
                for (std::size_t i = 0; i < N; ++i)
                    if (data[i] != other.data[i]) return false;
                return true;
            }
        }

        // Concatenate two fixed_strings
        template <std::size_t M>
        [[nodiscard]] consteval auto operator+(const fixed_string<M>& other) const noexcept {
            // N+M-1: (N-1)+(M-1) content chars + 1 null terminator
            fixed_string<N + M - 1> result{};
            for (std::size_t i = 0; i < length; ++i)
                result.data[i] = data[i];
            for (std::size_t i = 0; i < M; ++i)
                result.data[length + i] = other.data[i];
            return result;
        }
    };

    // CTAD deduction guide
    template <std::size_t N>
    fixed_string(const char (&)[N]) -> fixed_string<N>;

    // -------------------------------------------------------------------------
    // Comparison operators (free functions — ADL-friendly)
    // -------------------------------------------------------------------------

    template <std::size_t N, std::size_t M>
    [[nodiscard]] consteval bool operator!=(const fixed_string<N>& a,
                                            const fixed_string<M>& b) noexcept {
        return !(a == b);
    }

    template <std::size_t N, std::size_t M>
    [[nodiscard]] consteval std::strong_ordering
    operator<=>(const fixed_string<N>& a, const fixed_string<M>& b) noexcept {
        constexpr std::size_t min_len = (N < M) ? N - 1 : M - 1;
        for (std::size_t i = 0; i < min_len; ++i) {
            if (a.data[i] < b.data[i]) return std::strong_ordering::less;
            if (a.data[i] > b.data[i]) return std::strong_ordering::greater;
        }
        if (N - 1 < M - 1) return std::strong_ordering::less;
        if (N - 1 > M - 1) return std::strong_ordering::greater;
        return std::strong_ordering::equal;
    }

    // Concatenate with a single char
    template <std::size_t N>
    [[nodiscard]] consteval fixed_string<N + 1>
    operator+(const fixed_string<N>& s, char c) noexcept {
        fixed_string<N + 1> result{};
        for (std::size_t i = 0; i < N - 1; ++i)
            result.data[i] = s.data[i];
        result.data[N - 1] = c;
        return result;
    }

    template <std::size_t N>
    [[nodiscard]] consteval fixed_string<N + 1>
    operator+(char c, const fixed_string<N>& s) noexcept {
        fixed_string<N + 1> result{};
        result.data[0] = c;
        for (std::size_t i = 0; i < N; ++i)
            result.data[1 + i] = s.data[i];
        return result;
    }

    // =========================================================================
    //  SECTION 2: detail::fs — internal char classification & helpers
    // =========================================================================

    namespace detail::fs {
        // Char classification (consteval-safe, locale-independent)
        [[nodiscard]] consteval bool is_upper(const char c) noexcept { return c >= 'A' && c <= 'Z'; }
        [[nodiscard]] consteval bool is_lower(const char c) noexcept { return c >= 'a' && c <= 'z'; }
        [[nodiscard]] consteval bool is_alpha(const char c) noexcept { return is_upper(c) || is_lower(c); }
        [[nodiscard]] consteval bool is_digit(const char c) noexcept { return c >= '0' && c <= '9'; }
        [[nodiscard]] consteval bool is_alnum(const char c) noexcept { return is_alpha(c) || is_digit(c); }

        [[nodiscard]] consteval bool is_space(const char c) noexcept {
            return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
        }

        [[nodiscard]] consteval bool is_hex(const char c) noexcept {
            return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        }

        [[nodiscard]] consteval bool is_ident_start(const char c) noexcept { return is_alpha(c) || c == '_'; }
        [[nodiscard]] consteval bool is_ident_cont(const char c) noexcept { return is_alnum(c) || c == '_'; }
        [[nodiscard]] consteval bool is_print(const char c) noexcept { return c >= 0x20 && c < 0x7F; }

        [[nodiscard]] consteval bool is_punct(const char c) noexcept {
            return is_print(c) && !is_alnum(c) && c != ' ';
        }

        [[nodiscard]] consteval char to_upper(const char c) noexcept {
            return is_lower(c) ? static_cast<char>(c - ('a' - 'A')) : c;
        }

        [[nodiscard]] consteval char to_lower(const char c) noexcept {
            return is_upper(c) ? static_cast<char>(c + ('a' - 'A')) : c;
        }

        // repeat_impl — recursive compile-time string repetition
        template <std::size_t N, std::size_t Count>
        struct repeat_impl {
            static consteval auto make(const fixed_string<N>& s) noexcept {
                if constexpr (Count == 0) return fixed_string<1>{};
                else if constexpr (Count == 1) return s;
                else return s + repeat_impl<N, Count - 1>::make(s);
            }
        };

        // Trim offset helpers
        template <std::size_t N>
        constexpr std::size_t trim_front_offset(const fixed_string<N>& s) noexcept {
            for (std::size_t i = 0; i < s.length; ++i)
                if (!is_space(s.data[i])) return i;
            return s.length;
        }

        template <std::size_t N>
        constexpr std::size_t trim_back_end(const fixed_string<N>& s) noexcept {
            for (std::size_t i = s.length; i-- > 0;)
                if (!is_space(s.data[i])) return i + 1;
            return 0;
        }

        // Integer → string helpers
        template <std::size_t V>
        consteval std::size_t digit_count() noexcept {
            if constexpr (V < 10) return 1;
            else if constexpr (V < 100) return 2;
            else return 1 + digit_count<V / 10>();
        }

        template <std::size_t V, std::size_t Digits>
        consteval fixed_string<Digits + 1> uint_to_fixed() noexcept {
            fixed_string<Digits + 1> buf{};
            std::size_t rem = V;
            for (std::size_t i = Digits; i-- > 0;) {
                buf.data[i] = static_cast<char>('0' + rem % 10);
                rem /= 10;
            }
            return buf;
        }
    } // namespace detail::fs

    // =========================================================================
    //  SECTION 3: String algorithms (free functions)
    // =========================================================================

    // substr<Start, Len>(s) — compile-time slice; Start+Len must be <= s.length
    template <std::size_t Start, std::size_t Len, std::size_t N>
        requires(Start + Len < N)
    [[nodiscard]] consteval fixed_string<Len + 1> substr(const fixed_string<N>& s) noexcept {
        fixed_string<Len + 1> result{};
        for (std::size_t i = 0; i < Len; ++i)
            result.data[i] = s.data[Start + i];
        return result;
    }

    // find_char — first position of c, or string_view::npos
    template <std::size_t N>
    [[nodiscard]] consteval std::size_t find_char(const fixed_string<N>& s, char c) noexcept {
        for (std::size_t i = 0; i < s.length; ++i)
            if (s.data[i] == c) return i;
        return std::string_view::npos;
    }

    // rfind_char — last position of c, or npos
    template <std::size_t N>
    [[nodiscard]] consteval std::size_t rfind_char(const fixed_string<N>& s, char c) noexcept {
        for (std::size_t i = s.length; i-- > 0;)
            if (s.data[i] == c) return i;
        return std::string_view::npos;
    }

    // contains_char
    template <std::size_t N>
    [[nodiscard]] consteval bool contains_char(const fixed_string<N>& s, char c) noexcept {
        return find_char(s, c) != std::string_view::npos;
    }

    // find_substr — naive O(n·m) search; use kmp_find for large inputs
    template <std::size_t N, std::size_t M>
    [[nodiscard]] consteval std::size_t find_substr(const fixed_string<N>& haystack,
                                                    const fixed_string<M>& needle) noexcept {
        if constexpr (M - 1 > N - 1) return std::string_view::npos;
        const std::size_t limit = (N - 1) - (M - 1);
        for (std::size_t i = 0; i <= limit; ++i) {
            bool match = true;
            for (std::size_t j = 0; j < M - 1; ++j)
                if (haystack.data[i + j] != needle.data[j]) {
                    match = false;
                    break;
                }
            if (match) return i;
        }
        return std::string_view::npos;
    }

    // starts_with / ends_with
    template <std::size_t N, std::size_t M>
    [[nodiscard]] consteval bool starts_with(const fixed_string<N>& s,
                                             const fixed_string<M>& prefix) noexcept {
        if constexpr (M - 1 > N - 1) return false;
        for (std::size_t i = 0; i < M - 1; ++i)
            if (s.data[i] != prefix.data[i]) return false;
        return true;
    }

    template <std::size_t N, std::size_t M>
    [[nodiscard]] consteval bool ends_with(const fixed_string<N>& s,
                                           const fixed_string<M>& suffix) noexcept {
        if constexpr (M - 1 > N - 1) return false;
        const std::size_t offset = (N - 1) - (M - 1);
        for (std::size_t i = 0; i < M - 1; ++i)
            if (s.data[offset + i] != suffix.data[i]) return false;
        return true;
    }

    // to_upper / to_lower — case-mapped copies
    template <std::size_t N>
    [[nodiscard]] consteval fixed_string<N> to_upper(const fixed_string<N>& s) noexcept {
        fixed_string<N> result{};
        for (std::size_t i = 0; i < N; ++i)
            result.data[i] = detail::fs::to_upper(s.data[i]);
        return result;
    }

    template <std::size_t N>
    [[nodiscard]] consteval fixed_string<N> to_lower(const fixed_string<N>& s) noexcept {
        fixed_string<N> result{};
        for (std::size_t i = 0; i < N; ++i)
            result.data[i] = detail::fs::to_lower(s.data[i]);
        return result;
    }

    // replace_char — every 'from' → 'to'
    template <std::size_t N>
    [[nodiscard]] consteval fixed_string<N> replace_char(const fixed_string<N>& s,
                                                         char from, char to) noexcept {
        fixed_string<N> result{};
        for (std::size_t i = 0; i < N; ++i)
            result.data[i] = (s.data[i] == from) ? to : s.data[i];
        return result;
    }

    // repeat<Count>(s) — s concatenated Count times
    template <std::size_t Count, std::size_t N>
    [[nodiscard]] consteval auto repeat(const fixed_string<N>& s) noexcept {
        return detail::fs::repeat_impl<N, Count>::make(s);
    }

    // trim_view — strip leading/trailing whitespace, returns string_view
    template <std::size_t N>
    [[nodiscard]] constexpr std::string_view trim_view(const fixed_string<N>& s) noexcept {
        const std::size_t start = detail::fs::trim_front_offset(s);
        const std::size_t end = detail::fs::trim_back_end(s);
        if (start >= end) return {};
        return std::string_view{s.data + start, end - start};
    }

    // uint_to_str<V>() — compile-time unsigned integer → fixed_string
    template <std::size_t V>
    [[nodiscard]] consteval auto uint_to_str() noexcept {
        constexpr std::size_t Digits = detail::fs::digit_count<V>();
        return detail::fs::uint_to_fixed<V, Digits>();
    }

    // str_to_uint(s) — compile-time decimal string → std::size_t
    template <std::size_t N>
    [[nodiscard]] consteval std::size_t str_to_uint(const fixed_string<N>& s) noexcept {
        std::size_t result = 0;
        for (std::size_t i = 0; i < s.length; ++i)
            result = result * 10 + static_cast<std::size_t>(s.data[i] - '0');
        return result;
    }

    // =========================================================================
    //  SECTION 4: ct_string_builder — compile-time mutable string buffer
    // =========================================================================

    template <std::size_t Capacity>
    struct ct_string_builder {
        char buf[Capacity]{};
        std::size_t len = 0;

        consteval void push(char c) noexcept { buf[len++] = c; }

        template <std::size_t N>
        consteval void append(const fixed_string<N>& s) noexcept {
            for (std::size_t i = 0; i < s.length; ++i)
                push(s.data[i]);
        }

        consteval void append(const std::string_view sv) noexcept {
            for (const char c : sv) push(c);
        }

        // Build a fixed_string of exactly N chars from the buffer.
        // Caller must supply the correct N (= number of chars pushed).
        template <std::size_t N>
        [[nodiscard]] consteval fixed_string<N + 1> build() const noexcept {
            static_assert(N <= Capacity, "build<N>: N exceeds capacity");
            fixed_string<N + 1> result{};
            for (std::size_t i = 0; i < N; ++i)
                result.data[i] = buf[i];
            return result;
        }

        [[nodiscard]] consteval std::size_t size() const noexcept { return len; }
    };

    // =========================================================================
    //  SECTION 5: KMP compile-time substring search — O(N+M)
    // =========================================================================

    // kmp_find — first occurrence position, or string_view::npos
    template <std::size_t N, std::size_t M>
    [[nodiscard]] consteval std::size_t kmp_find(const fixed_string<N>& haystack,
                                                 const fixed_string<M>& needle) noexcept {
        constexpr std::size_t Nh = N - 1;
        constexpr std::size_t Nm = M - 1;
        if constexpr (Nm == 0) return 0;
        if constexpr (Nm > Nh) return std::string_view::npos;
        // Build KMP failure table inline (function-parameter data can't be constexpr local)
        std::array<int, Nm> fail{};
        fail[0] = 0;
        for (std::size_t i = 1; i < Nm; ++i) {
            int k = fail[i - 1];
            while (k > 0 && needle.data[i] != needle.data[k]) k = fail[k - 1];
            if (needle.data[i] == needle.data[k]) ++k;
            fail[i] = k;
        }
        std::size_t j = 0;
        for (std::size_t i = 0; i < Nh; ++i) {
            while (j > 0 && haystack.data[i] != needle.data[j])
                j = static_cast<std::size_t>(fail[j - 1]);
            if (haystack.data[i] == needle.data[j]) ++j;
            if (j == Nm) return i - Nm + 1;
        }
        return std::string_view::npos;
    }

    // kmp_count — count non-overlapping occurrences
    template <std::size_t N, std::size_t M>
    [[nodiscard]] consteval std::size_t kmp_count(const fixed_string<N>& haystack,
                                                  const fixed_string<M>& needle) noexcept {
        constexpr std::size_t Nh = N - 1;
        constexpr std::size_t Nm = M - 1;
        if constexpr (Nm == 0 || Nm > Nh) return 0;
        std::array<int, Nm> fail{};
        fail[0] = 0;
        for (std::size_t i = 1; i < Nm; ++i) {
            int k = fail[i - 1];
            while (k > 0 && needle.data[i] != needle.data[k]) k = fail[k - 1];
            if (needle.data[i] == needle.data[k]) ++k;
            fail[i] = k;
        }
        std::size_t count = 0, j = 0;
        for (std::size_t i = 0; i < Nh; ++i) {
            while (j > 0 && haystack.data[i] != needle.data[j])
                j = static_cast<std::size_t>(fail[j - 1]);
            if (haystack.data[i] == needle.data[j]) ++j;
            if (j == Nm) {
                ++count;
                j = static_cast<std::size_t>(fail[Nm - 1]);
            }
        }
        return count;
    }

    // =========================================================================
    //  SECTION 6: join — concatenate two fixed_strings with separator
    // =========================================================================

    template <std::size_t SN, std::size_t AN, std::size_t BN>
    [[nodiscard]] consteval auto join(const fixed_string<SN>& sep,
                                      const fixed_string<AN>& a,
                                      const fixed_string<BN>& b) noexcept {
        return a + sep + b;
    }

    // =========================================================================
    //  SECTION 7: ct_char_set — compile-time ASCII character set (bitset 0–127)
    //
    //  Internal representation: two uint64_t words (low = bits 0–63,
    //  high = bits 64–127). Operations are single bitwise instructions
    //  instead of 128 bool array writes — much smaller object, faster
    //  constexpr evaluation, easier compiler optimization.
    //  Public API is unchanged.
    // =========================================================================

    struct ct_char_set {
        std::uint64_t low = 0; // bits  0–63
        std::uint64_t high = 0; // bits 64–127

        constexpr ct_char_set() noexcept = default;

        constexpr ct_char_set(const std::uint64_t l, const std::uint64_t h) noexcept
            : low(l), high(h) {}

        // Construct from fixed_string of member characters
        template <std::size_t N>
        constexpr explicit ct_char_set(const fixed_string<N>& chars) noexcept {
            for (std::size_t i = 0; i < chars.length; ++i) {
                if (const unsigned idx = static_cast<unsigned char>(chars.data[i]) & 0x7Fu; idx < 64)
                    low |= (
                        std::uint64_t{1} << idx);
                else high |= (std::uint64_t{1} << (idx - 64));
            }
        }

        [[nodiscard]] constexpr bool contains(const char c) const noexcept {
            if (const unsigned idx = static_cast<unsigned char>(c) & 0x7Fu; idx < 64) return (low >> idx) & 1u;
            else return (high >> (idx - 64)) & 1u;
        }

        [[nodiscard]] constexpr ct_char_set operator|(const ct_char_set& o) const noexcept {
            return {low | o.low, high | o.high};
        }

        [[nodiscard]] constexpr ct_char_set operator&(const ct_char_set& o) const noexcept {
            return {low & o.low, high & o.high};
        }

        [[nodiscard]] constexpr ct_char_set operator^(const ct_char_set& o) const noexcept {
            return {low ^ o.low, high ^ o.high};
        }

        [[nodiscard]] constexpr ct_char_set complement() const noexcept {
            // Only invert bits 0–127; bits outside ASCII stay zero.
            return {~low, ~high & 0xFFFF'FFFF'FFFF'FFFFull};
        }

        constexpr void add_range(const char lo, const char hi) noexcept {
            for (int c = static_cast<unsigned char>(lo);
                 c <= static_cast<unsigned char>(hi); ++c) {
                if (const unsigned idx = static_cast<unsigned>(c) & 0x7Fu; idx < 64) low |= (std::uint64_t{1} << idx);
                else high |= (std::uint64_t{1} << (idx - 64));
            }
        }

        constexpr void add(const char c) noexcept {
            if (const unsigned idx = static_cast<unsigned char>(c) & 0x7Fu; idx < 64) low |= (std::uint64_t{1} << idx);
            else high |= (std::uint64_t{1} << (idx - 64));
        }
    };

    // Predefined char set factories
    [[nodiscard]] constexpr ct_char_set cs_digits() noexcept {
        ct_char_set s;
        s.add_range('0', '9');
        return s;
    }

    [[nodiscard]] constexpr ct_char_set cs_upper() noexcept {
        ct_char_set s;
        s.add_range('A', 'Z');
        return s;
    }

    [[nodiscard]] constexpr ct_char_set cs_lower() noexcept {
        ct_char_set s;
        s.add_range('a', 'z');
        return s;
    }

    [[nodiscard]] constexpr ct_char_set cs_alpha() noexcept { return cs_upper() | cs_lower(); }
    [[nodiscard]] constexpr ct_char_set cs_alnum() noexcept { return cs_alpha() | cs_digits(); }

    [[nodiscard]] constexpr ct_char_set cs_whitespace() noexcept {
        ct_char_set s;
        for (const char c : {' ', '\t', '\n', '\r', '\f', '\v'})
            s.add(c);
        return s;
    }

    [[nodiscard]] constexpr ct_char_set cs_hex() noexcept {
        return cs_digits() | ct_char_set{fixed_string{"abcdefABCDEF"}};
    }

    [[nodiscard]] constexpr ct_char_set cs_ident_start() noexcept {
        ct_char_set s = cs_alpha();
        s.add('_');
        return s;
    }

    [[nodiscard]] constexpr ct_char_set cs_ident_cont() noexcept {
        ct_char_set s = cs_alnum();
        s.add('_');
        return s;
    }

    // =========================================================================
    //  SECTION 8: fnv1a64 — compile-time FNV-1a 64-bit hash
    // =========================================================================

    [[nodiscard]] consteval std::uint64_t fnv1a64(const std::string_view sv) noexcept {
        constexpr std::uint64_t offset = 14695981039346656037ULL;
        constexpr std::uint64_t prime = 1099511628211ULL;
        std::uint64_t h = offset;
        for (const char c : sv) h = (h ^ static_cast<unsigned char>(c)) * prime;
        return h;
    }

    template <std::size_t N>
    [[nodiscard]] consteval std::uint64_t fnv1a64(const fixed_string<N>& s) noexcept {
        return fnv1a64(s.view());
    }

    // =========================================================================
    //  SECTION 9: Padding — pad_right / pad_left to a fixed width
    // =========================================================================

    template <std::size_t Width, std::size_t N>
        requires(Width + 1 >= N)
    [[nodiscard]] consteval fixed_string<Width + 1>
    pad_right(const fixed_string<N>& s, char fill = ' ') noexcept {
        fixed_string<Width + 1> result{};
        for (std::size_t i = 0; i < s.length; ++i)
            result.data[i] = s.data[i];
        for (std::size_t i = s.length; i < Width; ++i)
            result.data[i] = fill;
        return result;
    }

    template <std::size_t Width, std::size_t N>
        requires(Width + 1 >= N)
    [[nodiscard]] consteval fixed_string<Width + 1>
    pad_left(const fixed_string<N>& s, char fill = ' ') noexcept {
        fixed_string<Width + 1> result{};
        const std::size_t offset = Width - s.length;
        for (std::size_t i = 0; i < offset; ++i)
            result.data[i] = fill;
        for (std::size_t i = 0; i < s.length; ++i)
            result.data[offset + i] = s.data[i];
        return result;
    }

    // =========================================================================
    //  SECTION 10: intern_tag — type-level string identity
    // =========================================================================
    // intern_tag<S> is a unique type per string value.
    // Two intern_tag<S> with the same S are the SAME type → O(1) comparison
    // at the type level (std::same_as rather than character comparison).

    template <fixed_string S>
    struct intern_tag {
        static constexpr auto value = S;
        static consteval std::string_view str() noexcept { return S.view(); }
        static consteval std::uint64_t hash() noexcept { return fnv1a64(S); }
        static consteval std::size_t length() noexcept { return S.length; }

        template <fixed_string Other>
        static consteval bool same_as(intern_tag<Other>) noexcept { return S == Other; }
    };

    // intern_equal<A,B> — true iff both string values are identical
    template <fixed_string A, fixed_string B>
    inline constexpr bool intern_equal = (A == B);

    // =========================================================================
    //  SECTION 11: path — compile-time path operations on fixed_string
    // =========================================================================
    // All functions return string_view into the input string's storage (zero
    // allocation) or produce new fixed_string values at consteval time.

    namespace path {
        // filename — returns basename including extension ("foo.cpp" from "src/foo.cpp")
        template <std::size_t N>
        [[nodiscard]] consteval std::string_view filename(const fixed_string<N>& p) noexcept {
            const std::string_view sv = p.view();
            const auto slash = sv.rfind('/');
            if (slash == std::string_view::npos) return sv;
            return sv.substr(slash + 1);
        }

        // stem — basename without extension ("foo" from "src/foo.cpp")
        template <std::size_t N>
        [[nodiscard]] consteval std::string_view stem(const fixed_string<N>& p) noexcept {
            const std::string_view fname = filename(p);
            const auto dot = fname.rfind('.');
            if (dot == std::string_view::npos || dot == 0) return fname;
            return fname.substr(0, dot);
        }

        // extension — file extension including dot (".cpp" from "src/foo.cpp")
        template <std::size_t N>
        [[nodiscard]] consteval std::string_view extension(const fixed_string<N>& p) noexcept {
            const std::string_view fname = filename(p);
            const auto dot = fname.rfind('.');
            if (dot == std::string_view::npos || dot == 0) return {};
            return fname.substr(dot);
        }

        // parent_path — directory portion ("src/parser" from "src/parser/foo.cpp")
        template <std::size_t N>
        [[nodiscard]] consteval std::string_view parent_path(const fixed_string<N>& p) noexcept {
            const std::string_view sv = p.view();
            const auto slash = sv.rfind('/');
            if (slash == std::string_view::npos) return {};
            return sv.substr(0, slash);
        }

        // normalize — fold away "./" and trailing "/" (returns string_view into p)
        // Full ".." resolution requires heap; this only strips trivial redundancies.
        template <std::size_t N>
        [[nodiscard]] consteval std::string_view normalize(const fixed_string<N>& p) noexcept {
            std::string_view sv = p.view();
            // Strip leading "./"
            while (sv.starts_with("./")) sv.remove_prefix(2);
            // Strip trailing "/"
            while (sv.size() > 1 && sv.back() == '/') sv.remove_suffix(1);
            return sv;
        }
    } // namespace path

    // =========================================================================
    //  SECTION 12: Variadic Concat & Diagnostic String Formatters
    // =========================================================================

    /// Variadic compile-time string concatenation
    template <std::size_t N>
    [[nodiscard]] consteval auto concat_str(const fixed_string<N>& s) noexcept {
        return s;
    }

    template <std::size_t N1, std::size_t N2, std::size_t... Ns>
    [[nodiscard]] consteval auto concat_str(const fixed_string<N1>& s1,
                                            const fixed_string<N2>& s2,
                                            const fixed_string<Ns>&... rest) noexcept {
        return concat_str(s1 + s2, rest...);
    }

    template <std::size_t N, std::size_t... Ns>
    [[nodiscard]] consteval auto concat(const fixed_string<N>& s,
                                        const fixed_string<Ns>&... rest) noexcept {
        return concat_str(s, rest...);
    }

    /// Compile-time diagnostic formatter for static assertions (C++26 rich static_assert)
    template <fixed_string... Parts>
    struct static_error_message {
        static constexpr auto value = concat_str(Parts...);
        static constexpr const char* data = value.data;
    };

    template <fixed_string... Parts>
    inline constexpr auto format_static_error = concat_str(Parts...);

    // =========================================================================
    //  SECTION 13: literals — User-defined literal operator (""_fs)
    // =========================================================================

    inline namespace literals {
        template <fixed_string S>
        [[nodiscard]] consteval auto operator""_fs() noexcept {
            return S;
        }
    } // namespace literals

    // =========================================================================
    //  SECTION 14: Pattern & Character Matcher
    // =========================================================================

    struct matcher {
        /// Checks if a string contains only characters from the allowed charset
        template <std::size_t N>
        [[nodiscard]] static consteval bool matches_all(const fixed_string<N>& str,
                                                        ct_char_set allowed) noexcept {
            for (std::size_t i = 0; i < str.length; ++i) {
                if (!allowed.contains(str.data[i])) return false;
            }
            return true;
        }

        /// Checks if a string view contains only characters from the allowed charset
        [[nodiscard]] static constexpr bool matches_all(std::string_view sv,
                                                        ct_char_set allowed) noexcept {
            for (char c : sv) {
                if (!allowed.contains(c)) return false;
            }
            return true;
        }

        /// Checks if an identifier is a valid C++ identifier name
        template <std::size_t N>
        [[nodiscard]] static consteval bool is_valid_c_identifier(const fixed_string<N>& str) noexcept {
            if constexpr (str.length == 0) return false;
            if (!cs_ident_start().contains(str.data[0])) return false;
            for (std::size_t i = 1; i < str.length; ++i) {
                if (!cs_ident_cont().contains(str.data[i])) return false;
            }
            return true;
        }
    };
} // namespace akshara

// =========================================================================
// std::formatter specialization for akshara::fixed_string
// Enables std::format("{}", "text"_fs) and std::print
// =========================================================================
template <std::size_t N, typename CharT>
struct std::formatter<akshara::fixed_string<N>, CharT> : std::formatter<std::basic_string_view<CharT>, CharT> {
    template <typename FormatContext>
    auto format(const akshara::fixed_string<N>& str, FormatContext& ctx) const {
        return std::formatter<std::basic_string_view<CharT>, CharT>::format(
            std::basic_string_view<CharT>{str.data, str.length}, ctx);
    }
};
