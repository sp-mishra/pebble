// ============================================================================
// test_akshara.cpp — Unit tests for meta/akshara.hpp
// ============================================================================
// Tests for: fixed_string, char classifiers, string algorithms,
//            ct_string_builder, KMP search, join, ct_char_set,
//            fnv1a64, pad_right/pad_left, intern_tag, path
// ============================================================================

#include "catch_amalgamated.hpp"
#include "meta/akshara.hpp"

// ============================================================================
// SECTION 1: fixed_string — construction, data, size, operators
// ============================================================================

TEST_CASE (

"fixed_string: construction and length"
,
"[akshara][fixed_string]"
)
 {
    static constexpr akshara::fixed_string s{"hello"};
    STATIC_REQUIRE(s.length == 5);
    STATIC_REQUIRE(s.size() == 5);
    STATIC_REQUIRE(!s.empty());

    static constexpr akshara::fixed_string empty{""};
    STATIC_REQUIRE(empty.length == 0);
    STATIC_REQUIRE(empty.empty());
}

TEST_CASE (

"fixed_string: CTAD deduction"
,
"[akshara][fixed_string]"
)
 {
    static constexpr akshara::fixed_string s{"world"};
    STATIC_REQUIRE(std::same_as<decltype(s), const akshara::fixed_string<6>>);
}

TEST_CASE (

"fixed_string: operator[]"
,
"[akshara][fixed_string]"
)
 {
    static constexpr akshara::fixed_string s{"abc"};
    STATIC_REQUIRE(s[0] == 'a');
    STATIC_REQUIRE(s[1] == 'b');
    STATIC_REQUIRE(s[2] == 'c');
    STATIC_REQUIRE(s[3] == '\0');
}

TEST_CASE (

"fixed_string: view() and operator std::string_view"
,
"[akshara][fixed_string]"
)
 {
    static constexpr akshara::fixed_string s{"test"};
    STATIC_REQUIRE(s.view() == "test");

    static constexpr std::string_view sv = s;
    STATIC_REQUIRE(sv == "test");
    STATIC_REQUIRE(sv.size() == 4);
}

TEST_CASE (

"fixed_string: begin/end iterators"
,
"[akshara][fixed_string]"
)
 {
    static constexpr akshara::fixed_string s{"hi"};
    STATIC_REQUIRE(s.end() - s.begin() == 2);
    STATIC_REQUIRE(*s.begin() == 'h');
    STATIC_REQUIRE(*(s.begin() + 1) == 'i');
    STATIC_REQUIRE(s.cend() - s.cbegin() == 2);
}

TEST_CASE (

"fixed_string: ranges::equal"
,
"[akshara][fixed_string]"
)
 {
    static constexpr akshara::fixed_string a{"hello"};
    static constexpr akshara::fixed_string b{"hello"};
    static constexpr akshara::fixed_string c{"world"};
    STATIC_REQUIRE(std::ranges::equal(a, b));
    STATIC_REQUIRE(!std::ranges::equal(a, c));
}

TEST_CASE (

"fixed_string: operator== and operator!="
,
"[akshara][fixed_string]"
)
 {
    static constexpr akshara::fixed_string a{"foo"};
    static constexpr akshara::fixed_string b{"foo"};
    static constexpr akshara::fixed_string c{"bar"};
    STATIC_REQUIRE(a == b);
    STATIC_REQUIRE(a != c);
}

TEST_CASE (

"fixed_string: operator<=>"
,
"[akshara][fixed_string]"
)
 {
    static constexpr akshara::fixed_string a{"abc"};
    static constexpr akshara::fixed_string b{"abd"};
    static constexpr akshara::fixed_string c{"abc"};
    STATIC_REQUIRE((a <=> b) == std::strong_ordering::less);
    STATIC_REQUIRE((b <=> a) == std::strong_ordering::greater);
    STATIC_REQUIRE((a <=> c) == std::strong_ordering::equal);
}

TEST_CASE (

"fixed_string: operator+ concatenation"
,
"[akshara][fixed_string]"
)
 {
    static constexpr akshara::fixed_string a{"foo"};
    static constexpr akshara::fixed_string b{"bar"};
    static constexpr auto ab = a + b;
    STATIC_REQUIRE(ab == akshara::fixed_string{"foobar"});
    STATIC_REQUIRE(ab.length == 6);
}

