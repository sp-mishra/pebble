# Akshara — Compile-Time String Library

**Header**: `include/meta/akshara.hpp`  
**Namespace**: `akshara`  
**Standard**: C++23, header-only  
**Dependencies**: `<array>`, `<compare>`, `<cstddef>`, `<cstdint>`, `<string_view>` — no project dependencies

---

## Table of Contents

1. [Overview](#overview)
2. [Design Principles](#design-principles)
3. [Architecture](#architecture)
4. [Section 1 — fixed_string](#section-1--fixed_string)
5. [Section 2 — Character Classifiers (detail::fs)](#section-2--character-classifiers-detailfs)
6. [Section 3 — String Algorithms](#section-3--string-algorithms)
7. [Section 4 — ct_string_builder](#section-4--ct_string_builder)
8. [Section 5 — KMP Search](#section-5--kmp-search)
9. [Section 6 — join](#section-6--join)
10. [Section 7 — ct_char_set](#section-7--ct_char_set)
11. [Section 8 — FNV-1a Hash](#section-8--fnv-1a-hash)
12. [Section 9 — Padding](#section-9--padding)
13. [Section 10 — String Interning](#section-10--string-interning)
14. [Section 11 — Compile-Time Path Operations](#section-11--compile-time-path-operations)
15. [Backward Compatibility with meta.hpp](#backward-compatibility-with-metahpp)
16. [Usage Examples](#usage-examples)

---

## Overview

Akshara ("अक्षर", Sanskrit for "character" or "letter") is the compile-time string foundation. It provides:

- An NTTP-capable string type (`fixed_string<N>`) with full STL/ranges compliance
- A full suite of compile-time string algorithms (search, split, replace, case conversion, padding)
- A compile-time character set type (`ct_char_set`) backed by a two-word bitset for efficient constexpr evaluation
- KMP string search with O (N+M) compile-time complexity
- FNV-1a 64-bit hashing for compile-time dispatch
- Type-level string interning for O (1) compile-time identity comparison
- Compile-time path operations (`akshara::path`)

Everything is `consteval` or `constexpr`; no runtime overhead, no heap, no virtual, no macros.

---

## Design Principles

**Self-contained foundation.** Akshara has zero project-level dependencies.

**NTTP-first.** `fixed_string<N>` satisfies the structural type requirements for use as a non-type template parameter.

**STL-compatible.** `fixed_string<N>` now provides `begin()`/`end()`, `size()`, `empty()`, and
`operator std::string_view()`. It works with `std::ranges::equal`, range-for, and any algorithm expecting an iterator
range.

**All operations are consteval.** Every function is `consteval` (immediate) unless it must be `constexpr` for both
compile-time and runtime use.

**No size erase.** Operations that produce a string return `fixed_string<ResultSize>` with size in the type.

---

## Architecture

```
akshara.hpp
├── fixed_string<N>              — NTTP-capable, STL-range-compatible string
├── namespace detail::fs         — char predicates and low-level helpers
├── String algorithms            — substr, find, replace, case, trim, ...
├── ct_string_builder<Capacity>  — mutable compile-time string accumulator
├── kmp_find / kmp_count         — KMP substring search
├── join                         — concatenate with separator
├── ct_char_set                  — 128-bit ASCII bitset (two uint64_t words)
├── fnv1a64                      — FNV-1a 64-bit hash
├── pad_right / pad_left         — fixed-width padding
├── intern_tag / intern_equal    — type-level string identity
└── namespace path               — compile-time path operations
```

---

## Section 1 — fixed_string

```cpp
template <std::size_t N>
struct fixed_string {
    char data[N]{};
    static constexpr std::size_t length = N - 1;  // excludes null terminator

    consteval fixed_string(const char (&str)[N]) noexcept;
    consteval fixed_string() noexcept = default;

    // STL/ranges interface
    [[nodiscard]] constexpr const char* begin()   const noexcept;
    [[nodiscard]] constexpr const char* end()     const noexcept;
    [[nodiscard]] constexpr const char* cbegin()  const noexcept;
    [[nodiscard]] constexpr const char* cend()    const noexcept;
    [[nodiscard]] constexpr std::size_t size()    const noexcept;  // == length
    [[nodiscard]] constexpr bool        empty()   const noexcept;

    // Implicit conversion — enables: std::string_view sv = str;
    [[nodiscard]] constexpr operator std::string_view() const noexcept;

    // Kept for backward compatibility
    [[nodiscard]] consteval std::string_view view() const noexcept;

    [[nodiscard]] consteval char operator[](std::size_t i) const noexcept;
    consteval bool operator==(const fixed_string&) const noexcept;

    // Concatenation: fixed_string<N> + fixed_string<M> → fixed_string<N+M-1>
    template <std::size_t M>
    consteval fixed_string<N + M - 1> operator+(const fixed_string<M>& o) const noexcept;
};

// CTAD: fixed_string{"hello"} deduces fixed_string<6>
template <std::size_t N>
fixed_string(const char (&)[N]) -> fixed_string<N>;
```

**Key notes:**

- `N` includes the null terminator; `length == N - 1` is the character count.
- Structural type — suitable as NTTP.
- `operator std::string_view()` enables zero-copy interop with any API accepting `string_view`.
- `static constexpr` storage required when the pointer is used in constant expressions (see KMP section).

---

## Section 2 — Character Classifiers (detail::fs)

All functions in `namespace akshara::detail::fs`. Each is `constexpr`.

| Function            | Returns true when                         |
|---------------------|-------------------------------------------|
| `is_upper(c)`       | `'A'`–`'Z'`                               |
| `is_lower(c)`       | `'a'`–`'z'`                               |
| `is_alpha(c)`       | `is_upper(c)                              || is_lower(c)` |
| `is_digit(c)`       | `'0'`–`'9'`                               |
| `is_alnum(c)`       | `is_alpha(c)                              || is_digit(c)` |
| `is_space(c)`       | space, tab, newline, CR, FF, VT           |
| `is_hex(c)`         | digits + `a-f` + `A-F`                    |
| `is_ident_start(c)` | `is_alpha(c)                              || c == '_'` |
| `is_ident_cont(c)`  | `is_alnum(c)                              || c == '_'` |
| `is_print(c)`       | `0x20`–`0x7E`                             |
| `is_punct(c)`       | `is_print(c) && !is_alnum(c) && c != ' '` |

---

## Section 3 — String Algorithms

All functions in `namespace akshara`, all `consteval`.

### Extraction

```cpp
template <std::size_t Start, std::size_t Len, std::size_t N>
consteval fixed_string<Len + 1> substr(const fixed_string<N>& s) noexcept;
```

### Search

```cpp
consteval std::size_t find_char(const fixed_string<N>& s, char c) noexcept;
consteval std::size_t rfind_char(const fixed_string<N>& s, char c) noexcept;
consteval bool contains_char(const fixed_string<N>& s, char c) noexcept;
consteval std::size_t find_substr(const fixed_string<N>& hay, const fixed_string<M>& needle) noexcept;
consteval bool starts_with(const fixed_string<N>& s, const fixed_string<M>& prefix) noexcept;
consteval bool ends_with(const fixed_string<N>& s, const fixed_string<M>& suffix) noexcept;
```

### Transformation

```cpp
consteval fixed_string<N> to_upper(const fixed_string<N>& s) noexcept;
consteval fixed_string<N> to_lower(const fixed_string<N>& s) noexcept;
consteval fixed_string<N> replace_char(const fixed_string<N>& s, char from, char to) noexcept;
template <std::size_t Count, std::size_t N>
consteval auto repeat(const fixed_string<N>& s) noexcept;
constexpr std::string_view trim_view(const fixed_string<N>& s) noexcept;
```

### Integer conversion

```cpp
template <std::size_t V>
consteval auto uint_to_str() noexcept;
template <std::size_t N>
consteval std::size_t str_to_uint(const fixed_string<N>& s) noexcept;
```

---

## Section 4 — ct_string_builder

A mutable compile-time string accumulator.

```cpp
template <std::size_t Capacity>
struct ct_string_builder {
    consteval void push(char c) noexcept;
    template <std::size_t N>
    consteval void append(const fixed_string<N>& s) noexcept;
    consteval void append(std::string_view sv) noexcept;
    template <std::size_t N>
    consteval fixed_string<N> build() const noexcept;  // N = chars pushed (not including NUL)
    consteval std::size_t size() const noexcept;
};
```

---

## Section 5 — KMP Search

O (N+M) compile-time substring search.

```cpp
consteval std::size_t kmp_find(const fixed_string<N>& haystack, const fixed_string<M>& needle) noexcept;
consteval std::size_t kmp_count(const fixed_string<N>& haystack, const fixed_string<M>& needle) noexcept;
```

*Note:* Variables passed to `consteval` functions whose `.data` pointer matters must be `static constexpr`.

---

## Section 6 — join

```cpp
consteval auto join(const fixed_string<SN>& sep,
                    const fixed_string<AN>& a,
                    const fixed_string<BN>& b) noexcept;
// Returns fixed_string<AN + SN + BN - 2>
```

---

## Section 7 — ct_char_set

A compile-time bitset over ASCII (0–127). Structural type — usable as NTTP.

**Implementation**: two `uint64_t` words (`low` for bits 0–63, `high` for 64–127). All set operations are single bitwise
instructions — ~64x smaller than the old `bool[128]` representation and much faster to evaluate at compile time.

```cpp
struct ct_char_set {
    uint64_t low  = 0;  // bits 0–63
    uint64_t high = 0;  // bits 64–127

    consteval ct_char_set() noexcept = default;

    template <std::size_t N>
    consteval explicit ct_char_set(const fixed_string<N>& chars) noexcept;

    [[nodiscard]] consteval bool contains(char c) const noexcept;
    [[nodiscard]] consteval ct_char_set operator|(const ct_char_set& o) const noexcept;
    [[nodiscard]] consteval ct_char_set operator&(const ct_char_set& o) const noexcept;
    [[nodiscard]] consteval ct_char_set operator^(const ct_char_set& o) const noexcept;
    [[nodiscard]] consteval ct_char_set complement() const noexcept;
    consteval void add_range(char lo, char hi) noexcept;
    consteval void add(char c) noexcept;
};
```

**Predefined factory functions** (all `consteval`):

| Function           | Characters included                |
|--------------------|------------------------------------|
| `cs_digits()`      | `'0'`–`'9'`                        |
| `cs_upper()`       | `'A'`–`'Z'`                        |
| `cs_lower()`       | `'a'`–`'z'`                        |
| `cs_alpha()`       | `cs_upper()                        | cs_lower()` |
| `cs_alnum()`       | `cs_alpha()                        | cs_digits()` |
| `cs_whitespace()`  | space, tab, `\n`, `\r`, `\f`, `\v` |
| `cs_hex()`         | digits + `a-f` + `A-F`             |
| `cs_ident_start()` | alpha + `_`                        |
| `cs_ident_cont()`  | alnum + `_`                        |

---

## Section 8 — FNV-1a Hash

```cpp
consteval std::uint64_t fnv1a64(std::string_view sv) noexcept;
template <std::size_t N>
consteval std::uint64_t fnv1a64(const fixed_string<N>& s) noexcept;
```

---

## Section 9 — Padding

```cpp
template <std::size_t Width, std::size_t N>
    requires(Width + 1 >= N)
consteval fixed_string<Width + 1> pad_right(const fixed_string<N>& s, char fill = ' ') noexcept;

template <std::size_t Width, std::size_t N>
    requires(Width + 1 >= N)
consteval fixed_string<Width + 1> pad_left(const fixed_string<N>& s, char fill = ' ') noexcept;
```

---

## Section 10 — String Interning

```cpp
template <fixed_string S>
struct intern_tag {
    static constexpr auto value = S;
    static consteval std::string_view str() noexcept;
    static consteval std::uint64_t hash() noexcept;
    template <fixed_string Other>
    static consteval bool same_as(intern_tag<Other>) noexcept;
};

template <fixed_string A, fixed_string B>
inline constexpr bool intern_equal = (A == B);
```

---

## Section 11 — Compile-Time Path Operations

`namespace akshara::path` — all functions return `std::string_view` into the input (zero allocation) or operate at
`consteval` time.

```cpp
namespace path {
    // filename — basename including extension ("tokenizer.cpp" from "src/parser/tokenizer.cpp")
    template <std::size_t N>
    consteval std::string_view filename(const fixed_string<N>& p) noexcept;

    // stem — basename without extension ("tokenizer" from "src/parser/tokenizer.cpp")
    template <std::size_t N>
    consteval std::string_view stem(const fixed_string<N>& p) noexcept;

    // extension — file extension including dot (".cpp" from "src/parser/tokenizer.cpp")
    template <std::size_t N>
    consteval std::string_view extension(const fixed_string<N>& p) noexcept;

    // parent_path — directory portion ("src/parser" from "src/parser/tokenizer.cpp")
    template <std::size_t N>
    consteval std::string_view parent_path(const fixed_string<N>& p) noexcept;

    // normalize — strip leading "./" and trailing "/" 
    template <std::size_t N>
    consteval std::string_view normalize(const fixed_string<N>& p) noexcept;
}
```

**Usage:**

```cpp
static constexpr akshara::fixed_string p{"src/parser/tokenizer.cpp"};
static_assert(akshara::path::filename(p)    == "tokenizer.cpp");
static_assert(akshara::path::stem(p)        == "tokenizer");
static_assert(akshara::path::extension(p)   == ".cpp");
static_assert(akshara::path::parent_path(p) == "src/parser");
```

---

## Backward Compatibility with meta.hpp

`meta.hpp` includes `akshara.hpp` and re-exports all symbols under `namespace meta` via `using` declarations. Existing
code using `meta::fixed_string`, `meta::kmp_find`, `meta::ct_char_set`, etc. continues to work. New code should prefer
the `akshara::` prefix.

---

## Usage Examples

### STL/ranges integration

```cpp
static constexpr akshara::fixed_string s{"hello"};
// Implicit string_view conversion
static constexpr std::string_view sv = s;
// Range algorithm
static_assert(std::ranges::equal(s, std::string_view{"hello"}));
// Range-for
for (char c : s) { /* ... */ }
```

### NTTP string keys

```cpp
template <akshara::fixed_string Name>
struct config_entry {
    static constexpr auto hash = akshara::fnv1a64(Name);
};
```

### Compile-time path usage

```cpp
static constexpr akshara::fixed_string src{"src/parser/tokenizer.cpp"};
static constexpr auto ext = akshara::path::extension(src); // ".cpp"
```

### Character set operations

```cpp
static constexpr auto vowels  = akshara::ct_char_set{akshara::fixed_string{"aeiouAEIOU"}};
static constexpr auto not_vowel_ascii = vowels.complement();
static constexpr auto alpha_vowels = vowels & akshara::cs_alpha();
```

**Header**: `include/meta/akshara.hpp`  
**Namespace**: `akshara`  
**Standard**: C++23, header-only  
**Dependencies**: `<array>`, `<compare>`, `<cstddef>`, `<cstdint>`, `<string_view>` — no project dependencies

---

## Table of Contents

1. [Overview](#overview)
2. [Design Principles](#design-principles)
3. [Architecture](#architecture)
4. [Section 1 — fixed_string](#section-1--fixed_string)
5. [Section 2 — Character Classifiers (detail::fs)](#section-2--character-classifiers-detailfs)
6. [Section 3 — String Algorithms](#section-3--string-algorithms)
7. [Section 4 — ct_string_builder](#section-4--ct_string_builder)
8. [Section 5 — KMP Search](#section-5--kmp-search)
9. [Section 6 — join](#section-6--join)
10. [Section 7 — ct_char_set](#section-7--ct_char_set)
11. [Section 8 — FNV-1a Hash](#section-8--fnv-1a-hash)
12. [Section 9 — Padding](#section-9--padding)
13. [Section 10 — String Interning](#section-10--string-interning)
14. [Backward Compatibility with meta.hpp](#backward-compatibility-with-metahpp)
15. [Usage Examples](#usage-examples)

---

## Overview

Akshara ("अक्षर", Sanskrit for "character" or "letter") is the compile-time string foundation. It provides:

- An NTTP-capable string type (`fixed_string<N>`) for embedding string values as template parameters
- A full suite of compile-time string algorithms (search, split, replace, case conversion, padding)
- A compile-time character set type (`ct_char_set`) for predicate-driven parsing
- KMP string search with O (N+M) compile-time complexity
- FNV-1a 64-bit hashing for compile-time dispatch
- Type-level string interning for O (1) compile-time identity comparison

Everything is `consteval` or `constexpr`; no runtime overhead, no heap, no virtual, no macros.

---

## Design Principles

**Self-contained foundation.** Akshara has zero project-level dependencies. It depends only on the C++ standard library
headers listed above. `meta.hpp` depends on akshara, not the reverse.

**NTTP-first.** `fixed_string<N>` satisfies the structural type requirements for use as a non-type template parameter.
This enables patterns like `intern_tag<"hello">`, `lit<"keyword">`, and template specialization keyed on string values.

**All operations are consteval.** The library targets use in `static_assert`, template metaprogramming, and compile-time
code generation. Every function is `consteval` (immediate) unless it must be `constexpr` for use in both compile-time
and runtime contexts.

**No size erase.** Operations that produce a string return `fixed_string<ResultSize>` with the size encoded in the type.
Callers know the output size at compile time.

---

## Architecture

```
akshara.hpp
├── fixed_string<N>              — NTTP-capable string value type
├── namespace detail::fs         — char predicates and low-level helpers
├── String algorithms            — substr, find, replace, case, trim, ...
├── ct_string_builder<Capacity>  — mutable compile-time string accumulator
├── kmp_find / kmp_count         — KMP substring search
├── join                         — concatenate with separator
├── ct_char_set                  — 128-bit ASCII bitset
├── fnv1a64                      — FNV-1a 64-bit hash
├── pad_right / pad_left         — fixed-width padding
└── intern_tag / intern_equal    — type-level string identity
```

`meta.hpp` includes `akshara.hpp` and re-exports all symbols under `namespace meta` via `using` declarations, providing
backward compatibility for existing code.

---

## Section 1 — fixed_string

```cpp
template <std::size_t N>
struct fixed_string {
    char data[N]{};
    static constexpr std::size_t length = N - 1;  // excludes null terminator

    consteval fixed_string(const char (&str)[N]) noexcept;
    consteval fixed_string() noexcept = default;

    [[nodiscard]] consteval std::string_view view() const noexcept;
    [[nodiscard]] consteval bool empty() const noexcept;
    [[nodiscard]] consteval char operator[](std::size_t i) const noexcept;

    consteval bool operator==(const fixed_string&) const noexcept;

    // Concatenation: fixed_string<N> + fixed_string<M> → fixed_string<N+M-1>
    template <std::size_t M>
    consteval fixed_string<N + M - 1> operator+(const fixed_string<M>& o) const noexcept;
};

// CTAD: fixed_string{"hello"} deduces fixed_string<6>
template <std::size_t N>
fixed_string(const char (&)[N]) -> fixed_string<N>;

// Free operators
template <std::size_t N, std::size_t M>
consteval bool operator!=(const fixed_string<N>&, const fixed_string<M>&) noexcept;

template <std::size_t N, std::size_t M>
consteval auto operator<=>(const fixed_string<N>&, const fixed_string<M>&) noexcept;

// Append single char: fixed_string<N> + char → fixed_string<N+1>
template <std::size_t N>
consteval fixed_string<N + 1> operator+(const fixed_string<N>& s, char c) noexcept;
```

**Key notes:**

- `N` includes the null terminator; `length == N - 1` is the character count.
- The type is a structural type — suitable as an NTTP.
- Concatenation produces a new `fixed_string` with the correct size encoded in its type.

---

## Section 2 — Character Classifiers (detail::fs)

All functions in `namespace akshara::detail::fs`. Each is `constexpr` (callable at both compile and runtime).

| Function            | Returns true when                         |
|---------------------|-------------------------------------------|
| `is_upper(c)`       | `'A'`–`'Z'`                               |
| `is_lower(c)`       | `'a'`–`'z'`                               |
| `is_alpha(c)`       | `is_upper(c)                              || is_lower(c)` |
| `is_digit(c)`       | `'0'`–`'9'`                               |
| `is_alnum(c)`       | `is_alpha(c)                              || is_digit(c)` |
| `is_space(c)`       | space, tab, newline, CR, FF, VT           |
| `is_hex(c)`         | `is_digit(c)                              || 'a'`–`'f'` or `'A'`–`'F'` |
| `is_ident_start(c)` | `is_alpha(c)                              || c == '_'` |
| `is_ident_cont(c)`  | `is_alnum(c)                              || c == '_'` |
| `is_print(c)`       | `0x20`–`0x7E`                             |
| `is_punct(c)`       | `is_print(c) && !is_alnum(c) && c != ' '` |

Case converters:

```cpp
constexpr char to_upper(char c) noexcept;
constexpr char to_lower(char c) noexcept;
```

Internal helpers (not for direct use):

- `repeat_impl<C, Count>` — builds a `fixed_string` of `Count` copies of char `C`
- `trim_front_offset`, `trim_back_end` — compute trim offsets
- `digit_count<V>`, `uint_to_fixed<V, Digits>` — integer-to-string helpers

---

## Section 3 — String Algorithms

All functions in `namespace akshara`. All are `consteval`.

### Extraction

```cpp
// substr<Start, Len>(s) → fixed_string<Len+1>
template <std::size_t Start, std::size_t Len, std::size_t N>
consteval fixed_string<Len + 1> substr(const fixed_string<N>& s) noexcept;
```

### Search

```cpp
// First position of char, or string_view::npos
template <std::size_t N>
consteval std::size_t find_char(const fixed_string<N>& s, char c) noexcept;

// Last position of char, or string_view::npos
template <std::size_t N>
consteval std::size_t rfind_char(const fixed_string<N>& s, char c) noexcept;

// True if char is present
template <std::size_t N>
consteval bool contains_char(const fixed_string<N>& s, char c) noexcept;

// First position of needle, or string_view::npos (naive O(N*M))
template <std::size_t N, std::size_t M>
consteval std::size_t find_substr(const fixed_string<N>& hay,
                                   const fixed_string<M>& needle) noexcept;

template <std::size_t N, std::size_t M>
consteval bool starts_with(const fixed_string<N>& s,
                            const fixed_string<M>& prefix) noexcept;

template <std::size_t N, std::size_t M>
consteval bool ends_with(const fixed_string<N>& s,
                          const fixed_string<M>& suffix) noexcept;
```

### Transformation

```cpp
// Case conversion — returns fixed_string<N> of same size
template <std::size_t N>
consteval fixed_string<N> to_upper(const fixed_string<N>& s) noexcept;

template <std::size_t N>
consteval fixed_string<N> to_lower(const fixed_string<N>& s) noexcept;

// Replace all occurrences of from with to
template <std::size_t N>
consteval fixed_string<N> replace_char(const fixed_string<N>& s,
                                        char from, char to) noexcept;

// repeat<Count>(s) — concatenate s with itself Count times
template <std::size_t Count, std::size_t N>
consteval auto repeat(const fixed_string<N>& s) noexcept;

// Trim leading/trailing whitespace — returns string_view (no allocation)
template <std::size_t N>
consteval std::string_view trim_view(const fixed_string<N>& s) noexcept;
```

### Integer conversion

```cpp
// uint_to_str<V>() — compile-time integer to string, V must be >= 0
template <std::size_t V>
consteval auto uint_to_str() noexcept;  // → fixed_string<digit_count<V>+1>

// str_to_uint(s) — parse decimal digits, returns 0 on empty or invalid
template <std::size_t N>
consteval std::size_t str_to_uint(const fixed_string<N>& s) noexcept;
```

---

## Section 4 — ct_string_builder

A mutable compile-time string accumulator. Use when building a string from multiple pieces whose total size is known in
advance.

```cpp
template <std::size_t Capacity>
struct ct_string_builder {
    consteval ct_string_builder() noexcept;

    // Append a single char
    consteval void push(char c) noexcept;

    // Append a fixed_string
    template <std::size_t N>
    consteval void append(const fixed_string<N>& s) noexcept;

    // Append a string_view (only valid in consteval context with literal data)
    consteval void append(std::string_view sv) noexcept;

    // Materialize to fixed_string<N> (N must equal current size + 1)
    template <std::size_t N>
    consteval fixed_string<N> build() const noexcept;

    // Current character count (not including null terminator)
    consteval std::size_t size() const noexcept;
};
```

**Usage:**

```cpp
static constexpr auto make_label = []() consteval {
    akshara::ct_string_builder<32> b;
    b.append(akshara::fixed_string{"hello"});
    b.push('_');
    b.append(akshara::fixed_string{"world"});
    return b.build<12>();  // "hello_world" + NUL = 12 chars
}();
```

---

## Section 5 — KMP Search

Knuth–Morris–Pratt substring search. O (N+M) where N = haystack length, M = needle length. Builds the failure table
inline to avoid the constraint that pointers to function parameters cannot be constant expressions.

```cpp
// Returns position of first occurrence, or string_view::npos
template <std::size_t N, std::size_t M>
consteval std::size_t kmp_find(const fixed_string<N>& haystack,
                                const fixed_string<M>& needle) noexcept;

// Returns count of non-overlapping occurrences
template <std::size_t N, std::size_t M>
consteval std::size_t kmp_count(const fixed_string<N>& haystack,
                                 const fixed_string<M>& needle) noexcept;
```

**Usage:**

```cpp
static constexpr akshara::fixed_string hay{"hello world"};
static constexpr akshara::fixed_string needle{"world"};
static_assert(akshara::kmp_find(hay, needle) == 6);
static_assert(akshara::kmp_count(hay, needle) == 1);
```

Note: variables passed to `consteval` functions whose `.data` pointer is used as a constant expression must be
`static constexpr`.

---

## Section 6 — join

```cpp
// join(sep, a, b) — concatenate a + sep + b
template <std::size_t SN, std::size_t AN, std::size_t BN>
consteval auto join(const fixed_string<SN>& sep,
                    const fixed_string<AN>& a,
                    const fixed_string<BN>& b) noexcept;
// Returns fixed_string<AN + SN + BN - 2>
```

---

## Section 7 — ct_char_set

A compile-time bitset over ASCII (0–127). Structural type — usable as NTTP.

```cpp
struct ct_char_set {
    bool bits[128]{};

    consteval ct_char_set() noexcept = default;

    // Construct from a fixed_string of member chars
    template <std::size_t N>
    consteval explicit ct_char_set(const fixed_string<N>& chars) noexcept;

    [[nodiscard]] consteval bool contains(char c) const noexcept;
    [[nodiscard]] consteval ct_char_set operator|(const ct_char_set& o) const noexcept;
    [[nodiscard]] consteval ct_char_set operator&(const ct_char_set& o) const noexcept;
    [[nodiscard]] consteval ct_char_set complement() const noexcept;
    consteval void add_range(char lo, char hi) noexcept;
};
```

**Predefined factory functions** (all `consteval`):

| Function           | Characters included                |
|--------------------|------------------------------------|
| `cs_digits()`      | `'0'`–`'9'`                        |
| `cs_upper()`       | `'A'`–`'Z'`                        |
| `cs_lower()`       | `'a'`–`'z'`                        |
| `cs_alpha()`       | `cs_upper()                        | cs_lower()` |
| `cs_alnum()`       | `cs_alpha()                        | cs_digits()` |
| `cs_whitespace()`  | space, tab, `\n`, `\r`, `\f`, `\v` |
| `cs_hex()`         | digits + `a-f` + `A-F`             |
| `cs_ident_start()` | alpha + `_`                        |
| `cs_ident_cont()`  | alnum + `_`                        |

**Usage:**

```cpp
static constexpr auto vowels = akshara::ct_char_set{akshara::fixed_string{"aeiouAEIOU"}};
static_assert(vowels.contains('e'));
static_assert(!vowels.contains('b'));

// Complement: everything that is NOT a vowel
static constexpr auto consonants = vowels.complement();
```

---

## Section 8 — FNV-1a Hash

64-bit FNV-1a hash. Deterministic at compile time.

```cpp
// Hash a string_view
consteval std::uint64_t fnv1a64(std::string_view sv) noexcept;

// Hash a fixed_string
template <std::size_t N>
consteval std::uint64_t fnv1a64(const fixed_string<N>& s) noexcept;
```

**Usage:**

```cpp
static constexpr akshara::fixed_string key{"hello"};
static constexpr auto h = akshara::fnv1a64(key);
static_assert(h != 0);
```

Useful for compile-time switch-case dispatch on strings via `intern_tag::hash()`.

---

## Section 9 — Padding

Pad a `fixed_string` to a fixed column width.

```cpp
// Pad on the right (left-align text, fill with fill char)
// Requires Width + 1 >= N (result must fit)
template <std::size_t Width, std::size_t N>
    requires(Width + 1 >= N)
consteval fixed_string<Width + 1> pad_right(const fixed_string<N>& s,
                                              char fill = ' ') noexcept;

// Pad on the left (right-align text, fill with fill char)
template <std::size_t Width, std::size_t N>
    requires(Width + 1 >= N)
consteval fixed_string<Width + 1> pad_left(const fixed_string<N>& s,
                                             char fill = ' ') noexcept;
```

**Usage:**

```cpp
static constexpr akshara::fixed_string name{"hi"};
static constexpr auto padded = akshara::pad_right<8>(name, '-');
// padded.view() == "hi------"
```

---

## Section 10 — String Interning

Type-level string identity. Each distinct string value maps to a distinct C++ type, enabling O (1) identity comparison
by comparing type identities.

```cpp
// intern_tag<S>: unique type per string value S
template <fixed_string S>
struct intern_tag {
    static constexpr auto value = S;
    static consteval std::string_view str() noexcept;
    static consteval std::uint64_t hash() noexcept;

    template <fixed_string Other>
    static consteval bool same_as(intern_tag<Other>) noexcept;
};

// intern_equal<A, B> — true iff string A == string B (type-level)
template <fixed_string A, fixed_string B>
inline constexpr bool intern_equal = (A == B);
```

**Usage:**

```cpp
using Tag1 = akshara::intern_tag<"hello">;
using Tag2 = akshara::intern_tag<"hello">;
using Tag3 = akshara::intern_tag<"world">;

// Same string → same type
static_assert(std::is_same_v<Tag1, Tag2>);

// Different string → different type
static_assert(!std::is_same_v<Tag1, Tag3>);

// Equality via variable template
static_assert(akshara::intern_equal<"hello", "hello">);
static_assert(!akshara::intern_equal<"hello", "world">);
```

---

## Backward Compatibility with meta.hpp

`meta.hpp` includes `akshara.hpp` and re-exports all symbols under `namespace meta`:

```cpp
// In namespace meta:
template <std::size_t N>
using fixed_string = akshara::fixed_string<N>;

using akshara::kmp_find;
using akshara::ct_char_set;
using akshara::fnv1a64;
// ... and all other akshara symbols
```

Existing code using `meta::fixed_string`, `meta::kmp_find`, `meta::ct_char_set`, etc. continues to work without
modification. New code should prefer the `akshara::` prefix directly.

`meta::detail::fs` also re-exports `akshara::detail::fs` via `using` declarations.

The one function that stays exclusively in `meta` (not in akshara) is `split_by_char`, because it bridges
`akshara::fixed_string` with `meta::ct_array` — a type that lives in meta.

---

## Usage Examples

### NTTP string keys

```cpp
template <akshara::fixed_string Name>
struct config_entry {
    static constexpr auto key = Name;
    static constexpr auto hash = akshara::fnv1a64(Name);
};

using timeout_entry = config_entry<"timeout">;
static_assert(timeout_entry::key == akshara::fixed_string{"timeout"});
```

### Compile-time keyword check

```cpp
static constexpr auto keywords = akshara::ct_char_set{};  // build set as needed

static constexpr akshara::fixed_string kw{"return"};
static_assert(akshara::starts_with(kw, akshara::fixed_string{"ret"}));
```

### Building formatted strings

```cpp
template <std::size_t V>
consteval auto make_field_name() {
    constexpr auto prefix = akshara::fixed_string{"field_"};
    constexpr auto num    = akshara::uint_to_str<V>();
    return prefix + num;
}
static_assert(make_field_name<3>() == akshara::fixed_string{"field_3"});
```

### Padding for table output

```cpp
consteval auto format_row(akshara::fixed_string<9> name,
                           akshara::fixed_string<7> value) {
    return akshara::pad_right<16>(name) + akshara::pad_left<10>(value);
}
```

### Intern-based dispatch

```cpp
template <akshara::fixed_string Tag>
void dispatch() {
    if constexpr (akshara::intern_equal<Tag, "read">)  { /* ... */ }
    if constexpr (akshara::intern_equal<Tag, "write">) { /* ... */ }
}
```
