# Tutorial: The Language Crafter's Grimoire — Parsing Theory & Mastery with Samāsa

Welcome, traveler, to the **Language Crafter's Workshop**. In the realm of compiler engineering, text is raw chaos. A
user types characters into an editor:

```rust
let answer = 40 + 2; // the meaning of life
```

To a computer, this is merely a sequence of 38 arbitrary ASCII bytes. How does a compiler transform this stream of
characters into an executable computation tree without getting lost in ambiguities, wasting memory, or crashing when the
user types a syntax error?

Enter **Samāsa** (समास — *"synthesis / compounding"* in Sanskrit), Pebble's header-only, C++26 compile-time parsing
substrate.

In this tutorial, we will not merely show you code. We will embark on a fun, intuitive journey through **formal parsing
theory**:

- What is the battle between **Context-Free Grammars (CFG)** and **Parsing Expression Grammars (PEG)**?
- Why does **Backtracking** cause exponential nightmares (and how **Packrat Parsing** saves us)?
- How does **Vaughan Pratt's 1973 trick** solve mathematical operator precedence with elegant simplicity?
- How do modern IDEs (like Roslyn and Swift) preserve every space and comment using **Lossless Green/Red Concrete Syntax
  Trees**?

Grab your favorite beverage, and let's craft a language from scratch!

---

## 📑 Table of Contents

