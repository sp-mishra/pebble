// =============================================================================
// test_tarka.cpp — Unit tests for Tarka SMT substrate and native solver
//
// Tests:
//   1. Term Algebra & Hash-Consing (16B handles, CSE, operators).
//   2. Propositional CDCL SAT solver (BCP, 1UIP, Tseitin PG).
//   3. Difference Logic (QF_IDL / QF_RDL, negative cycle detection).
//   4. Equality with Uninterpreted Functions (QF_UF / EUF Congruence Closure).
//   5. Bit-Vectors (QF_BV bit-blasting, arithmetic, bitwise, model extraction).
//   6. Array Theory (QF_AX read-over-write axioms).
//   7. Linear Real & Integer Arithmetic (QF_LRA Simplex).
//   8. Multi-theory combination & Nelson-Oppen integration.
//   9. Incremental solving (push / pop / reset) and model generation.
// =============================================================================

#include "catch_amalgamated.hpp"

#include "tarka/tarka.hpp"
#include "tarka/backends/native_backend.hpp"
#include "tarka/backends/z3_backend.hpp"
#include "tarka/frontend/smt2_lexy.hpp"
#include "tarka/frontend/smt2_samasa.hpp"
#include "tarka/frontend/lower_to_tarka.hpp"
#include "tarka/frontend/smt2_printer.hpp"
#include "tarka/native/model_validator.hpp"
#include "containers/associative/order_heap.hpp"
#include "tarka/egraph_opt.hpp"
#include "tarka/native/simplifier.hpp"

using namespace tarka;
using namespace tarka::backend;
using namespace tarka::frontend;
using namespace tarka::native;

TEST_CASE (
"tarka: Term and Sort handle invariants"
,
"[tarka][term]"
)
 {
    Context ctx;

    auto b_sort = ctx.bool_sort();
    auto i_sort = ctx.int_sort();
    auto r_sort = ctx.real_sort();
    auto bv32 = ctx.bv_sort(32);

    REQUIRE(b_sort.kind() == SortKind::Bool);
    REQUIRE(i_sort.kind() == SortKind::Int);
    REQUIRE(r_sort.kind() == SortKind::Real);
    REQUIRE(bv32.kind() == SortKind::BitVec);
    REQUIRE(bv32.scalar_param() == 32);

    // 16B trivially copyable handles
    static_assert(sizeof(Sort) == 16);
    static_assert(sizeof(Term) == 16);
    static_assert(std::is_trivially_copyable_v<Sort>);
    static_assert(std::is_trivially_copyable_v<Term>);

    // CSE: identical terms share node address
    auto x1 = ctx.make_symbol("x", bv32);
    auto x2 = ctx.make_symbol("x", bv32);
    REQUIRE(x1.ptr() == x2.ptr());

    auto v1 = ctx.make_value(std::uint64_t{42}, bv32);
    auto v2 = ctx.make_value(std::uint64_t{42}, bv32);
    REQUIRE(v1.ptr() == v2.ptr());

    // Operator expressions recover Context
    auto f1 = (x1 + v1 == v2);
    auto f2 = (x2 + v2 == v1);
    REQUIRE(f1.valid());
    REQUIRE(f2.valid());
}

TEST_CASE (
"tarka native: Propositional SAT solving"
,
"[tarka][native][sat]"
)
 {
    Context ctx;
    auto b_sort = ctx.bool_sort();

    auto a = ctx.make_symbol("a", b_sort);
    auto b = ctx.make_symbol("b", b_sort);
    auto c = ctx.make_symbol("c", b_sort);

    RouterEngine<backend::native> solver;

    SECTION("Satisfiable 3-SAT formula") {
        // (a || b) && (!a || c) && (!b || !c)
        Term formula = (a || b) && (!a || c) && (!b || !c);
        solver.assert_formula(formula);

        auto res = solver.check_sat();
        REQUIRE(res.has_value());
        REQUIRE(*res == SatResult::Sat);

        auto a_val = solver.get_value(a);
        auto b_val = solver.get_value(b);
        auto c_val = solver.get_value(c);

        REQUIRE(a_val.has_value());
        REQUIRE(b_val.has_value());
        REQUIRE(c_val.has_value());
        REQUIRE(std::holds_alternative<bool>(*a_val));
    }

    SECTION("Unsatisfiable formula (contradiction)") {
        // a && !a
        Term formula = a && (!a);
        solver.assert_formula(formula);

        auto res = solver.check_sat();
        REQUIRE(res.has_value());
        REQUIRE(*res == SatResult::Unsat);
    }
}

TEST_CASE (
"tarka native: Difference Logic (QF_IDL / QF_RDL)"
,
"[tarka][native][dl]"
)
 {
    Context ctx;
    auto i_sort = ctx.int_sort();

    auto x = ctx.make_symbol("x", i_sort);
    auto y = ctx.make_symbol("y", i_sort);
    auto z = ctx.make_symbol("z", i_sort);

    auto v0 = ctx.make_int(0, i_sort);
    auto v5 = ctx.make_int(5, i_sort);
    auto v10 = ctx.make_int(10, i_sort);

    RouterEngine<backend::native> solver;

    SECTION("Satisfiable difference constraints") {
        // x - y <= 5 && y - z <= 5 && x - z <= 10
        Term c1 = (x - y <= v5);
        Term c2 = (y - z <= v5);
        Term c3 = (x - z <= v10);

        solver.assert_formula(c1 && c2 && c3);
        auto res = solver.check_sat();
        REQUIRE(res.has_value());
        REQUIRE(*res == SatResult::Sat);

        auto xv = solver.get_value(x);
        REQUIRE(xv.has_value());
        REQUIRE(std::holds_alternative<std::int64_t>(*xv));
    }

    SECTION("Negative cycle (Unsatisfiable)") {
        // x - y <= 2 && y - z <= 3 && z - x <= -6 => cycle sum = 2 + 3 - 6 = -1 < 0
        auto v2 = ctx.make_int(2, i_sort);
        auto v3 = ctx.make_int(3, i_sort);
        auto vn6 = ctx.make_int(-6, i_sort);

        Term c1 = (x - y <= v2);
        Term c2 = (y - z <= v3);
        Term c3 = (z - x <= vn6);

        solver.assert_formula(c1 && c2 && c3);
        auto res = solver.check_sat();
        REQUIRE(res.has_value());
        REQUIRE(*res == SatResult::Unsat);
    }
}

TEST_CASE (
"tarka native: Equality & Uninterpreted Functions (QF_UF)"
,
"[tarka][native][uf]"
)
 {
    Context ctx;
    auto u_sort = ctx.make_sort(SortKind::String); // uninterpreted sort

    auto a = ctx.make_symbol("a", u_sort);
    auto b = ctx.make_symbol("b", u_sort);
    auto c = ctx.make_symbol("c", u_sort);

    auto fn_sort = ctx.function_sort(std::vector<Sort>{u_sort}, u_sort);
    auto f_sym = ctx.make_symbol("f", fn_sort);

    auto fa = ctx.make_term(Op::Apply, u_sort, std::vector<Term>{f_sym, a});
    auto fb = ctx.make_term(Op::Apply, u_sort, std::vector<Term>{f_sym, b});
    auto ffa = ctx.make_term(Op::Apply, u_sort, std::vector<Term>{f_sym, fa});

    RouterEngine<backend::native> solver;

    SECTION("Congruence transitivity: a == b => f(a) == f(b)") {
        // a == b && f(a) != f(b) => UNSAT
        Term eq_ab = (a == b);
        Term neq_fafb = (fa != fb);

        solver.assert_formula(eq_ab && neq_fafb);
        auto res = solver.check_sat();
        REQUIRE(res.has_value());
        REQUIRE(*res == SatResult::Unsat);
    }

    SECTION("Multi-step congruence: f(f(a)) == a && f(f(f(a))) == a => f(a) == a") {
        auto fffa = ctx.make_term(Op::Apply, u_sort, std::vector<Term>{f_sym, ffa});
        Term eq1 = (ffa == a);
        Term eq2 = (fffa == a);
        Term neq = (fa != a);

        solver.assert_formula(eq1 && eq2 && neq);
        auto res = solver.check_sat();
        REQUIRE(res.has_value());
        REQUIRE(*res == SatResult::Unsat);
    }
}

