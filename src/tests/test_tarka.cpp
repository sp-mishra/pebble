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
#include "tarka/features.hpp"

using namespace tarka;
using namespace tarka::backend;

TEST_CASE("tarka: Term and Sort handle invariants", "[tarka][term]") {
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

TEST_CASE("tarka native: Propositional SAT solving", "[tarka][native][sat]") {
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

TEST_CASE("tarka native: Difference Logic (QF_IDL / QF_RDL)", "[tarka][native][dl]") {
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

TEST_CASE("tarka native: Equality & Uninterpreted Functions (QF_UF)", "[tarka][native][uf]") {
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

TEST_CASE("tarka native: Bit-Vectors (QF_BV)", "[tarka][native][bv]") {
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

TEST_CASE("tarka native: Array Theory (QF_AX)", "[tarka][native][array]") {
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

TEST_CASE("tarka native: Incremental push/pop scoping", "[tarka][native][incremental]") {
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

TEST_CASE("tarka native: BitVector Arithmetic & Bitwise Logic (QF_BV)", "[tarka][native][bv]") {
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

TEST_CASE("tarka native: Multi-Argument EUF & Congruence Closure (QF_UF)", "[tarka][native][uf]") {
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

TEST_CASE("tarka native: Linear Real Arithmetic Bounds & Feasibility (QF_LRA)", "[tarka][native][lra]") {
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

TEST_CASE("tarka native: Array Multi-Store & Transitivity (QF_AX)", "[tarka][native][array]") {
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

TEST_CASE("tarka native: Integrated Theory Combination (QF_AUFBV)", "[tarka][native][combination]") {
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

TEST_CASE("tarka native: BitVector Division & Modulo (QF_BV)", "[tarka][native][bv][div]") {
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

TEST_CASE("tarka native: Assumption-Based Solving & Unsat Core", "[tarka][native][assumptions]") {
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


