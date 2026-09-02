# Tutorial: Zero to Hero with Vākya — Structural Construction & Expression AST EDSL

Welcome to the **Vākya Tutorial**. Vākya (वाक्य, "sentence" / "structured expression" in Sanskrit) is Pebble's
header-only, C++26-ready Embedded Domain-Specific Language (EDSL) for constructing, traversing, pattern matching,
type-checking, and rewriting immutable expression ASTs and DAGs without macros or virtual functions.

Whether you are writing a programming language frontend, an optimizing compiler, an algebra engine, or a theorem proving
bridge, Vākya provides a high-performance, type-safe structural foundation.

---

## 📑 Table of Contents

1. [The Philosophy: Why Vākya?](#1-the-philosophy-why-vākya)
2. [Step 1: Building Your First Expression Tree](#step-1-building-your-first-expression-tree)
3. [Step 2: Structural Hashing & Equality ($O (1)$ Hash-Consing)](#step-2-structural-hashing--equality-o1-hash-consing)
4. [Step 3: Tree Traversals & Folds (`vakya::tree`)](#step-3-tree-traversals--folds-vakyatree)
5. [Step 4: Pattern Matching & Destructuring (`vakya::pattern`)](#step-4-pattern-matching--destructuring-vakyapattern)
6. [Step 5: Rule-Based Term Rewriting (`vakya::rule_registry`)](#step-5-rule-based-term-rewriting-vakyarule_registry)
7. [Step 6: Type Systems, Unification & Constraints (
   `vakya::types`)](#step-6-type-systems-unification--constraints-vakyatypes)
8. [Step 7: SMT Verification Bridge (Tarka / Z3 Integration)](#step-7-smt-verification-bridge-tarka--z3-integration)
9. [Vākya Cheat Sheet](#9-vākya-cheat-sheet)

---

## 1. The Philosophy: Why Vākya?

Traditional AST implementations rely on pointers, heap allocations for every binary node (`new BinaryOp(...)`), and
runtime virtual function dispatch (`node->accept(visitor)`).

**Vākya takes a modern data-oriented approach**:

1. **Value Semantics & Zero Allocation**: Small nodes live on the stack or in flat arrays.
2. **Tag-Driven NTTP Metaprogramming**: Node operators (e.g. `Add`, `Mul`, `Var`, `Const`) are non-type template
   parameter tags.
3. **Pure Structural Core**: Core construction has no dependencies and zero semantic baggage. Passes and type systems
   are modular, layered add-ons.

---

## Step 1: Building Your First Expression Tree

Expressions in Vākya are built using literal constructors and tag combinations:

```cpp
#include <vakya/vakya.hpp>
#include <iostream>

using namespace vakya;

// Define custom node tags
struct AddTag {};
struct MulTag {};
struct VarTag {};
struct ConstTag {};

void construction_demo() {
    // 1. Terminals (Leaves)
    auto x = terminal<VarTag>(std::string("x"));
    auto two = terminal<ConstTag>(2);
    auto three = terminal<ConstTag>(3);

    // 2. Binary nodes: (x * 2) + 3
    auto expr = binary<AddTag>(
        binary<MulTag>(x, two),
        three
    );

    std::cout << "Tree constructed! Arity: " << expr.arity << "\n";
}
```

---

## Step 2: Structural Hashing & Equality ($O (1)$ Hash-Consing)

Every expression computed in Vākya automatically maintains its deterministic structural hash. Two identical ASTs produce
the same hash code regardless of memory location:

```cpp
#include <vakya/vakya.hpp>
#include <cassert>

void hashing_demo() {
    auto e1 = binary<AddTag>(terminal<VarTag>("x"), terminal<ConstTag>(42));
    auto e2 = binary<AddTag>(terminal<VarTag>("x"), terminal<ConstTag>(42));

    // Structural hash equality
    assert(structural_hash(e1) == structural_hash(e2));

    // Deep structural equality
    assert(e1 == e2);
}
```

---

## Step 3: Tree Traversals & Folds (`vakya::tree`)

You can evaluate or transform ASTs using functional folds without writing visitor classes:

```cpp
#include <vakya/vakya.hpp>
#include <iostream>

void evaluate_demo() {
    auto ast = binary<AddTag>(
        binary<MulTag>(terminal<ConstTag>(5), terminal<ConstTag>(4)),
        terminal<ConstTag>(10)
    ); // (5 * 4) + 10 = 30

    // Evaluate integer AST bottom-up:
    int result = tree::fold(ast, [](auto tag, auto children_results) -> int {
        using Tag = decltype(tag);
        if constexpr (std::is_same_v<Tag, ConstTag>) {
            return children_results.value;
        } else if constexpr (std::is_same_v<Tag, AddTag>) {
            return children_results[0] + children_results[1];
        } else if constexpr (std::is_same_v<Tag, MulTag>) {
            return children_results[0] * children_results[1];
        }
        return 0;
    });

    std::cout << "Evaluated Result: " << result << "\n"; // 30
}
```

---

## Step 4: Pattern Matching & Destructuring (`vakya::pattern`)

Vākya provides a declarative pattern matching DSL to inspect tree structures:

```cpp
#include <vakya/pattern.hpp>
#include <iostream>

using namespace vakya::pattern;

void pattern_match_demo(const auto &expr) {
    // Match pattern: x + 0
    auto x = wildcard();
    auto zero = literal(0);

    auto pattern = match_binary<AddTag>(x, zero);

    if (auto match = pattern.match(expr)) {
        std::cout << "Matched (x + 0)! Simplified to: " << match.capture(x) << "\n";
    }
}
```

---

## Step 5: Rule-Based Term Rewriting (`vakya::rule_registry`)

Build algebraic simplifiers and compiler optimization passes by registering rewrite rules:

```cpp
#include <vakya/rule_registry.hpp>

void register_algebraic_rules(RuleRegistry &registry) {
    auto x = wildcard();
    auto zero = literal(0);
    auto one = literal(1);

    // Rule: x + 0 => x
    registry.add_rule(
        "add_zero_identity",
        match_binary<AddTag>(x, zero),
        [](const auto &match) { return match.capture(x); }
    );

    // Rule: x * 1 => x
    registry.add_rule(
        "mul_one_identity",
        match_binary<MulTag>(x, one),
        [](const auto &match) { return match.capture(x); }
    );

    // Rule: x * 0 => 0
    registry.add_rule(
        "mul_zero_annihilation",
        match_binary<MulTag>(x, zero),
        [](const auto &match) { return literal(0); }
    );
}
```

---

## Step 6: Type Systems, Unification & Constraints (`vakya::types`)

For language compilers, Vākya includes an optional Hindley-Milner type inference engine and Robinson first-order
unification solver:

```cpp
#include <vakya/types.hpp>
#include <vakya/unification.hpp>
#include <iostream>

void type_inference_demo() {
    types::TypeContext tctx;

    // Type variables: ?T0, ?T1
    auto alpha = tctx.fresh_type_var();
    auto int_type = tctx.primitive("i32");

    // Unify ?T0 = i32
    types::UnificationEngine unifier;
    bool success = unifier.unify(alpha, int_type);

    if (success) {
        std::cout << "Inferred type for ?T0: " << unifier.resolve(alpha) << "\n"; // i32
    }
}
```

---

## Step 7: SMT Verification Bridge (Tarka / Z3 Integration)

Verify program correctness and equivalence of compiler rewrites using Tarka SMT:

```cpp
#include <vakya/smt.hpp>

void verify_rewrite_correctness() {
    // Check if (x << 1) == (x * 2) under 32-bit integer arithmetic
    smt::SMTBridge bridge;
    
    auto lhs = binary<ShiftLeftTag>(terminal<VarTag>("x"), terminal<ConstTag>(1));
    auto rhs = binary<MulTag>(terminal<VarTag>("x"), terminal<ConstTag>(2));

    // Validates that NOT(lhs == rhs) is UNSAT (meaning theorem holds for all 32-bit x)
    bool is_valid = bridge.verify_equivalence(lhs, rhs);
    assert(is_valid);
}
```

---

## 9. Vākya Cheat Sheet

| Task                    | Code Snippet                                               |
|:------------------------|:-----------------------------------------------------------|
| **Terminal Node**       | `terminal<Tag>(value)`                                     |
| **Unary Node**          | `unary<NegTag>(child)`                                     |
| **Binary Node**         | `binary<AddTag>(lhs, rhs)`                                 |
| **Structural Equality** | `node1 == node2` or `structural_hash(node)`                |
| **Tree Fold**           | `tree::fold(node, callback)`                               |
| **Pattern Match**       | `match_binary<AddTag>(wildcard(), literal(0)).match(node)` |
| **Rewrite Registry**    | `registry.add_rule(name, pattern, rewrite_fn)`             |
| **Type Unification**    | `unifier.unify(type_var, concrete_type)`                   |