TEST_CASE (
"tarka native: Bit-Vectors (QF_BV)"
,
"[tarka][native][bv]"
)
 {
    Context ctx;
    auto bv8 = ctx.bv_sort(8);

    auto x = ctx.make_symbol("x", bv8);
    auto y = ctx.make_symbol("y", bv8);

    auto v10 = ctx.make_value(10, bv8);
    auto v20 = ctx.make_value(20, bv8);
    auto v30 = ctx.make_value(30, bv8);

    RouterEngine<backend::native> solver;

    SECTION("BV Addition and Model Extraction: x + y == 30 && x == 10 => y == 20") {
        Term add_term = ctx.make_term(Op::BvAdd, bv8, std::vector<Term>{x, y});
        Term eq1 = (add_term == v30);
        Term eq2 = (x == v10);

        solver.assert_formula(eq1 && eq2);
        auto res = solver.check_sat();
        REQUIRE(res.has_value());
        REQUIRE(*res == SatResult::Sat);

        auto y_val = solver.get_value(y);
        REQUIRE(y_val.has_value());
        REQUIRE(std::holds_alternative<bv_value>(*y_val));
        const auto& bv = std::get<bv_value>(*y_val);
        REQUIRE(bv.bits == 20);
        REQUIRE(bv.width == 8);
    }

    SECTION("BV Bitwise and Overflow: (x & 0x0F) == 0x05 && (x & 0xF0) == 0xA0 => x == 0xA5") {
        auto mask_lo = ctx.make_value(0x0F, bv8);
        auto mask_hi = ctx.make_value(0xF0, bv8);
        auto val_lo = ctx.make_value(0x05, bv8);
        auto val_hi = ctx.make_value(0xA0, bv8);

        Term and_lo = ctx.make_term(Op::BvAnd, bv8, std::vector<Term>{x, mask_lo});
        Term and_hi = ctx.make_term(Op::BvAnd, bv8, std::vector<Term>{x, mask_hi});

        solver.assert_formula((and_lo == val_lo) && (and_hi == val_hi));
        auto res = solver.check_sat();
        REQUIRE(res.has_value());
        REQUIRE(*res == SatResult::Sat);

        auto x_val = solver.get_value(x);
        REQUIRE(x_val.has_value());
        const auto& bv = std::get<bv_value>(*x_val);
        REQUIRE(bv.bits == 0xA5);
    }

    SECTION("BV Comparisons: x < 5 && x > 10 => UNSAT") {
        auto v5 = ctx.make_value(5, bv8);
        Term lt5 = ctx.make_term(Op::BvUlt, ctx.bool_sort(), std::vector<Term>{x, v5});
        Term gt10 = ctx.make_term(Op::BvUlt, ctx.bool_sort(), std::vector<Term>{v10, x});

        solver.assert_formula(lt5 && gt10);
        auto res = solver.check_sat();
        REQUIRE(res.has_value());
        REQUIRE(*res == SatResult::Unsat);
    }
}

TEST_CASE (
"tarka native: Array Theory (QF_AX)"
,
"[tarka][native][array]"
)
 {
    Context ctx;
    auto bv32 = ctx.bv_sort(32);
    auto arr_sort = ctx.array_sort(bv32, bv32);

    auto a = ctx.make_symbol("a", arr_sort);
    auto i = ctx.make_symbol("i", bv32);
    auto j = ctx.make_symbol("j", bv32);
    auto v = ctx.make_symbol("v", bv32);

    // store(a, i, v)
    auto a_prime = ctx.make_term(Op::Store, arr_sort, std::vector<Term>{a, i, v});
    // select(store(a, i, v), i)
    auto sel_hit = ctx.make_term(Op::Select, bv32, std::vector<Term>{a_prime, i});

    RouterEngine<backend::native> solver;

    SECTION("Read-over-write hit: select(store(a, i, v), i) == v") {
        Term neq = (sel_hit != v);
        solver.assert_formula(neq);
        auto res = solver.check_sat();
        REQUIRE(res.has_value());
        REQUIRE(*res == SatResult::Unsat);
    }

    SECTION("Read-over-write miss: i != j => select(store(a, i, v), j) == select(a, j)") {
        auto sel_miss = ctx.make_term(Op::Select, bv32, std::vector<Term>{a_prime, j});
        auto sel_orig = ctx.make_term(Op::Select, bv32, std::vector<Term>{a, j});

        Term neq_ij = (i != j);
        Term neq_sel = (sel_miss != sel_orig);

        solver.assert_formula(neq_ij && neq_sel);
        auto res = solver.check_sat();
        REQUIRE(res.has_value());
        REQUIRE(*res == SatResult::Unsat);
    }
}

TEST_CASE (
"tarka native: Incremental push/pop scoping"
,
"[tarka][native][incremental]"
)
 {
    Context ctx;
    auto bv32 = ctx.bv_sort(32);

    auto x = ctx.make_symbol("x", bv32);
    auto v10 = ctx.make_value(10, bv32);
    auto v20 = ctx.make_value(20, bv32);

    RouterEngine<backend::native> solver;

    solver.assert_formula(x == v10);
    auto r1 = solver.check_sat();
    REQUIRE(r1.has_value());
    REQUIRE(*r1 == SatResult::Sat);

    solver.push();
    solver.assert_formula(x == v20); // contradicts x == 10
    auto r2 = solver.check_sat();
    REQUIRE(r2.has_value());
    REQUIRE(*r2 == SatResult::Unsat);

    solver.pop();
    auto r3 = solver.check_sat(); // restored to Sat
    REQUIRE(r3.has_value());
    REQUIRE(*r3 == SatResult::Sat);
}

TEST_CASE (
"tarka native: BitVector Arithmetic & Bitwise Logic (QF_BV)"
,
"[tarka][native][bv]"
)
 {
    Context ctx;
    auto bv32 = ctx.bv_sort(32);
    auto bool_s = ctx.bool_sort();

    auto x = ctx.make_symbol("x", bv32);
    auto y = ctx.make_symbol("y", bv32);
    auto z = ctx.make_symbol("z", bv32);

    RouterEngine<backend::native> solver;

    SECTION("Bitwise XOR identity: (x ^ x) != 0 is UNSAT") {
        auto zero = ctx.make_value(0, bv32);
        Term xor_xx = ctx.make_term(Op::BvXor, bv32, std::vector<Term>{x, x});
        Term neq_zero = (xor_xx != zero);

        solver.assert_formula(neq_zero);
        auto res = solver.check_sat();
        REQUIRE(res.has_value());
        REQUIRE(*res == SatResult::Unsat);
    }

    SECTION("De Morgan's law over BitVectors: ~(x & y) != (~x | ~y) is UNSAT") {
        Term xy_and = ctx.make_term(Op::BvAnd, bv32, std::vector<Term>{x, y});
        Term not_and = ctx.make_term(Op::BvNot, bv32, std::vector<Term>{xy_and});
        Term not_x = ctx.make_term(Op::BvNot, bv32, std::vector<Term>{x});
        Term not_y = ctx.make_term(Op::BvNot, bv32, std::vector<Term>{y});
        Term or_not = ctx.make_term(Op::BvOr, bv32, std::vector<Term>{not_x, not_y});
        Term neq = (not_and != or_not);

        solver.assert_formula(neq);
        auto res = solver.check_sat();
        REQUIRE(res.has_value());
        REQUIRE(*res == SatResult::Unsat);
    }

    SECTION("Transitivity of signed less-than: x <s y && y <s z && z <s x is UNSAT") {
        Term c1 = ctx.make_term(Op::BvSlt, bool_s, std::vector<Term>{x, y});
        Term c2 = ctx.make_term(Op::BvSlt, bool_s, std::vector<Term>{y, z});
        Term c3 = ctx.make_term(Op::BvSlt, bool_s, std::vector<Term>{z, x});

        solver.assert_formula(c1 && c2 && c3);
        auto res = solver.check_sat();
        REQUIRE(res.has_value());
        REQUIRE(*res == SatResult::Unsat);
    }

    SECTION("Bit-masking satisfiable solution with model extraction") {
        auto mask = ctx.make_value(0xFF, bv32);
        auto target = ctx.make_value(0xAB, bv32);
        Term x_masked = ctx.make_term(Op::BvAnd, bv32, std::vector<Term>{x, mask});
        Term masked = (x_masked == target);

        solver.assert_formula(masked);
        auto res = solver.check_sat();
        REQUIRE(res.has_value());
        REQUIRE(*res == SatResult::Sat);

        auto val = solver.get_value(x);
        REQUIRE(val.has_value());
        REQUIRE(std::holds_alternative<bv_value>(*val));
        auto bv = std::get<bv_value>(*val);
        REQUIRE((bv.bits & 0xFF) == 0xAB);
    }
}