TEST_CASE (

"fixed_string: operator+ with char"
,
"[akshara][fixed_string]"
)
 {
    static constexpr akshara::fixed_string s{"ab"};
    static constexpr auto sc = s + 'c';
    STATIC_REQUIRE(sc == akshara::fixed_string{"abc"});

    static constexpr auto cs = 'z' + s;
    STATIC_REQUIRE(cs == akshara::fixed_string{"zab"});
}

// ============================================================================
// SECTION 2: char classifiers — detail::fs
// ============================================================================

TEST_CASE (

"detail::fs: is_upper / is_lower / is_alpha"
,
"[akshara][classifiers]"
)
 {
    using namespace akshara::detail::fs;
    STATIC_REQUIRE(is_upper('A'));
    STATIC_REQUIRE(is_upper('Z'));
    STATIC_REQUIRE(!is_upper('a'));
    STATIC_REQUIRE(!is_upper('5'));

    STATIC_REQUIRE(is_lower('a'));
    STATIC_REQUIRE(is_lower('z'));
    STATIC_REQUIRE(!is_lower('A'));

    STATIC_REQUIRE(is_alpha('A'));
    STATIC_REQUIRE(is_alpha('z'));
    STATIC_REQUIRE(!is_alpha('5'));
    STATIC_REQUIRE(!is_alpha('!'));
}

TEST_CASE (

"detail::fs: is_digit / is_alnum / is_hex"
,
"[akshara][classifiers]"
)
 {
    using namespace akshara::detail::fs;
    STATIC_REQUIRE(is_digit('0'));
    STATIC_REQUIRE(is_digit('9'));
    STATIC_REQUIRE(!is_digit('a'));

    STATIC_REQUIRE(is_alnum('5'));
    STATIC_REQUIRE(is_alnum('Z'));
    STATIC_REQUIRE(!is_alnum('-'));

    STATIC_REQUIRE(is_hex('0'));
    STATIC_REQUIRE(is_hex('f'));
    STATIC_REQUIRE(is_hex('F'));
    STATIC_REQUIRE(!is_hex('g'));
    STATIC_REQUIRE(!is_hex('G'));
}

TEST_CASE (

"detail::fs: is_space"
,
"[akshara][classifiers]"
)
 {
    using namespace akshara::detail::fs;
    STATIC_REQUIRE(is_space(' '));
    STATIC_REQUIRE(is_space('\t'));
    STATIC_REQUIRE(is_space('\n'));
    STATIC_REQUIRE(is_space('\r'));
    STATIC_REQUIRE(!is_space('a'));
    STATIC_REQUIRE(!is_space('0'));
}

TEST_CASE (

"detail::fs: is_ident_start / is_ident_cont"
,
"[akshara][classifiers]"
)
 {
    using namespace akshara::detail::fs;
    STATIC_REQUIRE(is_ident_start('_'));
    STATIC_REQUIRE(is_ident_start('A'));
    STATIC_REQUIRE(is_ident_start('z'));
    STATIC_REQUIRE(!is_ident_start('5'));
    STATIC_REQUIRE(!is_ident_start('-'));

    STATIC_REQUIRE(is_ident_cont('_'));
    STATIC_REQUIRE(is_ident_cont('5'));
    STATIC_REQUIRE(is_ident_cont('A'));
    STATIC_REQUIRE(!is_ident_cont('-'));
}

TEST_CASE (

"detail::fs: is_print / is_punct"
,
"[akshara][classifiers]"
)
 {
    using namespace akshara::detail::fs;
    STATIC_REQUIRE(is_print('A'));
    STATIC_REQUIRE(is_print('!'));
    STATIC_REQUIRE(!is_print('\n'));
    STATIC_REQUIRE(!is_print('\x01'));

    STATIC_REQUIRE(is_punct('!'));
    STATIC_REQUIRE(is_punct('.'));
    STATIC_REQUIRE(!is_punct(' '));
    STATIC_REQUIRE(!is_punct('a'));
}