1. [Act 0: The Grand Theory of Parsing (CFG vs. PEG vs. LL/LR)](#act-0-the-grand-theory-of-parsing-cfg-vs-peg-vs-lllr)
    - [The Chomsky Hierarchy & Context-Free Grammars](#the-chomsky-hierarchy--context-free-grammars)
    - [The Curse of Ambiguity: The "Dangling Else" Riddle](#the-curse-of-ambiguity-the-dangling-else-riddle)
    - [The PEG Revolution: Prioritized Choice ($/$)](#the-peg-revolution-prioritized-choice-)
    - [The Exponential Dragon & Packrat Memoization](#the-exponential-dragon--packrat-memoization)
    - [Syntactic Predicates: Lookahead Without Consuming ($\&, !$)](#syntactic-predicates-lookahead-without-consuming--)
2. [Act 1: The Scanner (Zero-Allocation Token Stream)](#act-1-the-scanner-zero-allocation-token-stream)
3. [Act 2: PEG Combinators (Building Grammars in Pure C++)](#act-2-peg-combinators-building-grammars-in-pure-c)
4. [Act 3: The Pratt Expression Wizard (Operator Precedence Solved)](#act-3-the-pratt-expression-wizard-operator-precedence-solved)
5. [Act 4: The Green/Red Tree Architecture (Lossless CST for IDEs)](#act-4-the-greenred-tree-architecture-lossless-cst-for-ides)
6. [Act 5: The Resilient Parser (Error Recovery & Synchronization)](#act-5-the-resilient-parser-error-recovery--synchronization)
7. [Act 6: The Grand Finale — Building a Complete Scripting Language](#act-6-the-grand-finale--building-a-complete-scripting-language)
8. [Samāsa Master Cheat Sheet](#8-samāsa-master-cheat-sheet)

---

## Act 0: The Grand Theory of Parsing (CFG vs. PEG vs. LL/LR)

Before writing code, let's understand the mathematics that govern all human and computer languages.

```
                  ┌─────────────────────────────────────────┐
                  │          Unrestricted (Turing)          │
                  │   ┌─────────────────────────────────┐   │
                  │   │    Context-Sensitive (CSG)      │   │
                  │   │   ┌─────────────────────────┐   │   │
                  │   │   │    Context-Free (CFG)   │   │   │
                  │   │   │   ┌─────────────────┐   │   │   │
                  │   │   │   │  Regular (Regex)│   │   │   │
                  │   │   │   └─────────────────┘   │   │   │
                  │   │   └─────────────────────────┘   │   │
                  │   └─────────────────────────────────┘   │
                  └─────────────────────────────────────────┘
```

### The Chomsky Hierarchy & Context-Free Grammars

In 1956, linguist Noam Chomsky classified grammars into four levels:

1. **Regular Languages (Type 3)**: Regex. Cannot count or match arbitrary nested parentheses `((...))`.
2. **Context-Free Languages (Type 2, CFG)**: Can match nested brackets. Defined by non-terminals expanding into
   alternatives:
   $$A \to B \mid C$$
   Here, the vertical bar $\mid$ is **non-deterministic (unordered) choice**. A string might match $B$, or $C$, or
   *both*.

### The Curse of Ambiguity: The "Dangling Else" Riddle

Because CFG choice is unordered, grammars can be **ambiguous** — a single sentence can generate multiple valid parse
trees!

Consider the classic compiler trap:

```c
if (condition1) if (condition2) do_something(); else do_other_thing();
```

To which `if` does the `else` belong?

- *Tree A*: Outer `if` owns the `else`.
- *Tree B*: Inner `if` owns the `else`.

In traditional tools (Yacc/Bison), you had to write shift/reduce precedence hacks to stop the parser from guessing
wrong.

---

### The PEG Revolution: Prioritized Choice ($/$)

In 2004, Bryan Ford introduced **Parsing Expression Grammars (PEG)**.

The core insight of PEG: Replace unordered choice ($\mid$) with **Prioritized (Ordered) Choice ($/$)**.

$$e_1 / e_2$$

- Try matching expression $e_1$.
- If $e_1$ succeeds, **commit immediately**. Expression $e_2$ is *never evaluated*.
- If $e_1$ fails, rewind the input cursor to where $e_1$ started and try $e_2$.

> [!TIP]
> **Why PEG is Loved by Systems Engineers**:
> 1. **Zero Ambiguity**: A PEG grammar has exactly *one* valid parse tree for any input.
> 2. **Scannerless Possible**: Lexing and parsing can be unified into a single declarative structure.
> 3. **Integrated Lookahead**: Unlimited lookahead without consuming input.

---

### The Exponential Dragon & Packrat Memoization

Consider this innocent PEG rule:
$$S \to (A \text{ 'b'}) / (A \text{ 'c'})$$
If $A$ is a complex 500-line expression and matches everything *except* the final `'b'`, the naive parser rewinds and
re-parses all 500 lines of $A$ just to check `'c'`.

In pathological cases, naive recursive descent with backtracking is $O (2^N)$ (exponential time)!

**Packrat Parsing** solves this by memoizing the result of parsing rule $R$ at input position $P$:
$$\text{MemoTable}[\text{Rule}, \text{Position}] \to (\text{Success/Failure}, \text{NewPosition}, \text{Node})$$
With memoization, parsing time is guaranteed to be linear $O (N)$!

**In Samāsa**: Because memory allocation is expensive, Samāsa provides *Configurable Memoization Policies* (`NoMemo`,
`WindowedMemo`, `FullPackrat`), allowing you to use $O (1)$ stack memory for deterministic paths and memoize only where
lookahead branches exist.

---

### Syntactic Predicates: Lookahead Without Consuming ($\&, !$)

PEGs introduce two superpowers:

1. **And-Predicate ($\&e$)**: Succeeded only if $e$ matches, but **does not consume any input**. (Think: *"Peek ahead to
   make sure a keyword isn't followed by an identifier character"*).
2. **Not-Predicate ($!e$)**: Succeeded only if $e$ **fails** to match, and does not consume input. (Think: *"Match
   anything except comments or keywords"*).

---

## Act 1: The Scanner (Zero-Allocation Token Stream)

Let's begin writing code with Samāsa!

The Scanner consumes a `std::string_view` and emits lightweight `Token` structs containing byte offsets (`Span`) rather
than copying strings into heap buffers.

```cpp
#include <languages/samasa/samasa.hpp>
#include <iostream>

using namespace lang::samasa;

void act1_scanner() {
    std::string_view source = "let answer: i32 = 42; // compute universe";

    // 1. Create a zero-copy scanner
    Scanner scanner(source);

    // 2. Iterate tokens
    while (!scanner.is_at_end()) {
        Token tok = scanner.next_token();
        
        std::cout << "Token: [" << tok.lexeme << "] "
                  << "Kind: " << static_cast<int>(tok.kind) << " "
                  << "Range: [" << tok.span.start << ".." << tok.span.end << "]\n";
    }
}
```

---

## Act 2: PEG Combinators (Building Grammars in Pure C++)

In Samāsa, you define PEG rules using C++ expressions. No external code generator, no `.y` or `.g4` files!

```cpp
#include <languages/samasa/samasa.hpp>

using namespace lang::samasa::dsl;

// 1. Match a single keyword or punctuation
constexpr auto kw_let   = lit("let");
constexpr auto sym_eq   = lit("=");
constexpr auto sym_semi = lit(";");

// 2. Character classes & ranges
constexpr auto alpha = choice(range('a', 'z'), range('A', 'Z'), lit('_'));
constexpr auto digit = range('0', '9');

// 3. Identifier: alpha (alpha | digit)*
constexpr auto identifier = seq(alpha, zero_or_more(choice(alpha, digit)));

// 4. Numeric Literal: digit+
constexpr auto integer = one_or_more(digit);

// 5. Lookahead: Ensure a keyword is not part of a longer variable name
// e.g. "let_variable" should match as identifier, not "let" keyword!
constexpr auto strict_let = seq(kw_let, not_pred(choice(alpha, digit)));

// 6. Complete variable declaration rule:
// let <id> = <integer> ;
constexpr auto let_statement = seq(
    strict_let, ws(),
    identifier, ws(),
    sym_eq,     ws(),
    integer,    ws(),
    sym_semi
);
```

---

## Act 3: The Pratt Expression Wizard (Operator Precedence Solved)

### The Problem with Parsing Math Expressions

In pure PEG, parsing $1 + 2 \times 3 - 4 / 5$ requires creating recursive nested grammar rules (`Expression` $\to$
`Term` $\to$ `Factor` $\to$ `Primary`). For 15 levels of C++ operator precedence, this creates a 15-deep call stack for
every single number!

### The Solution: Top-Down Operator Precedence (Pratt Parsing)

In 1973, Vaughan Pratt discovered an algorithm based on two numbers:

- **Binding Power (Precedence)**: How tightly an operator glues to its arguments. $\times$ has higher binding power
  (e.g. 20) than $+$ (e.g. 10).
- **Associativity**: Left-associative (`1 - 2 - 3` $\equiv$ `(1 - 2) - 3`) vs. Right-associative (`a = b = c` $\equiv$
  `a = (b = c)`).

Samāsa provides a compile-time Pratt table builder:

```cpp
#include <languages/samasa/samasa.hpp>

using namespace lang::samasa::pratt;

// Define token kinds
enum class MyTokens {
    Integer, Plus, Minus, Star, Slash, Caret, LParen, RParen
};

// Define Pratt Precedence Table at Compile-Time
struct MathPrecedence {
    static constexpr auto table() {
        return pratt_builder<MyTokens>()
            // Atoms & Sub-expressions
            .atom(MyTokens::Integer)
            .group(MyTokens::LParen, MyTokens::RParen)
            
            // Binary Operators: infix_left(Token, Precedence)
            .infix_left(MyTokens::Plus,  10)
            .infix_left(MyTokens::Minus, 10)
            .infix_left(MyTokens::Star,  20)
            .infix_left(MyTokens::Slash, 20)
            
            // Exponentiation is Right-Associative (2^3^4 = 2^(3^4))
            .infix_right(MyTokens::Caret, 30)
            
            // Prefix Unary (-x)
            .prefix(MyTokens::Minus, 40);
    }
};
```

---

## Act 4: The Green/Red Tree Architecture (Lossless CST for IDEs)

Modern tools (Rust Analyzer, Roslyn C#, Swift compiler) do not throw away comments or whitespace. If a user asks the IDE
to rename a variable, the IDE must rewrite the file while preserving every single space and comment.

Samāsa implements the **Green/Red Tree Design Pattern**:

```
                    ┌────────────────────────┐
                    │        Red Tree        │  (Has Parent pointers, absolute offsets,
                    │   (Contextual Views)   │   and spans for IDE refactoring)
                    └───────────┬────────────┘
                                │ wraps on-demand
                    ┌───────────▼────────────┐
                    │       Green Tree       │  (Immutable, pure structural nodes.
                    │   (Lossless Syntax)    │   Zero absolute offsets. 100% Cacheable!)
                    └────────────────────────┘
```

1. **Green Nodes**: Immutable, position-independent. A green node `10 + 20` has the exact same hash whether it appears
   at line 1 or line 10,000!
2. **Red Nodes**: Lightweight transient views created on the fly when traversing the tree to inspect source spans and
   diagnostics.

```cpp
#include <languages/samasa/samasa.hpp>
#include <iostream>

void act4_cst_inspection() {
    std::string_view code = "x + 42";
    auto parse_result = parse_expression(code);

    if (parse_result.is_ok()) {
        const GreenNode &green = parse_result.green_root();
        std::cout << "Green Node Kind: " << green.kind_name() << "\n";
        std::cout << "Exact Width in Bytes: " << green.text_len() << "\n";
        
        // Inspect children
        for (const auto &child : green.children()) {
            std::cout << "  Child: " << child.kind_name() << "\n";
        }
    }
}
```

---

## Act 5: The Resilient Parser (Error Recovery & Synchronization)

If your compiler crashes on the first missing semicolon, your language server (LSP) will show red squiggles everywhere
and stop providing autocomplete.

Samāsa incorporates **Synchronization-Based Error Recovery**:

```cpp
#include <languages/samasa/samasa.hpp>

using namespace lang::samasa::dsl;

// When a syntax error occurs inside a statement:
// Skip malformed tokens until we hit a statement boundary (';' or '}')
constexpr auto safe_statement = synchronize_on(
    statement_body,
    choice(lit(";"), lit("}"))
);
```

When an error happens:

1. An error diagnostic node is attached to the Green CST (preserving the broken text!).
2. The cursor advances to the synchronization token.
3. Parsing resumes smoothly for the next function or statement.

---

## Act 6: The Grand Finale — Building a Complete Scripting Language

Let's put everything together to build a complete arithmetic & variable calculator DSL!

```cpp
#include <languages/samasa/samasa.hpp>
#include <iostream>
#include <unordered_map>

using namespace lang::samasa;

void grand_finale_demo() {
    std::string_view program = 
        "let radius = 10;\n"
        "let height = 5;\n"
        "let volume = 3 * radius * radius * height;\n";

    std::cout << "--- Parsing Script with Samāsa ---\n";
    std::cout << program << "\n";

    // 1. Parse into CST
    auto cst = parse_program(program);

    if (!cst.has_errors()) {
        std::cout << "✅ Syntax Validated! Green Tree Node Count: " 
                  << cst.node_count() << "\n";

        // 2. Evaluate AST
        std::unordered_map<std::string, int64_t> variables;
        evaluate_statements(cst.root(), variables);

        std::cout << "--- Execution Output ---\n";
        std::cout << "volume = " << variables["volume"] << "\n"; // 3 * 10 * 10 * 5 = 1500
    } else {
        std::cerr << "❌ Parsing Failed: " << cst.first_error() << "\n";
    }
}
```

---

## 8. Samāsa Master Cheat Sheet

### Core PEG Combinators

| Combinator             | Syntax / Function             | Description                                      |
|:-----------------------|:------------------------------|:-------------------------------------------------|
| **Literal**            | `lit("foo")`, `lit('x')`      | Matches exact text or character                  |
| **Range**              | `range('0', '9')`             | Matches single char within $[a..b]$ range        |
| **Sequence**           | `seq(A, B, C)`                | Matches $A$, then $B$, then $C$                  |
| **Prioritized Choice** | `choice(A, B)`                | Tries $A$; if failed rewinds and tries $B$       |
| **Optional**           | `opt(A)`                      | Matches $A$ 0 or 1 time                          |
| **Kleene Star**        | `zero_or_more(A)` / `star(A)` | Matches $A$ zero or more times ($A^*$)           |
| **Kleene Plus**        | `one_or_more(A)` / `plus(A)`  | Matches $A$ one or more times ($A^+$)            |
| **And-Predicate**      | `and_pred(A)`                 | Succeeds if $A$ matches; **consumes zero input** |
| **Not-Predicate**      | `not_pred(A)`                 | Succeeds if $A$ fails; **consumes zero input**   |
| **Whitespace**         | `ws()`                        | Consumes spaces, tabs, newlines                  |

### Pratt Expression Parser

| Method                      | Description                                     |
|:----------------------------|:------------------------------------------------|
| `.atom(Token)`              | Primary terminal (number, identifier, string)   |
| `.group(L, R)`              | Parentheses grouping `(expr)`                   |
| `.prefix(Token, Prec)`      | Unary prefix operator (`-x`, `!x`, `~x`)        |
| `.postfix(Token, Prec)`     | Postfix operator (`x++`, `x?`)                  |
| `.infix_left(Token, Prec)`  | Left-associative binary op (`+`, `-`, `*`, `/`) |
| `.infix_right(Token, Prec)` | Right-associative binary op (`=`, `^`)          |