TEST_CASE (
"tarka native: Multi-Argument EUF & Congruence Closure (QF_UF)"
,
"[tarka][native][uf]"
)
 {
    Context ctx;
    auto u_sort = ctx.string_sort();
    auto f_sort = ctx.function_sort(std::vector<Sort>{u_sort, u_sort}, u_sort);

    auto f = ctx.make_symbol("f", f_sort);
    auto x1 = ctx.make_symbol("x1", u_sort);
    auto x2 = ctx.make_symbol("x2", u_sort);
    auto y1 = ctx.make_symbol("y1", u_sort);
    auto y2 = ctx.make_symbol("y2", u_sort);

    RouterEngine<backend::native> solver;

    SECTION("Multi-arg congruence: x1 == y1 && x2 == y2 => f(x1, x2) == f(y1, y2)") {
        Term fx = ctx.make_term(Op::Apply, u_sort, std::vector<Term>{f, x1, x2});
        Term fy = ctx.make_term(Op::Apply, u_sort, std::vector<Term>{f, y1, y2});

        Term eq1 = (x1 == y1);
        Term eq2 = (x2 == y2);
        Term neq_f = (fx != fy);

        solver.assert_formula(eq1 && eq2 && neq_f);
        auto res = solver.check_sat();
        REQUIRE(res.has_value());
        REQUIRE(*res == SatResult::Unsat);
    }

    SECTION("Functional permutation congruence chain") {
        auto g_sort = ctx.function_sort(std::vector<Sort>{u_sort}, u_sort);
        auto g = ctx.make_symbol("g", g_sort);
        auto a = ctx.make_symbol("a", u_sort);

        // g(g(g(a))) == a && g(g(g(g(g(a))))) == a && g(a) != a => UNSAT (gcd(3, 5) = 1)
        Term g1 = ctx.make_term(Op::Apply, u_sort, std::vector<Term>{g, a});
        Term g2 = ctx.make_term(Op::Apply, u_sort, std::vector<Term>{g, g1});
        Term g3 = ctx.make_term(Op::Apply, u_sort, std::vector<Term>{g, g2});
        Term g4 = ctx.make_term(Op::Apply, u_sort, std::vector<Term>{g, g3});
        Term g5 = ctx.make_term(Op::Apply, u_sort, std::vector<Term>{g, g4});

        Term c1 = (g3 == a);
        Term c2 = (g5 == a);
        Term c3 = (g1 != a);

        solver.assert_formula(c1 && c2 && c3);
        auto res = solver.check_sat();
        REQUIRE(res.has_value());
        REQUIRE(*res == SatResult::Unsat);
    }
}

TEST_CASE (
"tarka native: Linear Real Arithmetic Bounds & Feasibility (QF_LRA)"
,
"[tarka][native][lra]"
)
 {
    Context ctx;
    auto r_sort = ctx.real_sort();

    auto x = ctx.make_symbol("x", r_sort);
    auto y = ctx.make_symbol("y", r_sort);

    RouterEngine<backend::native> solver;

    SECTION("Satisfiable 2D bounded box") {
        auto v1 = ctx.make_real(1, 1, r_sort);
        auto v5 = ctx.make_real(5, 1, r_sort);
        auto v2 = ctx.make_real(2, 1, r_sort);
        auto v6 = ctx.make_real(6, 1, r_sort);

        Term bx1 = (x >= v1);
        Term bx2 = (x <= v5);
        Term by1 = (y >= v2);
        Term by2 = (y <= v6);

        solver.assert_formula(bx1 && bx2 && by1 && by2);
        auto res = solver.check_sat();
        REQUIRE(res.has_value());
        REQUIRE(*res == SatResult::Sat);

        auto vx = solver.get_value(x);
        auto vy = solver.get_value(y);
        REQUIRE(vx.has_value());
        REQUIRE(vy.has_value());
    }

    SECTION("Infeasible 1D interval: x >= 5.0 && x <= 3.0") {
        auto v5 = ctx.make_real(5, 1, r_sort);
        auto v3 = ctx.make_real(3, 1, r_sort);

        Term b1 = (x >= v5);
        Term b2 = (x <= v3);

        solver.assert_formula(b1 && b2);
        auto res = solver.check_sat();
        REQUIRE(res.has_value());
        REQUIRE(*res == SatResult::Unsat);
    }
}

TEST_CASE (
"tarka native: Array Multi-Store & Transitivity (QF_AX)"
,
"[tarka][native][array]"
)
 {
    Context ctx;
    auto idx_sort = ctx.bv_sort(32);
    auto elem_sort = ctx.bv_sort(32);
    auto arr_sort = ctx.array_sort(idx_sort, elem_sort);

    auto a = ctx.make_symbol("a", arr_sort);
    auto i = ctx.make_symbol("i", idx_sort);
    auto j = ctx.make_symbol("j", idx_sort);
    auto v1 = ctx.make_value(100, elem_sort);
    auto v2 = ctx.make_value(200, elem_sort);

    RouterEngine<backend::native> solver;

    SECTION("Sequential store overwrite: select(store(store(a, i, v1), i, v2), i) == v2") {
        Term a1 = ctx.make_term(Op::Store, arr_sort, std::vector<Term>{a, i, v1});
        Term a2 = ctx.make_term(Op::Store, arr_sort, std::vector<Term>{a1, i, v2});
        Term sel = ctx.make_term(Op::Select, elem_sort, std::vector<Term>{a2, i});

        Term neq = (sel != v2);
        solver.assert_formula(neq);

        auto res = solver.check_sat();
        REQUIRE(res.has_value());
        REQUIRE(*res == SatResult::Unsat);
    }

    SECTION("Store commutativity at distinct indices") {
        // i != j => select(store(store(a, i, v1), j, v2), i) == v1
        Term a1 = ctx.make_term(Op::Store, arr_sort, std::vector<Term>{a, i, v1});
        Term a2 = ctx.make_term(Op::Store, arr_sort, std::vector<Term>{a1, j, v2});
        Term sel = ctx.make_term(Op::Select, elem_sort, std::vector<Term>{a2, i});

        Term distinct_indices = (i != j);
        Term not_equal_v1 = (sel != v1);

        solver.assert_formula(distinct_indices && not_equal_v1);
        auto res = solver.check_sat();
        REQUIRE(res.has_value());
        REQUIRE(*res == SatResult::Unsat);
    }
}