TEST_CASE (

"detail::fs: to_upper / to_lower"
,
"[akshara][classifiers]"
)
 {
    using namespace akshara::detail::fs;
    STATIC_REQUIRE(to_upper('a') == 'A');
    STATIC_REQUIRE(to_upper('Z') == 'Z');
    STATIC_REQUIRE(to_upper('5') == '5');

    STATIC_REQUIRE(to_lower('A') == 'a');
    STATIC_REQUIRE(to_lower('z') == 'z');
    STATIC_REQUIRE(to_lower('5') == '5');
}

// ============================================================================
// SECTION 3: String algorithms
// ============================================================================

TEST_CASE (

"substr"
,
"[akshara][algorithms]"
)
 {
    static constexpr akshara::fixed_string s{"hello world"};
    static constexpr auto h = akshara::substr<0, 5>(s);
    STATIC_REQUIRE(h == akshara::fixed_string{"hello"});

    static constexpr auto w = akshara::substr<6, 5>(s);
    STATIC_REQUIRE(w == akshara::fixed_string{"world"});
}

TEST_CASE (

"find_char / rfind_char / contains_char"
,
"[akshara][algorithms]"
)
 {
    static constexpr akshara::fixed_string s{"hello"};
    STATIC_REQUIRE(akshara::find_char(s, 'l') == 2);
    STATIC_REQUIRE(akshara::rfind_char(s, 'l') == 3);
    STATIC_REQUIRE(akshara::find_char(s, 'z') == std::string_view::npos);
    STATIC_REQUIRE(akshara::contains_char(s, 'e'));
    STATIC_REQUIRE(!akshara::contains_char(s, 'z'));
}

TEST_CASE (

"find_substr"
,
"[akshara][algorithms]"
)
 {
    static constexpr akshara::fixed_string hay{"hello world"};
    static constexpr akshara::fixed_string n1{"world"};
    static constexpr akshara::fixed_string n2{"xyz"};
    STATIC_REQUIRE(akshara::find_substr(hay, n1) == 6);
    STATIC_REQUIRE(akshara::find_substr(hay, n2) == std::string_view::npos);
}

TEST_CASE (

"starts_with / ends_with"
,
"[akshara][algorithms]"
)
 {
    static constexpr akshara::fixed_string s{"foobar"};
    static constexpr akshara::fixed_string pre{"foo"};
    static constexpr akshara::fixed_string suf{"bar"};
    static constexpr akshara::fixed_string no{"baz"};
    STATIC_REQUIRE(akshara::starts_with(s, pre));
    STATIC_REQUIRE(!akshara::starts_with(s, suf));
    STATIC_REQUIRE(akshara::ends_with(s, suf));
    STATIC_REQUIRE(!akshara::ends_with(s, pre));
    STATIC_REQUIRE(!akshara::ends_with(s, no));
}

TEST_CASE (

"to_upper / to_lower"
,
"[akshara][algorithms]"
)
 {
    static constexpr akshara::fixed_string s{"Hello"};
    static constexpr auto up = akshara::to_upper(s);
    STATIC_REQUIRE(up == akshara::fixed_string{"HELLO"});

    static constexpr auto lo = akshara::to_lower(s);
    STATIC_REQUIRE(lo == akshara::fixed_string{"hello"});
}

TEST_CASE (

"replace_char"
,
"[akshara][algorithms]"
)
 {
    static constexpr akshara::fixed_string s{"foo_bar"};
    static constexpr auto r = akshara::replace_char(s, '_', '-');
    STATIC_REQUIRE(r == akshara::fixed_string{"foo-bar"});
}

TEST_CASE (

"repeat"
,
"[akshara][algorithms]"
)
 {
    static constexpr akshara::fixed_string unit{"ab"};
    static constexpr auto r3 = akshara::repeat<3>(unit);
    STATIC_REQUIRE(r3 == akshara::fixed_string{"ababab"});

    static constexpr auto r1 = akshara::repeat<1>(unit);
    STATIC_REQUIRE(r1 == unit);
}

