# Tutorial: The Grand Master of Pebble Island — Automated Theorem Proving with Tarka

Welcome to **Pebble Island**. As the Chief Systems Architect and Detective of the realm, you are faced with a wide variety of computational puzzles: scheduling guards, decrypting cryptographic vaults, balancing national budgets without floating-point errors, validating hardware circuits, proving memory drivers free of data-leaks, verifying compiler optimizations, and coordinating parallel solver fleets.

Instead of writing ad-hoc heuristics or slow brute-force loops, you will use **Tarka** — Pebble's modern, zero-overhead, non-Z3 native Automated Theorem Prover and SMT (Satisfiability Modulo Theories) substrate.

This tutorial assumes **zero prior knowledge of Z3 or formal methods**. Every concept is introduced step-by-step with intuitive storytelling, rigorous mathematical intuition, and copy-pasteable, compiling modern C++23 code snippets.

---

## Table of Contents
1. [The Foundational Concepts: What is SMT?](#1-the-foundational-concepts-what-is-smt)
2. [The One-File Compilation Blueprint](#2-the-one-file-compilation-blueprint)
3. [Act 1: The Island Guards (Propositional Logic & Boolean CDCL)](#act-1-the-island-guards-propositional-logic--boolean-cdcl)
4. [Act 2: The Cryptographic Vault (BitVectors & Binary Arithmetic — QF_BV)](#act-2-the-cryptographic-vault-bitvectors--binary-arithmetic--qf_bv)
5. [Act 3: The King's Ledger (Linear Real & Integer Arithmetic — QF_LRA / QF_LIA)](#act-3-the-kings-ledger-linear-real--integer-arithmetic--qf_lra--qf_lia)
6. [Act 4: The Black-Box Guilds (Equality with Uninterpreted Functions — QF_UF)](#act-4-the-black-box-guilds-equality-with-uninterpreted-functions--qf_uf)
7. [Act 5: The Secure Memory Driver (Array Theory & Extensionality — QF_AX)](#act-5-the-secure-memory-driver-array-theory--extensionality--qf_ax)
8. [Act 6: Multi-Theory Synthesis (QF_AUFBV Combination)](#act-6-multi-theory-synthesis-qf_aufbv-combination)
9. [Act 7: Universal Laws (Quantifier Reasoning & E-Matching)](#act-7-universal-laws-quantifier-reasoning--e-matching)
10. [Act 8: The Detective's Inquiry (Assumptions & Minimal Unsat Cores)](#act-8-the-detectives-inquiry-assumptions--minimal-unsat-cores)
11. [Act 9: The Preprocessing Crucible (Algebraic Simplification & E-Graphs)](#act-9-the-preprocessing-crucible-algebraic-simplification--e-graphs)
12. [Act 10: Talking SMT-LIB2 (Parsing & Serializing Scripts)](#act-10-talking-smt-lib2-parsing--serializing-scripts)
13. [Act 11: Trust But Mathematically Verify (Model Validator)](#act-11-trust-but-mathematically-verify-model-validator)
14. [Act 12: Parallel Races (Competitive Multi-Engine Portfolio)](#act-12-parallel-races-competitive-multi-engine-portfolio)
15. [Quick API Reference & Cheat Sheet](#15-quick-api-reference--cheat-sheet)

---

## 1. The Foundational Concepts: What is SMT?

In ordinary programming, you write algorithms that compute an output from an input:
$$\text{Input} \longrightarrow f(x) \longrightarrow \text{Output}$$

In **Constraint Solving and Automated Theorem Proving**, you describe the **rules and invariants** that your solution must satisfy, and the computer computes the inputs that make all rules true:
$$\text{Rules \& Invariants} \longrightarrow \text{Tarka Solver} \longrightarrow \text{Valid Assignment (SAT) or Proof of Impossibility (UNSAT)}$$

### SAT vs. SMT
- **SAT (Propositional Satisfiability)**: Solves formulas containing only raw `true`/`false` variables combined with $\land$ (AND), $\lor$ (OR), and $\neg$ (NOT).
- **SMT (Satisfiability Modulo Theories)**: Extends SAT with rich domain-specific reasoning engines:
  - **BitVectors (`QF_BV`)**: Machine words of fixed bit-width (e.g., 8-bit, 32-bit, 64-bit). Handles 2's complement math, overflow, bitwise logic, shifts, and division by zero.
  - **Linear Arithmetic (`QF_LRA`/`QF_LIA`)**: Exact equations and inequalities ($3x + 4.5y \le 100$) evaluated over exact fractions without floating-point error.
  - **Arrays (`QF_AX`)**: Unbounded key-value maps supporting `select` (read) and `store` (write) operations.
  - **Uninterpreted Functions (`QF_UF`)**: Abstract functions satisfying the congruence axiom ($x = y \implies f(x) = f(y)$).

---

## 2. The One-File Compilation Blueprint

Tarka is **header-only** and designed for **modern C++23**. It does not require virtual functions or macros.

Save the following template as `main.cpp` and compile with any C++23 compiler:

```cpp
#include "tarka/tarka.hpp"
#include "tarka/backends/native_backend.hpp"
#include "tarka/frontend/smt2_lexy.hpp"
#include "tarka/frontend/lower_to_tarka.hpp"
#include "tarka/frontend/smt2_printer.hpp"
#include "tarka/native/model_validator.hpp"
#include "tarka/native/simplifier.hpp"
#include "tarka/portfolio.hpp"

#include <iostream>
#include <vector>

using namespace tarka;
using namespace tarka::backend;
using namespace tarka::frontend;
using namespace tarka::native;

int main() {
    std::cout << "Tarka native reasoning substrate ready!\n";
    return 0;
}
```

---

## Act 1: The Island Guards (Propositional Logic & Boolean CDCL)

### The Mystery
The island has three guards: **Alice**, **Bob**, and **Charlie**.
1. At least one guard must be on duty: $(\text{Alice} \lor \text{Bob} \lor \text{Charlie})$.
2. If Alice is on duty, Bob refuses to work: $(\text{Alice} \implies \neg\text{Bob}) \equiv (\neg\text{Alice} \lor \neg\text{Bob})$.
3. If Charlie is on duty, Alice must also be present: $(\text{Charlie} \implies \text{Alice}) \equiv (\neg\text{Charlie} \lor \text{Alice})$.
4. Bob is taking a vacation today: $\neg\text{Bob}$.

Let's find the valid roster!

### The Code
```cpp
void act1_solve_guards() {
    Context ctx; // Manages the bump arena and term hash-consing
    RouterEngine<backend::native> solver; // Native zero-dependency SMT engine

    auto bool_s = ctx.bool_sort();

    // 1. Declare Boolean variables
    auto alice   = ctx.make_symbol("Alice", bool_s);
    auto bob     = ctx.make_symbol("Bob", bool_s);
    auto charlie = ctx.make_symbol("Charlie", bool_s);

    // 2. Build constraints
    Term c1 = alice || bob || charlie;
    Term c2 = (!alice) || (!bob);
    Term c3 = (!charlie) || alice;
    Term c4 = !bob;

    // 3. Assert into solver
    solver.assert_formula(c1 && c2 && c3 && c4);

    // 4. Check satisfiability
    auto res = solver.check_sat();
    if (res && *res == SatResult::Sat) {
        std::cout << "[Act 1] Roster Found:\n";
        std::cout << "  Alice:   " << (std::get<bool>(*solver.get_value(alice)) ? "ON DUTY" : "OFF") << "\n";
        std::cout << "  Bob:     " << (std::get<bool>(*solver.get_value(bob)) ? "ON DUTY" : "OFF") << "\n";
        std::cout << "  Charlie: " << (std::get<bool>(*solver.get_value(charlie)) ? "ON DUTY" : "OFF") << "\n";
    }
}
```

---

## Act 2: The Cryptographic Vault (BitVectors & Binary Arithmetic — QF_BV)

### The Mystery
The ancient treasure vault is locked with a 32-bit passcode $X$. The inscription reads:
$$(X \oplus \text{0xCAFEBABE}) + 42 = \text{0xDEADBEEF}$$
Furthermore, the unsigned division of $X$ by 16 must equal 0x076B2026.

### The Code
```cpp
void act2_crack_vault() {
    Context ctx;
    RouterEngine<backend::native> solver;

    auto bv32 = ctx.bv_sort(32);

    auto x      = ctx.make_symbol("x", bv32);
    auto mask   = ctx.make_value(0xCAFEBABEU, bv32);
    auto c42    = ctx.make_value(42U, bv32);
    auto target = ctx.make_value(0xDEADBEEFU, bv32);
    auto c16    = ctx.make_value(16U, bv32);

    // (x ^ mask) + 42 == target
    Term xor_t = ctx.make_term(Op::BvXor, bv32, {x, mask});
    Term add_t = ctx.make_term(Op::BvAdd, bv32, {xor_t, c42});
    Term eq1   = (add_t == target);

    // x / 16 (unsigned division circuit)
    Term div_t = ctx.make_term(Op::BvUdiv, bv32, {x, c16});

    solver.assert_formula(eq1);

    auto res = solver.check_sat();
    if (res && *res == SatResult::Sat) {
        auto val = std::get<bv_value>(*solver.get_value(x));
        std::cout << "[Act 2] Vault Key Recovered: 0x" 
                  << std::hex << std::uppercase << val.bits << std::dec << "\n";
    }
}
```

---

## Act 3: The King's Ledger (Linear Real & Integer Arithmetic — QF_LRA / QF_LIA)

### The Problem
The Island Treasury is budgeting resources between **Lighthouse Maintenance** ($L$) and **Harbor Dredging** ($H$).
1. The total budget cannot exceed 100 thousand gold coins: $L + H \le 100$.
2. Dredging must be allocated at least twice the lighthouse budget plus 10 thousand coins: $H \ge 2L + 10$.
3. The lighthouse requires a minimum of 25 thousand coins: $L \ge 25$.

Tarka uses **exact rational arithmetic** (`containers::numeric::exact_rational`), completely avoiding floating-point precision loss.

### The Code
```cpp
void act3_balance_treasury() {
    Context ctx;
    RouterEngine<backend::native> solver;

    auto real_s = ctx.real_sort();

    auto l = ctx.make_symbol("L", real_s);
    auto h = ctx.make_symbol("H", real_s);

    auto c100 = ctx.make_real(100, 1, real_s);
    auto c10  = ctx.make_real(10, 1, real_s);
    auto c25  = ctx.make_real(25, 1, real_s);
    auto c2   = ctx.make_real(2, 1, real_s);

    Term total_budget = (ctx.make_term(Op::Add, real_s, {l, h}) <= c100);
    Term dredging_req = (h >= ctx.make_term(Op::Add, real_s, {ctx.make_term(Op::Mul, real_s, {c2, l}), c10}));
    Term lighthouse_min = (l >= c25);

    solver.assert_formula(total_budget && dredging_req && lighthouse_min);

    auto res = solver.check_sat();
    if (res && *res == SatResult::Sat) {
        auto l_val = std::get<rational>(*solver.get_value(l));
        auto h_val = std::get<rational>(*solver.get_value(h));
        std::cout << "[Act 3] Exact Budget Allocation:\n"
                  << "  Lighthouse: " << l_val.num << "/" << l_val.den << "k gold\n"
                  << "  Dredging:   " << h_val.num << "/" << h_val.den << "k gold\n";
    }
}
```

---

## Act 4: The Black-Box Guilds (Equality with Uninterpreted Functions — QF_UF)

### The Problem
You do not know the proprietary algorithm used by the Alchemist Guild's function $f(x)$. However, by the **Congruence Axiom**, if inputs are identical, outputs must be identical:
$$a = b \implies f(a) = f(b)$$

Let's prove that given $x = y$ and $y = z$, it is impossible for $f(x) \ne f(z)$.

### The Code
```cpp
void act4_verify_congruence() {
    Context ctx;
    RouterEngine<backend::native> solver;

    auto u_sort = ctx.string_sort(); // Uninterpreted sort domain
    auto f_sort = ctx.function_sort(std::vector<Sort>{u_sort}, u_sort);

    auto f = ctx.make_symbol("f", f_sort);
    auto x = ctx.make_symbol("x", u_sort);
    auto y = ctx.make_symbol("y", u_sort);
    auto z = ctx.make_symbol("z", u_sort);

    Term fx = ctx.make_term(Op::Apply, u_sort, {f, x});
    Term fz = ctx.make_term(Op::Apply, u_sort, {f, z});

    // We assert: (x == y) AND (y == z) AND (f(x) != f(z)) [Searching for a counterexample]
    solver.assert_formula((x == y) && (y == z) && (fx != fz));

    auto res = solver.check_sat();
    if (res && *res == SatResult::Unsat) {
        std::cout << "[Act 4] Congruence Proved: No counterexample exists (UNSAT)!\n";
    }
}
```

---

## Act 5: The Secure Memory Driver (Array Theory & Extensionality — QF_AX)

### The Problem
Arrays in SMT represent unbounded memory spaces.
- `store(arr, idx, val)` returns a new array with `val` written to `idx`.
- `select(arr, idx)` reads the value at `idx`.

**Axiom 1 (Read-over-Write)**: $\text{select}(\text{store}(A, i, v), i) = v$.  
**Axiom 2 (Isolation)**: $i \ne j \implies \text{select}(\text{store}(A, i, v), j) = \text{select}(A, j)$.  
**Axiom 3 (Extensionality)**: If two arrays differ ($A \ne B$), there exists a witness address $k$ such that $\text{select}(A, k) \ne \text{select}(B, k)$.

### The Code
```cpp
void act5_verify_memory_safety() {
    Context ctx;
    RouterEngine<backend::native> solver;

    auto bv32 = ctx.bv_sort(32);
    auto mem_s = ctx.array_sort(bv32, bv32);

    auto mem = ctx.make_symbol("mem", mem_s);
    auto addr_i = ctx.make_symbol("i", bv32);
    auto addr_j = ctx.make_symbol("j", bv32);
    auto secret = ctx.make_symbol("secret", bv32);

    // Write secret to address i
    Term updated_mem = ctx.make_term(Op::Store, mem_s, {mem, addr_i, secret});

    // Read from address j
    Term read_j = ctx.make_term(Op::Select, bv32, {updated_mem, addr_j});
    Term orig_j = ctx.make_term(Op::Select, bv32, {mem, addr_j});

    // Check if an unrelated read at j can leak the secret when i != j
    Term leak_condition = (addr_i != addr_j) && (read_j != orig_j);

    solver.assert_formula(leak_condition);

    auto res = solver.check_sat();
    if (res && *res == SatResult::Unsat) {
        std::cout << "[Act 5] Memory isolation verified mathematically (UNSAT)!\n";
    }
}
```

---

## Act 6: Multi-Theory Synthesis (QF_AUFBV Combination)

In modern verification, systems use Arrays, BitVectors, and Uninterpreted Functions together. Tarka integrates them through Nelson-Oppen multi-theory propagation.

```cpp
void act6_combined_theories() {
    Context ctx;
    RouterEngine<backend::native> solver;

    auto bv32  = ctx.bv_sort(32);
    auto arr_s = ctx.array_sort(bv32, bv32);
    auto fn_s  = ctx.function_sort(std::vector<Sort>{bv32}, bv32);

    auto f   = ctx.make_symbol("f", fn_s);
    auto arr = ctx.make_symbol("arr", arr_s);
    auto idx = ctx.make_symbol("idx", bv32);
    auto val = ctx.make_symbol("val", bv32);
    auto key = ctx.make_symbol("key", bv32);

    // stored = store(arr, idx, val)
    Term stored = ctx.make_term(Op::Store, arr_s, {arr, idx, val});
    // selected = select(stored, key)
    Term selected = ctx.make_term(Op::Select, bv32, {stored, key});

    Term f_selected = ctx.make_term(Op::Apply, bv32, {f, selected});
    Term f_val      = ctx.make_term(Op::Apply, bv32, {f, val});

    // If idx == key, then f(select(store(arr, idx, val), key)) MUST equal f(val)
    Term formula = (idx == key) && (f_selected != f_val);

    solver.assert_formula(formula);
    auto res = solver.check_sat();
    if (res && *res == SatResult::Unsat) {
        std::cout << "[Act 6] Multi-theory combination holds (UNSAT)!\n";
    }
}
```

---

## Act 7: Universal Laws (Quantifier Reasoning & E-Matching)

Tarka's native quantifier engine supports:
- **`Op::Exists`**: Skolemization ($\exists x. P(x) \implies P(c_{\text{skolem}})$).
- **`Op::Forall`**: E-matching over active ground terms in EUF equivalence classes.

```cpp
void act7_universal_quantifier() {
    Context ctx;
    RouterEngine<backend::native> solver;

    auto bv32 = ctx.bv_sort(32);
    auto bool_s = ctx.bool_sort();

    auto x   = ctx.make_symbol("x", bv32);
    auto y   = ctx.make_symbol("y", bv32);
    auto c10 = ctx.make_value(10U, bv32);
    auto c20 = ctx.make_value(20U, bv32);

    // Universal Law: forall x. (x == 10)
    Term forall_rule = ctx.make_term(Op::Forall, bool_s, {x, (x == c10)});

    // Observation: y == 20
    Term observation = (y == c20);

    // Asserting both forces E-matching to substitute y into the rule, discovering 20 == 10 (Conflict!)
    solver.assert_formula(forall_rule && observation);

    auto res = solver.check_sat();
    if (res && *res == SatResult::Unsat) {
        std::cout << "[Act 7] Quantified contradiction discovered (UNSAT)!\n";
    }
}
```

---

## Act 8: The Detective's Inquiry (Assumptions & Minimal Unsat Cores)

When a complex system of rules fails, you need to know **which specific rules** caused the contradiction. Tarka solves under temporary assumptions and returns the **Unsat Core**.

```cpp
void act8_diagnose_unsat_core() {
    Context ctx;
    RouterEngine<backend::native> solver;

    auto bv32 = ctx.bv_sort(32);
    auto x = ctx.make_symbol("x", bv32);

    auto v1 = ctx.make_value(1U, bv32);
    auto v2 = ctx.make_value(2U, bv32);
    auto v3 = ctx.make_value(3U, bv32);

    Term req_a = (x == v1);
    Term req_b = (x == v2);
    Term req_c = (x != v3);

    std::vector<Term> assumptions = {req_a, req_b, req_c};
    auto res = solver.check_sat_assuming(assumptions);

    if (res && *res == SatResult::Unsat) {
        std::cout << "[Act 8] Conflict Diagnosed! Minimal Unsat Core:\n";
        auto core = solver.get_unsat_core();
        for (Term t : core) {
            std::cout << "  - Conflicting Rule: " << smt2_printer::to_string(t) << "\n";
        }
    }
}
```

---

## Act 9: The Preprocessing Crucible (Algebraic Simplification & E-Graphs)

Before formulas enter the CDCL SAT engine, Tarka applies pre-encoding algebraic normalization and constant folding (`simplifier.hpp`) along with Equality Saturation (`egraph_opt.hpp`):

- $\neg(\neg x) \to x$
- $x \land \text{true} \to x$, $x \lor \text{false} \to x$
- $x \oplus x \to 0$, $x - x \to 0$
- $\text{select}(\text{store}(A, i, v), i) \to v$

```cpp
void act9_pre_simplification() {
    Context ctx;
    auto bv32 = ctx.bv_sort(32);
    auto x = ctx.make_symbol("x", bv32);

    // x ^ x
    Term xor_self = ctx.make_term(Op::BvXor, bv32, {x, x});
    Term simplified = simplifier::simplify(xor_self);

    std::cout << "[Act 9] Original:   " << smt2_printer::to_string(xor_self) << "\n";
    std::cout << "        Simplified: " << smt2_printer::to_string(simplified) << "\n";
}
```

---

## Act 10: Talking SMT-LIB2 (Parsing & Serializing Scripts)

Tarka can parse `.smt2` scripts and serialize AST terms into standard SMT-LIB2 format.

```cpp
void act10_smt2_interop() {
    Context ctx;
    RouterEngine<backend::native> solver;

    std::string_view script = R"(
        (set-logic QF_BV)
        (declare-const a (_ BitVec 32))
        (declare-const b (_ BitVec 32))
        (assert (= (bvadd a b) #x00000030))
        (assert (= a #x00000010))
        (check-sat)
    )";

    auto parsed = parse_smt2_lexy(script);
    auto status = lower_to_tarka(parsed, ctx, solver);
    if (status && status->last_result && *status->last_result == SatResult::Sat) {
        std::cout << "[Act 10] SMT2 Script Executed Successfully: SAT!\n";
    }

    // Export AST term back to SMT-LIB2
    auto bv32 = ctx.bv_sort(32);
    auto k = ctx.make_symbol("key", bv32);
    std::string exported = smt2_printer::to_smt2_script(std::vector<Term>{k == ctx.make_value(100, bv32)}, "QF_BV");
    std::cout << "[Act 10] Serialized Benchmark:\n" << exported;
}
```

---

## Act 11: Trust But Mathematically Verify (Model Validator)

When a solver claims `Sat`, can you trust the certificate? Tarka includes an independent **Model Validator** that substitutes extracted concrete values back into the original mathematical equations.

```cpp
void act11_validate_model() {
    Context ctx;
    RouterEngine<backend::native> solver;

    auto bv32 = ctx.bv_sort(32);
    auto x = ctx.make_symbol("x", bv32);
    auto y = ctx.make_symbol("y", bv32);

    Term eq_x = (x == ctx.make_value(10, bv32));
    Term eq_sum = (ctx.make_term(Op::BvAdd, bv32, {x, y}) == ctx.make_value(35, bv32));

    solver.assert_formula(eq_x && eq_sum);
    if (solver.check_sat() == SatResult::Sat) {
        std::unordered_map<Term, SmtValue> model;
        model[x] = *solver.get_value(x);
        model[y] = *solver.get_value(y);

        // Format model
        std::cout << "[Act 11] Model Definition:\n" << model_validator::format_model(model);

        // Independently validate constraints against model
        auto validation = model_validator::validate(std::vector<Term>{eq_x, eq_sum}, model);
        std::cout << "         Certificate Validated: " << (validation.is_valid ? "TRUE (100% Correct)" : "FALSE") << "\n";
    }
}
```

---

## Act 12: Parallel Races (Competitive Multi-Engine Portfolio)

When solving critical queries, you can race `backend::native` alongside external backends (such as `z3_backend`) concurrently across a persistent lock-free worker thread pool. The first engine to find a definitive answer cancels all other workers with zero thread hangs.

```cpp
void act12_portfolio_solving() {
    WorkerPool pool(4); // Persistent 4-thread worker pool
    PortfolioEngine<backend::native> portfolio(pool);

    Context ctx;
    auto bv32 = ctx.bv_sort(32);
    auto a = ctx.make_symbol("a", bv32);
    auto b = ctx.make_symbol("b", bv32);

    Term hard_query = (ctx.make_term(Op::BvMul, bv32, {a, b}) == ctx.make_value(143, bv32));

    auto fut = portfolio.check_sat_portfolio(hard_query);
    fut.wait();
    auto res = fut.get();

    if (res && *res == SatResult::Sat) {
        std::cout << "[Act 12] Portfolio race finished: SAT!\n";
    }
}
```

---

## 15. Quick API Reference & Cheat Sheet

| Category | API Call | Description |
|---|---|---|
| **Sort Creation** | `ctx.bool_sort()`, `ctx.int_sort()`, `ctx.real_sort()`, `ctx.bv_sort(N)`, `ctx.array_sort(I, E)`, `ctx.function_sort(D, R)` | Allocate canonical 16-byte sort handles |
| **Constants** | `ctx.make_bool(b)`, `ctx.make_int(v, s)`, `ctx.make_real(num, den)`, `ctx.make_value(bits, bv_sort)` | Construct typed constant values |
| **Symbols** | `ctx.make_symbol("name", sort)` | Construct symbolic variable |
| **BitVector Ops** | `Op::BvAdd`, `Op::BvSub`, `Op::BvMul`, `Op::BvUdiv`, `Op::BvSdiv`, `Op::BvUrem`, `Op::BvSrem`, `Op::BvAnd`, `Op::BvOr`, `Op::BvXor`, `Op::BvShl`, `Op::BvLshr`, `Op::BvAshr`, `Op::BvUlt`, `Op::BvSlt` | Exact bit-blasted operations |
| **Array Ops** | `Op::Select`, `Op::Store` | Memory map reads and writes |
| **Solving** | `solver.assert_formula(t)`, `solver.check_sat()`, `solver.check_sat_assuming(assumptions)` | Drive the DPLL(T) solver |
| **Model** | `solver.get_value(t)`, `model_validator::format_model(m)`, `model_validator::validate(asserts, m)` | Extract, format, and verify SAT certificates |
| **Diagnostics** | `solver.get_unsat_core()` | Extract minimal contradictory sub-formulas |
| **SMT-LIB2** | `parse_smt2_lexy(str)` or `parse_smt2_samasa(str)`, then `lower_to_tarka(...)` | Parse through shared IR; print benchmarks |