TEST_CASE (
"tarka native: Integrated Theory Combination (QF_AUFBV)"
,
"[tarka][native][combination]"
)
 {
    Context ctx;
    auto bv32 = ctx.bv_sort(32);
    auto arr_sort = ctx.array_sort(bv32, bv32);
    auto f_sort = ctx.function_sort(std::vector<Sort>{bv32}, bv32);

    auto f = ctx.make_symbol("f", f_sort);
    auto a = ctx.make_symbol("a", arr_sort);
    auto x = ctx.make_symbol("x", bv32);
    auto y = ctx.make_symbol("y", bv32);
    auto v = ctx.make_symbol("v", bv32);

    RouterEngine<backend::native> solver;

    SECTION("Combined Array + BV + UF: x == y => f(select(store(a, x, v), y)) == f(v)") {
        Term stored = ctx.make_term(Op::Store, arr_sort, std::vector<Term>{a, x, v});
        Term selected = ctx.make_term(Op::Select, bv32, std::vector<Term>{stored, y});
        Term f_sel = ctx.make_term(Op::Apply, bv32, std::vector<Term>{f, selected});
        Term f_v = ctx.make_term(Op::Apply, bv32, std::vector<Term>{f, v});

        Term eq_xy = (x == y);
        Term neq_f = (f_sel != f_v);

        solver.assert_formula(eq_xy && neq_f);
        auto res = solver.check_sat();
        REQUIRE(res.has_value());
        REQUIRE(*res == SatResult::Unsat);
    }
}

TEST_CASE (
"tarka native: BitVector Division & Modulo (QF_BV)"
,
"[tarka][native][bv][div]"
)
 {
    Context ctx;
    auto bv32 = ctx.bv_sort(32);

    auto x = ctx.make_symbol("x", bv32);
    auto v40 = ctx.make_value(40, bv32);
    auto v6 = ctx.make_value(6, bv32);
    auto v0 = ctx.make_value(0, bv32);

    RouterEngine<backend::native> solver;

    SECTION("Unsigned division: 40 / 6 == 6") {
        auto q = ctx.make_term(Op::BvUdiv, bv32, std::vector<Term>{v40, v6});
        auto expected = ctx.make_value(6, bv32);
        Term neq = (q != expected);

        solver.assert_formula(neq);
        auto res = solver.check_sat();
        REQUIRE(res.has_value());
        REQUIRE(*res == SatResult::Unsat);
    }

    SECTION("Unsigned modulo: 40 % 6 == 4") {
        auto r = ctx.make_term(Op::BvUrem, bv32, std::vector<Term>{v40, v6});
        auto expected = ctx.make_value(4, bv32);
        Term neq = (r != expected);

        solver.assert_formula(neq);
        auto res = solver.check_sat();
        REQUIRE(res.has_value());
        REQUIRE(*res == SatResult::Unsat);
    }

    SECTION("Division by zero: 40 / 0 == ~0") {
        auto q = ctx.make_term(Op::BvUdiv, bv32, std::vector<Term>{v40, v0});
        auto expected = ctx.make_value(0xFFFFFFFFULL, bv32);
        Term neq = (q != expected);

        solver.assert_formula(neq);
        auto res = solver.check_sat();
        REQUIRE(res.has_value());
        REQUIRE(*res == SatResult::Unsat);
    }
}

TEST_CASE (
"tarka native: Assumption-Based Solving & Unsat Core"
,
"[tarka][native][assumptions]"
)
 {
    Context ctx;
    auto bv32 = ctx.bv_sort(32);

    auto x = ctx.make_symbol("x", bv32);
    auto v1 = ctx.make_value(1, bv32);
    auto v2 = ctx.make_value(2, bv32);
    auto v3 = ctx.make_value(3, bv32);

    RouterEngine<backend::native> solver;

    Term a1 = (x == v1);
    Term a2 = (x == v2);
    Term a3 = (x != v3);

    std::vector<Term> assumptions = {a1, a2, a3};
    auto res = solver.check_sat_assuming(assumptions);
    REQUIRE(res.has_value());
    REQUIRE(*res == SatResult::Unsat);

    auto core = solver.get_unsat_core();
    REQUIRE(!core.empty());
}