TEST_CASE (

"trim_view"
,
"[akshara][algorithms]"
)
 {
    static constexpr akshara::fixed_string s{"  hello  "};
    constexpr auto tv = akshara::trim_view(s);
    STATIC_REQUIRE(tv == "hello");

    static constexpr akshara::fixed_string ws{"   "};
    constexpr auto empty = akshara::trim_view(ws);
    STATIC_REQUIRE(empty.empty());
}

TEST_CASE (

"uint_to_str / str_to_uint"
,
"[akshara][algorithms]"
)
 {
    static constexpr auto s0 = akshara::uint_to_str<0>();
    static constexpr auto s42 = akshara::uint_to_str<42>();
    static constexpr auto s100 = akshara::uint_to_str<100>();
    STATIC_REQUIRE(s0.view() == "0");
    STATIC_REQUIRE(s42.view() == "42");
    STATIC_REQUIRE(s100.view() == "100");

    static constexpr akshara::fixed_string dec{"1234"};
    STATIC_REQUIRE(akshara::str_to_uint(dec) == 1234);
}

// ============================================================================
// SECTION 4: ct_string_builder
// ============================================================================

TEST_CASE (

"ct_string_builder: push and append"
,
"[akshara][builder]"
)
 {
    static constexpr auto result = []() consteval {
        akshara::ct_string_builder<32> b;
        b.append(akshara::fixed_string{"hello"});
        b.push('_');
        b.append(akshara::fixed_string{"world"});
        return b.build<11>();
    }();
    STATIC_REQUIRE(result == akshara::fixed_string{"hello_world"});
}

TEST_CASE (

"ct_string_builder: size tracking"
,
"[akshara][builder]"
)
 {
    static constexpr std::size_t sz = []() consteval {
        akshara::ct_string_builder<16> b;
        b.append(akshara::fixed_string{"abc"});
        b.push('d');
        return b.size();
    }();
    STATIC_REQUIRE(sz == 4);
}

TEST_CASE (

"ct_string_builder: string_view append"
,
"[akshara][builder]"
)
 {
    static constexpr auto result = []() consteval {
        akshara::ct_string_builder<16> b;
        b.append(std::string_view{"xyz"});
        return b.build<3>();
    }();
    STATIC_REQUIRE(result == akshara::fixed_string{"xyz"});
}

// ============================================================================
// SECTION 5: KMP search
// ============================================================================

TEST_CASE (

"kmp_find: basic cases"
,
"[akshara][kmp]"
)
 {
    static constexpr akshara::fixed_string hay{"abcabcabc"};
    static constexpr akshara::fixed_string n1{"abc"};
    static constexpr akshara::fixed_string n2{"cab"};
    static constexpr akshara::fixed_string n3{"xyz"};
    STATIC_REQUIRE(akshara::kmp_find(hay, n1) == 0);
    STATIC_REQUIRE(akshara::kmp_find(hay, n2) == 2);
    STATIC_REQUIRE(akshara::kmp_find(hay, n3) == std::string_view::npos);
}

TEST_CASE (

"kmp_find: edge cases — empty needle, needle longer than hay"
,
"[akshara][kmp]"
)
 {
    static constexpr akshara::fixed_string hay{"hello"};
    static constexpr akshara::fixed_string empty{""};
    static constexpr akshara::fixed_string longer{"hello world"};
    STATIC_REQUIRE(akshara::kmp_find(hay, empty) == 0);
    STATIC_REQUIRE(akshara::kmp_find(hay, longer) == std::string_view::npos);
}

TEST_CASE (

"kmp_count: non-overlapping occurrences"
,
"[akshara][kmp]"
)
 {
    static constexpr akshara::fixed_string hay{"abababab"};
    static constexpr akshara::fixed_string n{"ab"};
    STATIC_REQUIRE(akshara::kmp_count(hay, n) == 4);

    static constexpr akshara::fixed_string n2{"xyz"};
    STATIC_REQUIRE(akshara::kmp_count(hay, n2) == 0);
}

TEST_CASE (

"kmp_count: single occurrence"
,
"[akshara][kmp]"
)
 {
    static constexpr akshara::fixed_string hay{"hello world"};
    static constexpr akshara::fixed_string n{"world"};
    STATIC_REQUIRE(akshara::kmp_count(hay, n) == 1);
}

