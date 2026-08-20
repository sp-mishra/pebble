#pragma once
// =============================================================================
// tarka/backends/native_backend.hpp — header-only z3-free SMT backend
//
// Namespace:  tarka::backend
// Provides:   native — models SmtSolverBackend + CancelableBackend, built
//             entirely on internal Pebble algorithms and data structures
//             (no Z3, zero external dependencies).
//
// Supported Theory Fragments:
//   - Propositional SAT (CDCL with 2-watched literals, 1UIP, VSIDS, Luby restarts)
//   - QF_UF (Congruence Closure over uninterpreted functions and equalities)
//   - QF_BV (Bit-Vectors via complete Bit-Blasting and Highway SIMD acceleration)
//   - QF_LRA & QF_LIA (Linear Real & Integer Arithmetic via Simplex & Difference Logic)
//   - QF_AX (Arrays with read-over-write axioms and extensionality)
//   - Nelson-Oppen multi-theory combination
//   - Full model extraction for Bool, BitVec, Int, Real, and symbols
//
// Design:
//   - No virtual, no macros. Header-only, C++23/C++26.
//   - Opt-in: include explicitly and wire via RouterEngine<backend::native>.
//   - Identity lowering: consumes tarka interned IR directly.
// =============================================================================

#include "tarka/backend.hpp"
#include "tarka/context.hpp"
#include "tarka/native/atom_registry.hpp"
#include "tarka/native/cdcl_solver.hpp"
#include "tarka/native/cnf_encoder.hpp"
#include "tarka/native/dpllt.hpp"
#include "tarka/native/theory_array.hpp"
#include "tarka/native/theory_bv.hpp"
#include "tarka/native/theory_combination.hpp"
#include "tarka/native/theory_dl.hpp"
#include "tarka/native/theory_lra.hpp"
#include "tarka/native/theory_uf.hpp"
#include "tarka/native/model.hpp"
#include "tarka/native/simplifier.hpp"
#include "tarka/native/theory_quant.hpp"

#include <cstdint>
#include <expected>
#include <functional>
#include <stop_token>
#include <vector>

namespace tarka::backend {
    using namespace tarka::native;
    // Bring the native engine's types into scope for this facade.
    using tarka::native::atom_registry;
    using tarka::native::cdcl_solver;
    using tarka::native::cnf_encoder;
    using tarka::native::dpllt;
    using tarka::native::LBool;
    using tarka::native::model_builder;
    using tarka::native::theory_array;
    using tarka::native::theory_bv;
    using tarka::native::theory_combination;
    using tarka::native::theory_dl;
    using tarka::native::theory_quant;
    using namespace tarka::native;

    class native {
    public:
        using combination_t = theory_combination<
            theory_uf,
            theory_bv,
            theory_lra,
            theory_dl,
            theory_array
        >;

        using native_term_t = Term;
        using native_sort_t = Sort;

        native() = default;

        [[nodiscard]] static constexpr theory_mask capabilities() noexcept {
            return theory_bit(theory_family::core) |
                   theory_bit(theory_family::bv) |
                   theory_bit(theory_family::lra) |
                   theory_bit(theory_family::lia) |
                   theory_bit(theory_family::array) |
                   theory_bit(theory_family::uf);
        }

        // Identity lowering — the native engine consumes tarka IR directly.
        [[nodiscard]] native_sort_t lower_sort(Sort s) noexcept { return s; }
        [[nodiscard]] native_term_t lower_term(Term t) noexcept { return t; }

        void assert_formula(Term t) { assertions_.push_back(simplifier::simplify(t)); }

        [[nodiscard]] std::expected<SatResult, SmtError> check_sat() {
            return run({}, [] { return false; });
        }

        [[nodiscard]] std::expected<SatResult, SmtError>
        check_sat_cancelable(Term t, std::stop_token stop) {
            push();
            assert_formula(t);
            auto r = run({}, [&stop] { return stop.stop_requested(); });
            pop();
            return r;
        }

        [[nodiscard]] std::expected<SatResult, SmtError>
        check_sat_assuming(std::span<const Term> assumptions) {
            return run(assumptions, [] { return false; });
        }