TEST_CASE (
"tarka frontend: SMT-LIB2 Parser Script Execution"
,
"[tarka][frontend][smt2]"
)
 {
    Context ctx;
    RouterEngine<backend::native> solver;

    std::string_view smt2_script = R"(
        (set-logic QF_BV)
        (declare-const a (_ BitVec 32))
        (declare-const b (_ BitVec 32))
        (assert (= (bvadd a b) #x0000000A))
        (assert (= a #x00000004))
        (check-sat)
    )";

    auto parsed = parse_smt2_lexy(smt2_script);
    REQUIRE(parsed.valid());
    auto lowered = lower_to_tarka(parsed, ctx, solver);
    REQUIRE(lowered.has_value());
    auto sat_res = lowered->last_result;
    REQUIRE(sat_res.has_value());
    REQUIRE(*sat_res == SatResult::Sat);
}

TEST_CASE (
"tarka frontend: Lexy and Samasa share SMT script IR"
,
"[tarka][frontend][smt2][parity]"
)
 {
    constexpr std::string_view source = R"(
        (declare-const x Int)
        (assert (> x 0))
        (check-sat)
    )";
    const auto lexy = parse_smt2_lexy(source);
    const auto samasa = parse_smt2_samasa(source);
    REQUIRE(lexy.valid());
    REQUIRE(samasa.valid());
    REQUIRE(lexy.commands.size() == samasa.commands.size());
    for (std::size_t i = 0; i < lexy.commands.size(); ++i)
        REQUIRE(lexy.nodes[lexy.commands[i]].kind == samasa.nodes[samasa.commands[i]].kind);
}

TEST_CASE (
"tarka native: Array Extensionality Skolemization"
,
"[tarka][native][array][ext]"
)
 {
    Context ctx;
    auto bv32 = ctx.bv_sort(32);
    auto arr_sort = ctx.array_sort(bv32, bv32);

    auto a = ctx.make_symbol("a", arr_sort);
    auto b = ctx.make_symbol("b", arr_sort);
    auto i = ctx.make_symbol("i", bv32);
    auto v = ctx.make_symbol("v", bv32);

    RouterEngine<backend::native> solver;

    SECTION("Extensionality: a != b implies not identical on all reads") {
        Term store_a = ctx.make_term(Op::Store, arr_sort, std::vector<Term>{a, i, v});
        Term store_b = ctx.make_term(Op::Store, arr_sort, std::vector<Term>{b, i, v});
        Term deq_arr = (a != b);

        solver.assert_formula(deq_arr);
        auto res = solver.check_sat();
        REQUIRE(res.has_value());
        REQUIRE(*res == SatResult::Sat);
    }
}

TEST_CASE (
"tarka native: Quantifier Instantiation (E-matching & Skolem)"
,
"[tarka][native][quant]"
)
 {
    Context ctx;
    auto bv32 = ctx.bv_sort(32);
    auto bool_sort = ctx.bool_sort();

    auto x = ctx.make_symbol("x", bv32);
    auto y = ctx.make_symbol("y", bv32);
    auto c10 = ctx.make_value(10, bv32);
    auto c20 = ctx.make_value(20, bv32);

    RouterEngine<backend::native> solver;

    SECTION("Universal quantifier: (forall x. x == 10) && (y == 20) is UNSAT") {
        Term eq_x10 = (x == c10);
        Term forall_t = ctx.make_term(Op::Forall, bool_sort, std::vector<Term>{x, eq_x10});

        Term eq_y20 = (y == c20);
        Term assertion = forall_t && eq_y20;

        solver.assert_formula(assertion);
        auto res = solver.check_sat();
        REQUIRE(res.has_value());
        REQUIRE(*res == SatResult::Unsat);
    }
}

TEST_CASE (
"tarka frontend: SMT-LIB2 Serializer (smt2_printer)"
,
"[tarka][frontend][smt2][printer]"
)
 {
    Context ctx;
    auto bv32 = ctx.bv_sort(32);
    auto a = ctx.make_symbol("a", bv32);
    auto b = ctx.make_symbol("b", bv32);
    auto c10 = ctx.make_value(10, bv32);

    Term sum = ctx.make_term(Op::BvAdd, bv32, std::vector<Term>{a, b});
    Term eq = (sum == c10);

    std::string term_str = smt2_printer::to_string(eq);
    REQUIRE(!term_str.empty());
    REQUIRE(term_str.find("bvadd") != std::string::npos);

    std::string script_str = smt2_printer::to_smt2_script(std::vector<Term>{eq}, "QF_BV");
    REQUIRE(!script_str.empty());
    REQUIRE(script_str.find("(set-logic QF_BV)") != std::string::npos);
    REQUIRE(script_str.find("(declare-const a (_ BitVec 32))") != std::string::npos);
    REQUIRE(script_str.find("(assert") != std::string::npos);
    REQUIRE(script_str.find("(check-sat)") != std::string::npos);
}

TEST_CASE (
"tarka native: Model Formatter & Validator"
,
"[tarka][native][model][validator]"
)
 {
    Context ctx;
    auto bv32 = ctx.bv_sort(32);
    auto x = ctx.make_symbol("x", bv32);
    auto y = ctx.make_symbol("y", bv32);
    auto v5 = ctx.make_value(5, bv32);
    auto v15 = ctx.make_value(15, bv32);

    RouterEngine<backend::native> solver;

    Term eq_x = (x == v5);
    Term add_xy = ctx.make_term(Op::BvAdd, bv32, std::vector<Term>{x, y});
    Term eq_sum = (add_xy == v15);

    solver.assert_formula(eq_x && eq_sum);
    auto res = solver.check_sat();
    REQUIRE(res.has_value());
    REQUIRE(*res == SatResult::Sat);

    auto x_val = solver.get_value(x);
    auto y_val = solver.get_value(y);
    REQUIRE(x_val.has_value());
    REQUIRE(y_val.has_value());

    std::unordered_map<Term, SmtValue> model;
    model[x] = *x_val;
    model[y] = *y_val;

    std::string formatted_model = model_validator::format_model(model);
    REQUIRE(!formatted_model.empty());
    REQUIRE(formatted_model.find("define-fun x") != std::string::npos);

    auto valid_res = model_validator::validate(std::vector<Term>{eq_x, eq_sum}, model);
    REQUIRE(valid_res.is_valid);
    REQUIRE(valid_res.violated_assertions.empty());
}

#if defined(HAS_Z3) && (HAS_Z3 != 0) && (__has_include(<z3++.h>) || __has_include("z3++.h"))
TEST_CASE ("tarka differential: Native Backend vs Z3 Backend", "[tarka][differential][z3]") {
    Context ctx;

    SECTION("Propositional logic equivalence (SAT and UNSAT)") {
        auto bool_s = ctx.bool_sort();
        auto p = ctx.make_symbol("p", bool_s);
        auto q = ctx.make_symbol("q", bool_s);
        auto r = ctx.make_symbol("r", bool_s);

        // SAT formula: (p || q) && (!p || r) && (!q || r) && r
        Term f_sat = (p || q) && ((!p) || r) && ((!q) || r) && r;

        RouterEngine<backend::native> native_solver;
        RouterEngine<backend::z3_backend> z3_solver;

        native_solver.assert_formula(f_sat);
        z3_solver.assert_formula(f_sat);

        auto native_res1 = native_solver.check_sat();
        auto z3_res1 = z3_solver.check_sat();

        REQUIRE(native_res1.has_value());
        REQUIRE(z3_res1.has_value());
        CHECK(*native_res1 == SatResult::Sat);
        CHECK(*z3_res1 == SatResult::Sat);

        // UNSAT formula: f_sat && !r
        Term f_unsat = f_sat && (!r);
        RouterEngine<backend::native> native_unsat;
        RouterEngine<backend::z3_backend> z3_unsat;

        native_unsat.assert_formula(f_unsat);
        z3_unsat.assert_formula(f_unsat);

        auto native_res2 = native_unsat.check_sat();
        auto z3_res2 = z3_unsat.check_sat();

        REQUIRE(native_res2.has_value());
        REQUIRE(z3_res2.has_value());
        CHECK(*native_res2 == SatResult::Unsat);
        CHECK(*z3_res2 == SatResult::Unsat);
    }

    SECTION("BitVector arithmetic & bitwise equivalence (QF_BV)") {
        auto bv32 = ctx.bv_sort(32);
        auto x = ctx.make_symbol("x", bv32);
        auto y = ctx.make_symbol("y", bv32);
        auto c10 = ctx.make_value(10, bv32);
        auto c25 = ctx.make_value(25, bv32);

        // SAT: (x ^ y) == 25 && (x + 10) == y
        Term xor_xy = ctx.make_term(Op::BvXor, bv32, {x, y});
        Term add_x10 = ctx.make_term(Op::BvAdd, bv32, {x, c10});
        Term f_bv = (xor_xy == c25) && (add_x10 == y);

        RouterEngine<backend::native> native_solver;
        RouterEngine<backend::z3_backend> z3_solver;

        native_solver.assert_formula(f_bv);
        z3_solver.assert_formula(f_bv);

        auto n_res = native_solver.check_sat();
        auto z_res = z3_solver.check_sat();

        REQUIRE(n_res.has_value());
        REQUIRE(z_res.has_value());
        CHECK(*n_res == *z_res);

        if (*n_res == SatResult::Sat) {
            auto n_x = native_solver.get_value(x);
            auto n_y = native_solver.get_value(y);
            REQUIRE(n_x.has_value());
            REQUIRE(n_y.has_value());

            std::unordered_map<Term, SmtValue> model;
            model[x] = *n_x;
            model[y] = *n_y;
            auto valid = model_validator::validate(std::vector<Term>{f_bv}, model);
            CHECK(valid.is_valid);
        }
    }

    SECTION("BitVector division-by-zero semantics comparison") {
        auto bv32 = ctx.bv_sort(32);
        auto a = ctx.make_symbol("a", bv32);
        auto c0 = ctx.make_value(0, bv32);
        auto all_ones = ctx.make_value(0xFFFFFFFFU, bv32);

        // SMT-LIB division-by-zero standard: (a / 0) == 0xFFFFFFFF
        Term udiv_0 = ctx.make_term(Op::BvUdiv, bv32, {a, c0});
        Term eq_all_ones = (udiv_0 == all_ones);

        RouterEngine<backend::native> native_solver;
        RouterEngine<backend::z3_backend> z3_solver;

        native_solver.assert_formula(eq_all_ones);
        z3_solver.assert_formula(eq_all_ones);

        auto n_res = native_solver.check_sat();
        auto z_res = z3_solver.check_sat();

        REQUIRE(n_res.has_value());
        REQUIRE(z_res.has_value());
        CHECK(*n_res == SatResult::Sat);
        CHECK(*z_res == SatResult::Sat);
    }

    SECTION("Array read-over-write and extensionality comparison (QF_AX)") {
        auto bv32 = ctx.bv_sort(32);
        auto arr_s = ctx.array_sort(bv32, bv32);

        auto arr = ctx.make_symbol("arr", arr_s);
        auto i = ctx.make_symbol("i", bv32);
        auto j = ctx.make_symbol("j", bv32);
        auto val = ctx.make_symbol("val", bv32);

        Term stored = ctx.make_term(Op::Store, arr_s, {arr, i, val});
        Term read_j = ctx.make_term(Op::Select, bv32, {stored, j});
        Term orig_j = ctx.make_term(Op::Select, bv32, {arr, j});

        // Bug search: i != j && read_j != orig_j -> must be UNSAT in both solvers
        Term formula = (i != j) && (read_j != orig_j);

        RouterEngine<backend::native> native_solver;
        RouterEngine<backend::z3_backend> z3_solver;

        native_solver.assert_formula(formula);
        z3_solver.assert_formula(formula);

        auto n_res = native_solver.check_sat();
        auto z_res = z3_solver.check_sat();

        REQUIRE(n_res.has_value());
        REQUIRE(z_res.has_value());
        CHECK(*n_res == SatResult::Unsat);
        CHECK(*z_res == SatResult::Unsat);
    }

    SECTION("Linear Real Arithmetic comparison (QF_LRA)") {
        auto real_s = ctx.real_sort();
        auto x = ctx.make_symbol("x", real_s);
        auto y = ctx.make_symbol("y", real_s);

        auto c10 = ctx.make_real(10, 1, real_s);
        auto c5  = ctx.make_real(5, 1, real_s);
        auto c100 = ctx.make_real(100, 1, real_s);

        // x + y <= 10 && x >= 5 && y >= 5
        Term sum_xy = ctx.make_term(Op::Add, real_s, {x, y});
        Term f_lra = (sum_xy <= c10) && (x >= c5) && (y >= c5);

        RouterEngine<backend::native> native_solver;
        RouterEngine<backend::z3_backend> z3_solver;

        native_solver.assert_formula(f_lra);
        z3_solver.assert_formula(f_lra);

        auto n_res = native_solver.check_sat();
        auto z_res = z3_solver.check_sat();

        REQUIRE(n_res.has_value());
        REQUIRE(z_res.has_value());
        CHECK(*n_res == SatResult::Sat);
        CHECK(*z_res == SatResult::Sat);
    }
}
#endif

// =============================================================================
// Appended coverage for the design upgrades (items 1,2,9,14,18,24,28).
// These tests only exercise the always-on native path — no Z3, no opt-in flags.
// =============================================================================

TEST_CASE (
"tarka: leaf ops carry a non-core theory mask"
,
"[tarka][mask]"
)
 {
    // items 1,2 — Lit/Sym no longer fall to the op_descriptor default (core-only).
    Context ctx;
    auto i_sort = ctx.int_sort();
    Term x = ctx.make_symbol("x", i_sort);
    Term k = ctx.make_int(7, i_sort);

    const theory_mask sym_bits = get_op_info(x.op()).theory_bits;
    const theory_mask lit_bits = get_op_info(k.op()).theory_bits;

    // Both must at least include the arithmetic families they can appear in.
    CHECK((sym_bits & theory_bit(theory_family::lia)) != 0);
    CHECK((lit_bits & theory_bit(theory_family::lia)) != 0);

    // A whole arithmetic atom's mask must fold in the LIA family.
    Term atom = (x < k);
    const theory_mask m = compute_theory_mask(atom);
    CHECK((m & theory_bit(theory_family::lia)) != 0);
}

TEST_CASE (
"tarka: RouterEngine reports the active backend"
,
"[tarka][router]"
)
 {
    // item 9 — active_index() reflects capability selection, not a hardcoded 0.
    Context ctx;
    auto b = ctx.bool_sort();
    Term p = ctx.make_symbol("p", b);
    Term q = ctx.make_symbol("q", b);

    RouterEngine<backend::native> solver; // single backend → always index 0
    auto r = solver.solve(p || q);
    REQUIRE(r.has_value());
    CHECK(*r == SatResult::Sat);
    CHECK(solver.active_index() == 0);
}

TEST_CASE (
"tarka: is_conclusive classifies results"
,
"[tarka][result]"
)
 {
    // item 3
    CHECK(is_conclusive(SatResult::Sat));
    CHECK(is_conclusive(SatResult::Unsat));
    CHECK_FALSE(is_conclusive(SatResult::Unknown));
}

TEST_CASE (
"tarka: order_heap pops in activity order"
,
"[tarka][order_heap]"
)
 {
    // item 28 — generic decision heap used by CDCL decide().
    using namespace containers::associative;
    std::vector<double> act{0.1, 0.9, 0.5, 0.3, 0.7};
    struct cmp_t {
        const std::vector<double>* a;
        bool operator()(std::uint32_t x, std::uint32_t y) const { return (*a)[x] > (*a)[y]; }
    };
    order_heap<cmp_t> h{cmp_t{&act}};
    h.reserve_universe(act.size());
    for (std::uint32_t i = 0; i < act.size(); ++i) h.insert(i);

    REQUIRE(h.size() == act.size());
    CHECK(h.remove_max() == 1); // 0.9
    CHECK(h.remove_max() == 4); // 0.7
    // raise element 3's activity above the rest, then it must come out next
    act[3] = 2.0;
    h.increase(3);
    CHECK(h.remove_max() == 3); // 2.0
    CHECK(h.remove_max() == 2); // 0.5
    CHECK(h.remove_max() == 0); // 0.1
    CHECK(h.empty());
}

TEST_CASE (
"tarka: solver handles trees deeper than 64 levels (LBD)"
,
"[tarka][cdcl][lbd]"
)
 {
    // item 18 — LBD no longer capped at 64 decision levels. A long implication
    // chain forces a deep trail; the solver must still terminate correctly.
    Context ctx;
    auto b = ctx.bool_sort();
    constexpr int N = 200;
    std::vector<Term> v;
    v.reserve(N);
    for (int i = 0; i < N; ++i) v.push_back(ctx.make_symbol("v" + std::to_string(i), b));

    // v0 && (v0 -> v1) && (v1 -> v2) && ... forces v_i all true across many levels.
    Term f = v[0];
    for (int i = 0; i + 1 < N; ++i) f = f && (v[i].implies(v[i + 1]));

    RouterEngine<backend::native> solver;
    auto r = solver.solve(f);
    REQUIRE(r.has_value());
    CHECK(*r == SatResult::Sat);
}

TEST_CASE (
"tarka: simplifier folds integer literals"
,
"[tarka][simplify]"
)
 {
    // item 24 — arithmetic / relational const folding.
    Context ctx;
    auto i_sort = ctx.int_sort();
    Term a = ctx.make_int(3, i_sort);
    Term b = ctx.make_int(4, i_sort);

    Term sum = ctx.make_term(Op::Add, i_sort, std::vector<Term>{a, b});
    Term folded = tarka::native::simplifier::simplify(sum);
    auto fv = ctx.int_literal(folded.ptr()->payload_hash);
    REQUIRE(fv.has_value());
    CHECK(*fv == 7);

    Term lt = (a < b);
    Term lt_folded = tarka::native::simplifier::simplify(lt);
    CHECK(lt_folded.op() == Op::True);

    Term ge = (a >= b);
    Term ge_folded = tarka::native::simplifier::simplify(ge);
    CHECK(ge_folded.op() == Op::False);
}

TEST_CASE (
"tarka: egraph DAG node count dedups shared subterms"
,
"[tarka][egraph]"
)
 {
    // item 14 — egraph_node_count_dag counts distinct hash-consed nodes, so a
    // shared subterm is counted once (the old tree count inflated it).
    Context ctx;
    auto i_sort = ctx.int_sort();
    Term x = ctx.make_symbol("x", i_sort);
    Term shared = ctx.make_term(Op::Add, i_sort, std::vector<Term>{x, x});
    // (shared + shared): both operands are the *same* interned node.
    Term t = ctx.make_term(Op::Add, i_sort, std::vector<Term>{shared, shared});

    // Distinct nodes: t, shared, x  → 3 (not the 5 a tree walk would report).
    CHECK(egraph_node_count_dag(t) == 3);
    // Result stays valid after optimize (may or may not shrink; must not crash).
    Term opt = egraph_optimize(t);
    CHECK(opt.valid());
}

// =============================================================================
// Corner-case coverage for the same always-on native upgrades. Still no Z3 and
// no opt-in flags — only paths that ship by default.
// =============================================================================

TEST_CASE (
"tarka: order_heap corner cases"
,
"[tarka][order_heap][corner]"
)
 {
    using namespace containers::associative;
    std::vector<double> act{0.1, 0.9, 0.5, 0.3, 0.7};
    struct cmp_t {
        const std::vector<double>* a;
        bool operator()(std::uint32_t x, std::uint32_t y) const { return (*a)[x] > (*a)[y]; }
    };

    SECTION("duplicate insert is a no-op; contains tracks membership") {
        order_heap<cmp_t> h{cmp_t{&act}};
        h.reserve_universe(act.size());
        h.insert(2);
        h.insert(2); // duplicate — must not double-add
        CHECK(h.size() == 1);
        CHECK(h.contains(2));
        CHECK_FALSE(h.contains(0));
        CHECK(h.remove_max() == 2);
        CHECK_FALSE(h.contains(2)); // popped → no longer a member
        CHECK(h.empty());
    }

    SECTION("single element pops itself and empties") {
        order_heap<cmp_t> h{cmp_t{&act}};
        h.insert(3);
        REQUIRE(h.size() == 1);
        CHECK(h.remove_max() == 3);
        CHECK(h.empty());
    }

    SECTION("decrease-key demotes an element toward the leaves") {
        order_heap<cmp_t> h{cmp_t{&act}};
        for (std::uint32_t i = 0; i < act.size(); ++i) h.insert(i);
        // Element 1 is the current max (0.9). Drop it below everyone.
        act[1] = 0.0;
        h.decrease(1);
        CHECK(h.remove_max() == 4); // 0.7 now the largest
        CHECK(h.remove_max() == 2); // 0.5
    }

    SECTION("clear empties, then the heap is reusable") {
        order_heap<cmp_t> h{cmp_t{&act}};
        for (std::uint32_t i = 0; i < act.size(); ++i) h.insert(i);
        h.clear();
        CHECK(h.empty());
        CHECK_FALSE(h.contains(1));
        h.insert(0);
        h.insert(4);
        CHECK(h.remove_max() == 4); // 0.7 > 0.1
        CHECK(h.remove_max() == 0);
    }

    SECTION("increase/decrease on an absent element is a no-op, not a crash") {
        order_heap<cmp_t> h{cmp_t{&act}};
        h.reserve_universe(act.size());
        h.increase(2); // 2 not inserted
        h.decrease(2);
        CHECK(h.empty());
    }
}

TEST_CASE (
"tarka: simplifier fold corner cases"
,
"[tarka][simplify][corner]"
)
 {
    Context ctx;
    auto i_sort = ctx.int_sort();
    auto two = ctx.make_int(2, i_sort);
    auto three = ctx.make_int(3, i_sort);
    auto five = ctx.make_int(5, i_sort);

    SECTION("multiplication and subtraction fold") {
        Term prod = ctx.make_term(Op::Mul, i_sort, std::vector<Term>{two, three});
        auto pv = ctx.int_literal(tarka::native::simplifier::simplify(prod).ptr()->payload_hash);
        REQUIRE(pv.has_value());
        CHECK(*pv == 6);

        Term diff = ctx.make_term(Op::Sub, i_sort, std::vector<Term>{five, three});
        auto dv = ctx.int_literal(tarka::native::simplifier::simplify(diff).ptr()->payload_hash);
        REQUIRE(dv.has_value());
        CHECK(*dv == 2);
    }

    SECTION("unary negate folds") {
        Term neg = ctx.make_term(Op::Neg, i_sort, std::vector<Term>{five});
        auto nv = ctx.int_literal(tarka::native::simplifier::simplify(neg).ptr()->payload_hash);
        REQUIRE(nv.has_value());
        CHECK(*nv == -5);
    }

    SECTION("nested arithmetic folds bottom-up") {
        // (2 + 3) * 5 == 25
        Term inner = ctx.make_term(Op::Add, i_sort, std::vector<Term>{two, three});
        Term outer = ctx.make_term(Op::Mul, i_sort, std::vector<Term>{inner, five});
        auto ov = ctx.int_literal(tarka::native::simplifier::simplify(outer).ptr()->payload_hash);
        REQUIRE(ov.has_value());
        CHECK(*ov == 25);
    }

    SECTION("a symbolic operand blocks the fold") {
        Term x = ctx.make_symbol("x", i_sort);
        Term mixed = ctx.make_term(Op::Add, i_sort, std::vector<Term>{x, three});
        Term s = tarka::native::simplifier::simplify(mixed);
        CHECK(s.op() == Op::Add);                  // stays an Add
        CHECK_FALSE(ctx.int_literal(s.ptr()->payload_hash).has_value());
    }

    SECTION("boolean identities simplify") {
        auto b = ctx.bool_sort();
        Term p = ctx.make_symbol("p", b);
        Term t = ctx.make_bool(true);
        Term f = ctx.make_bool(false);

        // p && true -> p ; p || false -> p
        Term and_true = ctx.make_term(Op::And, b, std::vector<Term>{p, t});
        Term or_false = ctx.make_term(Op::Or, b, std::vector<Term>{p, f});
        CHECK(tarka::native::simplifier::simplify(and_true).ptr() == p.ptr());
        CHECK(tarka::native::simplifier::simplify(or_false).ptr() == p.ptr());

        // p && false -> false ; p || true -> true
        Term and_false = ctx.make_term(Op::And, b, std::vector<Term>{p, f});
        Term or_true = ctx.make_term(Op::Or, b, std::vector<Term>{p, t});
        CHECK(tarka::native::simplifier::simplify(and_false).op() == Op::False);
        CHECK(tarka::native::simplifier::simplify(or_true).op() == Op::True);

        // double negation: !!p -> p
        Term nnp = ctx.make_term(Op::Not, b,
                                 std::vector<Term>{ctx.make_term(Op::Not, b, std::vector<Term>{p})});
        CHECK(tarka::native::simplifier::simplify(nnp).ptr() == p.ptr());
    }

    SECTION("structural equalities fold on identical operands") {
        Term x = ctx.make_symbol("y", i_sort);
        Term eq_same = ctx.make_term(Op::Eq, ctx.bool_sort(), std::vector<Term>{x, x});
        Term dist_same = ctx.make_term(Op::Distinct, ctx.bool_sort(), std::vector<Term>{x, x});
        CHECK(tarka::native::simplifier::simplify(eq_same).op() == Op::True);
        CHECK(tarka::native::simplifier::simplify(dist_same).op() == Op::False);
    }
}

TEST_CASE (
"tarka: egraph DAG node count corner cases"
,
"[tarka][egraph][corner]"
)
 {
    Context ctx;
    auto i_sort = ctx.int_sort();

    SECTION("a bare leaf counts as one node") {
        Term x = ctx.make_symbol("leaf", i_sort);
        CHECK(egraph_node_count_dag(x) == 1);
    }

    SECTION("a diamond shares its apex once") {
        // g = x + x (node: g, x)         -> 2
        // top = (g + x)                  -> adds top; g and x already counted -> 3
        Term x = ctx.make_symbol("d", i_sort);
        Term g = ctx.make_term(Op::Add, i_sort, std::vector<Term>{x, x});
        Term top = ctx.make_term(Op::Add, i_sort, std::vector<Term>{g, x});
        CHECK(egraph_node_count_dag(top) == 3);
    }

    SECTION("distinct leaves are all counted") {
        Term a = ctx.make_symbol("a1", i_sort);
        Term b = ctx.make_symbol("b1", i_sort);
        Term sum = ctx.make_term(Op::Add, i_sort, std::vector<Term>{a, b});
        CHECK(egraph_node_count_dag(sum) == 3); // sum, a, b
    }
}

TEST_CASE (
"tarka: CDCL solves adversarial boolean corner cases"
,
"[tarka][cdcl][corner]"
)
 {
    Context ctx;
    auto b = ctx.bool_sort();

    SECTION("direct contradiction p && !p is UNSAT") {
        Term p = ctx.make_symbol("p", b);
        Term f = p && ctx.make_term(Op::Not, b, std::vector<Term>{p});
        RouterEngine<backend::native> solver;
        auto r = solver.solve(f);
        REQUIRE(r.has_value());
        CHECK(*r == SatResult::Unsat);
    }

    SECTION("a satisfiable XOR is SAT") {
        Term p = ctx.make_symbol("xp", b);
        Term q = ctx.make_symbol("xq", b);
        Term f = ctx.make_term(Op::Xor, b, std::vector<Term>{p, q});
        RouterEngine<backend::native> solver;
        auto r = solver.solve(f);
        REQUIRE(r.has_value());
        CHECK(*r == SatResult::Sat);
    }

    SECTION("implication chain forcing the head false then asserting it is UNSAT") {
        // v0 && (v0 -> v1) && ... && (v_{n-1} -> v_n) && !v_n  is UNSAT.
        constexpr int N = 120; // deep enough to exceed the old 64-level LBD cap
        std::vector<Term> v;
        v.reserve(N);
        for (int i = 0; i < N; ++i) v.push_back(ctx.make_symbol("c" + std::to_string(i), b));
        Term f = v[0];
        for (int i = 0; i + 1 < N; ++i) f = f && (v[i].implies(v[i + 1]));
        f = f && ctx.make_term(Op::Not, b, std::vector<Term>{v[N - 1]});

        RouterEngine<backend::native> solver;
        auto r = solver.solve(f);
        REQUIRE(r.has_value());
        CHECK(*r == SatResult::Unsat);
    }

    SECTION("a constant-true formula is SAT and a constant-false is UNSAT") {
        RouterEngine<backend::native> sat_solver;
        auto rs = sat_solver.solve(ctx.make_bool(true));
        REQUIRE(rs.has_value());
        CHECK(*rs == SatResult::Sat);

        RouterEngine<backend::native> unsat_solver;
        auto ru = unsat_solver.solve(ctx.make_bool(false));
        REQUIRE(ru.has_value());
        CHECK(*ru == SatResult::Unsat);
    }
}

// =============================================================================
// Phase 0 — interning permanence + collision-safe identity.
//
// The intern tables became permanent, non-evicting FlatHashStorage with an
// intrusive per-hash collision chain; every hit does a full structural verify.
// symbol interning walks a rehash chain with name compare; each interned node
// carries a dense monotonic node_id. These fixtures exercise the public-API
// consequences: stable CSE under churn, faithful symbol names, and no router
// mis-route now that the feature store keys on interned identity.
// =============================================================================

TEST_CASE (
"tarka: interning is permanent — CSE stable under heavy churn"
,
"[tarka][term][intern]"
)
 {
    Context ctx;
    auto i_sort = ctx.int_sort();

    // Intern a landmark term, then churn thousands of unrelated terms. With an
    // evicting cache the landmark could be dropped and re-interned at a fresh
    // address; a permanent table keeps one immutable identity forever.
    Term x = ctx.make_symbol("landmark_x", i_sort);
    Term y = ctx.make_symbol("landmark_y", i_sort);
    Term landmark = ctx.make_term(Op::Add, i_sort, std::vector<Term>{x, y});
    const TermImpl* landmark_id = landmark.ptr();

    for (int i = 0; i < 8192; ++i) {
        Term ci = ctx.make_int(i, i_sort);
        Term v = ctx.make_symbol("churn_" + std::to_string(i), i_sort);
        (void)ctx.make_term(Op::Add, i_sort, std::vector<Term>{v, ci});
    }

    // Re-deriving the identical structure must hash-cons back to the same node.
    Term x2 = ctx.make_symbol("landmark_x", i_sort);
    Term y2 = ctx.make_symbol("landmark_y", i_sort);
    Term again = ctx.make_term(Op::Add, i_sort, std::vector<Term>{x2, y2});
    CHECK(again.ptr() == landmark_id);
    CHECK(x2.ptr() == x.ptr());
}

TEST_CASE (
"tarka: distinct interned nodes get distinct dense node_ids"
,
"[tarka][term][intern]"
)
 {
    Context ctx;
    auto i_sort = ctx.int_sort();
    Term a = ctx.make_symbol("nid_a", i_sort);
    Term b = ctx.make_symbol("nid_b", i_sort);
    Term sum = ctx.make_term(Op::Add, i_sort, std::vector<Term>{a, b});

    CHECK(a.ptr()->node_id != 0u);
    CHECK(a.ptr()->node_id != b.ptr()->node_id);
    CHECK(sum.ptr()->node_id != a.ptr()->node_id);
    CHECK(sum.ptr()->node_id != b.ptr()->node_id);

    // Re-interning the same structure reuses the node — and thus its id.
    Term a_again = ctx.make_symbol("nid_a", i_sort);
    CHECK(a_again.ptr()->node_id == a.ptr()->node_id);
}

TEST_CASE (
"tarka: symbol names stay faithful across many variables"
,
"[tarka][term][intern]"
)
 {
    // The symbol interner walks a rehash chain with a name compare, so distinct
    // names never alias even when their base hash collides. We can't force an
    // FNV collision by hand, but we can assert the round-trip contract holds at
    // scale: every distinct name interns to a node whose recovered name matches.
    Context ctx;
    auto i_sort = ctx.int_sort();
    std::vector<Term> syms;
    for (int i = 0; i < 4096; ++i)
        syms.push_back(ctx.make_symbol("var_" + std::to_string(i), i_sort));

    for (int i = 0; i < 4096; ++i) {
        const std::uint64_t ph = syms[static_cast<std::size_t>(i)].ptr()->payload_hash;
        CHECK(ctx.symbol_name(ph) == ("var_" + std::to_string(i)));
    }
    // Same name re-interns to the same payload key (same node).
    Term dup = ctx.make_symbol("var_100", i_sort);
    CHECK(dup.ptr() == syms[100].ptr());
}

TEST_CASE (
"tarka: router keys on interned identity — no cross-formula mis-route"
,
"[tarka][native][router]"
)
 {
    // Two structurally different formulas must route independently. A hash-only
    // feature cache could alias them on a collision; the ptr-keyed store cannot.
    // Both are solvable by the native backend; the point is that routing each
    // yields a correct, independent decision.
    Context ctx;
    auto i_sort = ctx.int_sort();
    Term x = ctx.make_symbol("rx", i_sort);
    Term y = ctx.make_symbol("ry", i_sort);

    // Linear arithmetic: x + 0 <= y  (satisfiable).
    Term zero = ctx.make_int(0, i_sort);
    Term lin = (ctx.make_term(Op::Add, i_sort, std::vector<Term>{x, zero}) <= y);

    // A different linear formula over the same vars: x >= y + 1 (satisfiable).
    Term one = ctx.make_int(1, i_sort);
    Term other = (x >= ctx.make_term(Op::Add, i_sort, std::vector<Term>{y, one}));

    RouterEngine<backend::native> s1;
    auto r1 = s1.solve(lin);
    REQUIRE(r1.has_value());
    CHECK(*r1 == SatResult::Sat);

    RouterEngine<backend::native> s2;
    auto r2 = s2.solve(other);
    REQUIRE(r2.has_value());
    CHECK(*r2 == SatResult::Sat);
}