// ============================================================================
// SECTION 6: join
// ============================================================================

TEST_CASE (

"join: two fixed_strings with separator"
,
"[akshara][join]"
)
 {
    static constexpr akshara::fixed_string sep{"::"};
    static constexpr akshara::fixed_string a{"foo"};
    static constexpr akshara::fixed_string b{"bar"};
    static constexpr auto r = akshara::join(sep, a, b);
    STATIC_REQUIRE(r == akshara::fixed_string{"foo::bar"});
}

TEST_CASE (

"join: single-char separator"
,
"[akshara][join]"
)
 {
    static constexpr akshara::fixed_string sep{"/"};
    static constexpr akshara::fixed_string a{"usr"};
    static constexpr akshara::fixed_string b{"lib"};
    static constexpr auto r = akshara::join(sep, a, b);
    STATIC_REQUIRE(r == akshara::fixed_string{"usr/lib"});
}

// ============================================================================
// SECTION 7: ct_char_set
// ============================================================================

TEST_CASE (

"ct_char_set: construction from fixed_string"
,
"[akshara][char_set]"
)
 {
    static constexpr auto vowels = akshara::ct_char_set{akshara::fixed_string{"aeiou"}};
    STATIC_REQUIRE(vowels.contains('a'));
    STATIC_REQUIRE(vowels.contains('e'));
    STATIC_REQUIRE(vowels.contains('u'));
    STATIC_REQUIRE(!vowels.contains('b'));
    STATIC_REQUIRE(!vowels.contains('z'));
}

TEST_CASE (

"ct_char_set: factory functions"
,
"[akshara][char_set]"
)
 {
    static constexpr auto d = akshara::cs_digits();
    STATIC_REQUIRE(d.contains('0'));
    STATIC_REQUIRE(d.contains('9'));
    STATIC_REQUIRE(!d.contains('a'));

    static constexpr auto u = akshara::cs_upper();
    STATIC_REQUIRE(u.contains('A'));
    STATIC_REQUIRE(u.contains('Z'));
    STATIC_REQUIRE(!u.contains('a'));

    static constexpr auto l = akshara::cs_lower();
    STATIC_REQUIRE(l.contains('a'));
    STATIC_REQUIRE(!l.contains('A'));

    static constexpr auto h = akshara::cs_hex();
    STATIC_REQUIRE(h.contains('0'));
    STATIC_REQUIRE(h.contains('f'));
    STATIC_REQUIRE(h.contains('F'));
    STATIC_REQUIRE(!h.contains('g'));

    static constexpr auto ws = akshara::cs_whitespace();
    STATIC_REQUIRE(ws.contains(' '));
    STATIC_REQUIRE(ws.contains('\t'));
    STATIC_REQUIRE(ws.contains('\n'));
    STATIC_REQUIRE(!ws.contains('a'));
}

TEST_CASE (

"ct_char_set: ident_start / ident_cont"
,
"[akshara][char_set]"
)
 {
    static constexpr auto is = akshara::cs_ident_start();
    STATIC_REQUIRE(is.contains('_'));
    STATIC_REQUIRE(is.contains('A'));
    STATIC_REQUIRE(is.contains('z'));
    STATIC_REQUIRE(!is.contains('5'));
    STATIC_REQUIRE(!is.contains('-'));

    static constexpr auto ic = akshara::cs_ident_cont();
    STATIC_REQUIRE(ic.contains('_'));
    STATIC_REQUIRE(ic.contains('5'));
    STATIC_REQUIRE(ic.contains('Z'));
    STATIC_REQUIRE(!ic.contains('-'));
    STATIC_REQUIRE(!ic.contains(' '));
}

TEST_CASE (

"ct_char_set: bitwise union operator|"
,
"[akshara][char_set]"
)
 {
    static constexpr auto d = akshara::cs_digits();
    static constexpr auto l = akshara::cs_lower();
    static constexpr auto dl = d | l;
    STATIC_REQUIRE(dl.contains('5'));
    STATIC_REQUIRE(dl.contains('z'));
    STATIC_REQUIRE(!dl.contains('A'));
}