        [[nodiscard]] std::vector<Term> get_unsat_core() const {
            auto core_lits = sat_.unsat_core();
            std::vector<Term> core_terms;
            core_terms.reserve(core_lits.size());
            for (Lit l : core_lits) {
                const AtomId a = reg_.atom_of(lit_var(l));
                if (a != kNullAtom) {
                    core_terms.push_back(reg_.atom(a).term);
                }
            }
            return core_terms;
        }

        [[nodiscard]] std::expected<SmtValue, SmtError> get_value(Term t) {
            if (!solved_) {
                return std::unexpected(SmtError{SmtError::Kind::Internal, "get_value called before check_sat returned Sat"});
            }
            model_builder mb(sat_, reg_);
            return mb.eval(t, theories_);
        }

        void push(std::uint32_t n = 1) {
            for (std::uint32_t i = 0; i < n; ++i) scopes_.push_back(assertions_.size());
        }

        void pop(std::uint32_t n = 1) {
            for (std::uint32_t i = 0; i < n && !scopes_.empty(); ++i) {
                assertions_.resize(scopes_.back());
                scopes_.pop_back();
            }
            solved_ = false;
        }

        void reset() {
            assertions_.clear();
            scopes_.clear();
            solved_ = false;
            sat_.reset();
            reg_.reset();
            theories_.reset();
        }

    private:
        // Guard rejects constructs beyond supported theories
        [[nodiscard]] static std::expected<void, SmtError> guard(Term t) {
            switch (t.op()) {
                case Op::Mul: {
                    // Reject nonlinear symbolic multiplication (constant * symbol is supported)
                    auto ch = t.children();
                    if (ch.size() == 2 && ch[0].op() != Op::Lit && ch[1].op() != Op::Lit &&
                        ch[0].sort().kind() != SortKind::BitVec) {
                        return std::unexpected(SmtError{SmtError::Kind::Unsupported, "native: nonlinear real/int multiplication"});
                    }
                    break;
                }
                default:
                    break;
            }
            for (Term c : t.children()) {
                if (auto g = guard(c); !g) return g;
            }
            return {};
        }

        [[nodiscard]] std::expected<SatResult, SmtError> run(std::span<const Term> assumptions,
                                                             const std::function<bool()>& stop) {
            for (Term t : assertions_) {
                if (auto g = guard(t); !g) return std::unexpected(g.error());
            }
            for (Term a : assumptions) {
                if (auto g = guard(a); !g) return std::unexpected(g.error());
            }

            sat_.reset();
            reg_.reset();
            theories_.reset();

            cnf_encoder enc(sat_, reg_);
            dpllt<combination_t> driver(sat_, reg_, theories_);

            theory_quant quant;
            quant.attach(reg_);
            quant.attach_sat(sat_);

            for (Term t : assertions_) {
                enc.assert_formula(t);
                quant.register_term(t);
            }
            for (Term a : assumptions) {
                quant.register_term(a);
            }
            quant.instantiate_all(enc);

            std::vector<Lit> assumption_lits;
            assumption_lits.reserve(assumptions.size());
            for (Term a : assumptions) {
                Term sa = simplifier::simplify(a);
                AtomId aid = reg_.intern(sa, cnf_encoder::classify(sa));
                sat_.ensure_var(reg_.var_of(aid));
                assumption_lits.push_back(make_lit(reg_.var_of(aid), false));
            }

            driver.register_all_atoms();

            LBool r;
            if (assumption_lits.empty()) {
                r = driver.solve(stop);
            } else {
                r = sat_.solve_assuming(assumption_lits, stop);
            }
            solved_ = (r == LBool::True);

            switch (r) {
                case LBool::True: return SatResult::Sat;
                case LBool::False: return SatResult::Unsat;
                default: return SatResult::Unknown;
            }
        }

        cdcl_solver sat_;
        atom_registry reg_;
        combination_t theories_;
        std::vector<Term> assertions_;
        std::vector<std::size_t> scopes_;
        bool solved_ = false;
    };

    static_assert(SmtSolverBackend<native>);
    static_assert(CancelableBackend<native>);
} // namespace tarka::backend