TEST_CASE (

"ct_char_set: bitwise intersection operator&"
,
"[akshara][char_set]"
)
 {
    static constexpr auto al = akshara::cs_alnum();
    static constexpr auto lo = akshara::cs_lower();
    static constexpr auto res = al & lo;
    STATIC_REQUIRE(res.contains('z'));
    STATIC_REQUIRE(!res.contains('A')); // upper not in lower
    STATIC_REQUIRE(!res.contains('5')); // digit not in lower
}

TEST_CASE (

"ct_char_set: bitwise xor operator^"
,
"[akshara][char_set]"
)
 {
    static constexpr auto al = akshara::cs_alpha();
    static constexpr auto lo = akshara::cs_lower();
    static constexpr auto up_only = al ^ lo; // alpha XOR lower = upper only
    STATIC_REQUIRE(up_only.contains('A'));
    STATIC_REQUIRE(up_only.contains('Z'));
    STATIC_REQUIRE(!up_only.contains('a')); // in both alpha and lower → removed
}

TEST_CASE (

"ct_char_set: complement"
,
"[akshara][char_set]"
)
 {
    static constexpr auto d = akshara::cs_digits();
    static constexpr auto nd = d.complement();
    STATIC_REQUIRE(!nd.contains('0'));
    STATIC_REQUIRE(!nd.contains('9'));
    STATIC_REQUIRE(nd.contains('a'));
    STATIC_REQUIRE(nd.contains(' '));
}

TEST_CASE (

"ct_char_set: add_range"
,
"[akshara][char_set]"
)
 {
    static constexpr auto s = []() consteval {
        akshara::ct_char_set cs;
        cs.add_range('a', 'f');
        return cs;
    }();
    STATIC_REQUIRE(s.contains('a'));
    STATIC_REQUIRE(s.contains('f'));
    STATIC_REQUIRE(!s.contains('g'));
    STATIC_REQUIRE(!s.contains('A'));
}

TEST_CASE (

"ct_char_set: add single char"
,
"[akshara][char_set]"
)
 {
    static constexpr auto s = []() consteval {
        akshara::ct_char_set cs;
        cs.add('X');
        cs.add('_');
        return cs;
    }();
    STATIC_REQUIRE(s.contains('X'));
    STATIC_REQUIRE(s.contains('_'));
    STATIC_REQUIRE(!s.contains('Y'));
}

TEST_CASE (

"ct_char_set: NTTP usability"
,
"[akshara][char_set]"
)
 {
    static constexpr akshara::ct_char_set vowels{akshara::fixed_string{"aeiou"}};
    // ct_char_set is a structural type — usable as NTTP
    []<akshara::ct_char_set CS>() {
        STATIC_REQUIRE(CS.contains('a'));
        STATIC_REQUIRE(!CS.contains('b'));
    }.template operator()<vowels>();
}

// ============================================================================
// SECTION 8: fnv1a64
// ============================================================================

TEST_CASE (

"fnv1a64: string_view overload"
,
"[akshara][hash]"
)
 {
    constexpr auto h1 = akshara::fnv1a64(std::string_view{"hello"});
    constexpr auto h2 = akshara::fnv1a64(std::string_view{"hello"});
    constexpr auto h3 = akshara::fnv1a64(std::string_view{"world"});
    STATIC_REQUIRE(h1 == h2);
    STATIC_REQUIRE(h1 != h3);
    STATIC_REQUIRE(h1 != 0);
}

TEST_CASE (

"fnv1a64: fixed_string overload"
,
"[akshara][hash]"
)
 {
    static constexpr akshara::fixed_string s{"hello"};
    constexpr auto h = akshara::fnv1a64(s);
    STATIC_REQUIRE(h == akshara::fnv1a64(std::string_view{"hello"}));
}

TEST_CASE (

"fnv1a64: empty string"
,
"[akshara][hash]"
)
 {
    constexpr auto h = akshara::fnv1a64(std::string_view{""});
    STATIC_REQUIRE(h != 0); // FNV offset basis, never zero
}

TEST_CASE (

"fnv1a64: distinct values for different strings"
,
"[akshara][hash]"
)
 {
    constexpr auto h_a = akshara::fnv1a64(std::string_view{"a"});
    constexpr auto h_b = akshara::fnv1a64(std::string_view{"b"});
    STATIC_REQUIRE(h_a != h_b);
}

// ============================================================================
// SECTION 9: Padding
// ============================================================================

TEST_CASE (

"pad_right: basic usage"
,
"[akshara][padding]"
)
 {
    static constexpr akshara::fixed_string s{"hi"};
    static constexpr auto p = akshara::pad_right<8>(s);
    STATIC_REQUIRE(p.length == 8);
    STATIC_REQUIRE(p.view() == "hi      ");
}

TEST_CASE (

"pad_right: custom fill char"
,
"[akshara][padding]"
)
 {
    static constexpr akshara::fixed_string s{"hi"};
    static constexpr auto p = akshara::pad_right<6>(s, '-');
    STATIC_REQUIRE(p.view() == "hi----");
}

TEST_CASE (

"pad_right: already at width"
,
"[akshara][padding]"
)
 {
    static constexpr akshara::fixed_string s{"hi"};
    static constexpr auto p = akshara::pad_right<2>(s);
    STATIC_REQUIRE(p.view() == "hi");
}

TEST_CASE (

"pad_left: basic usage"
,
"[akshara][padding]"
)
 {
    static constexpr akshara::fixed_string s{"hi"};
    static constexpr auto p = akshara::pad_left<8>(s);
    STATIC_REQUIRE(p.length == 8);
    STATIC_REQUIRE(p.view() == "      hi");
}

TEST_CASE (

"pad_left: custom fill char"
,
"[akshara][padding]"
)
 {
    static constexpr akshara::fixed_string s{"42"};
    static constexpr auto p = akshara::pad_left<5>(s, '0');
    STATIC_REQUIRE(p.view() == "00042");
}

// ============================================================================
// SECTION 10: intern_tag / intern_equal
// ============================================================================

TEST_CASE (

"intern_tag: same string same type"
,
"[akshara][intern]"
)
 {
    using T1 = akshara::intern_tag<"hello">;
    using T2 = akshara::intern_tag<"hello">;
    STATIC_REQUIRE(std::is_same_v<T1, T2>);
}

TEST_CASE (

"intern_tag: different strings different types"
,
"[akshara][intern]"
)
 {
    using T1 = akshara::intern_tag<"hello">;
    using T2 = akshara::intern_tag<"world">;
    STATIC_REQUIRE(!std::is_same_v<T1, T2>);
}

TEST_CASE (

"intern_tag: str() / hash() / length()"
,
"[akshara][intern]"
)
 {
    using T = akshara::intern_tag<"hello">;
    STATIC_REQUIRE(T::str() == "hello");
    STATIC_REQUIRE(T::hash() == akshara::fnv1a64(std::string_view{"hello"}));
    STATIC_REQUIRE(T::length() == 5);
}

TEST_CASE (

"intern_tag: same_as()"
,
"[akshara][intern]"
)
 {
    using T1 = akshara::intern_tag<"x">;
    using T2 = akshara::intern_tag<"y">;
    STATIC_REQUIRE(T1::same_as(T1{}));
    STATIC_REQUIRE(!T1::same_as(T2{}));
}

TEST_CASE (

"intern_equal variable template"
,
"[akshara][intern]"
)
 {
    STATIC_REQUIRE(akshara::intern_equal<"hello", "hello">);
    STATIC_REQUIRE(!akshara::intern_equal<"hello", "world">);
}

// ============================================================================
// SECTION 11: path
// ============================================================================

TEST_CASE (

"path::filename"
,
"[akshara][path]"
)
 {
    static constexpr akshara::fixed_string p1{"src/parser/tokenizer.cpp"};
    STATIC_REQUIRE(akshara::path::filename(p1) == "tokenizer.cpp");

    static constexpr akshara::fixed_string p2{"README.md"};
    STATIC_REQUIRE(akshara::path::filename(p2) == "README.md");

    static constexpr akshara::fixed_string p3{"a/b/c/d.txt"};
    STATIC_REQUIRE(akshara::path::filename(p3) == "d.txt");
}

TEST_CASE (

"path::stem"
,
"[akshara][path]"
)
 {
    static constexpr akshara::fixed_string p1{"src/foo.cpp"};
    STATIC_REQUIRE(akshara::path::stem(p1) == "foo");

    static constexpr akshara::fixed_string p2{"README.md"};
    STATIC_REQUIRE(akshara::path::stem(p2) == "README");

    // no extension
    static constexpr akshara::fixed_string p3{"Makefile"};
    STATIC_REQUIRE(akshara::path::stem(p3) == "Makefile");
}

TEST_CASE (

"path::extension"
,
"[akshara][path]"
)
 {
    static constexpr akshara::fixed_string p1{"foo.cpp"};
    STATIC_REQUIRE(akshara::path::extension(p1) == ".cpp");

    static constexpr akshara::fixed_string p2{"archive.tar.gz"};
    STATIC_REQUIRE(akshara::path::extension(p2) == ".gz");

    static constexpr akshara::fixed_string p3{"Makefile"};
    STATIC_REQUIRE(akshara::path::extension(p3) == "");
}

TEST_CASE (

"path::parent_path"
,
"[akshara][path]"
)
 {
    static constexpr akshara::fixed_string p1{"src/parser/tokenizer.cpp"};
    STATIC_REQUIRE(akshara::path::parent_path(p1) == "src/parser");

    static constexpr akshara::fixed_string p2{"foo.cpp"};
    STATIC_REQUIRE(akshara::path::parent_path(p2) == "");

    static constexpr akshara::fixed_string p3{"a/b"};
    STATIC_REQUIRE(akshara::path::parent_path(p3) == "a");
}

TEST_CASE (

"path::normalize: strip ./ prefix"
,
"[akshara][path]"
)
 {
    static constexpr akshara::fixed_string p1{"./src/foo.cpp"};
    STATIC_REQUIRE(akshara::path::normalize(p1) == "src/foo.cpp");

    static constexpr akshara::fixed_string p2{"././bar.hpp"};
    STATIC_REQUIRE(akshara::path::normalize(p2) == "bar.hpp");
}

TEST_CASE (

"path::normalize: strip trailing /"
,
"[akshara][path]"
)
 {
    static constexpr akshara::fixed_string p1{"src/"};
    STATIC_REQUIRE(akshara::path::normalize(p1) == "src");

    static constexpr akshara::fixed_string p2{"src/lib/"};
    STATIC_REQUIRE(akshara::path::normalize(p2) == "src/lib");
}

TEST_CASE (

"path::normalize: no-op on clean path"
,
"[akshara][path]"
)
 {
    static constexpr akshara::fixed_string p{"src/foo.cpp"};
    STATIC_REQUIRE(akshara::path::normalize(p) == "src/foo.cpp");
}

// ============================================================================
// Composite: NTTP string dispatch pattern
// ============================================================================

TEST_CASE (

"fixed_string as NTTP — dispatch via if constexpr"
,
"[akshara][composite]"
)
 {
    auto dispatch = []<akshara::fixed_string Tag>() -> std::string_view {
        if constexpr (akshara::intern_equal<Tag, "read">) return "read-path";
        if constexpr (akshara::intern_equal<Tag, "write">) return "write-path";
        return "unknown";
    };

    CHECK(dispatch.operator()<"read">() == "read-path");
    CHECK(dispatch.operator()<"write">() == "write-path");
    CHECK(dispatch.operator()<"exec">() == "unknown");
}

TEST_CASE (

"ct_char_set as NTTP — lexer pattern"
,
"[akshara][composite]"
)
 {
    static constexpr akshara::ct_char_set alpha_under =
        akshara::cs_alpha() | []() consteval {
            akshara::ct_char_set s;
            s.add('_');
            return s;
        }();

    auto scan_ident = [&](std::string_view src) {
        std::size_t i = 0;
        while (i < src.size() && alpha_under.contains(src[i])) ++i;
        return src.substr(0, i);
    };

    CHECK(scan_ident("my_var + 1") == "my_var");
    CHECK(scan_ident("_private123") == "_private");
    CHECK(scan_ident("123bad") == "");
}
