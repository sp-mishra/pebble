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

#include <cstdint>
#include <expected>
#include <functional>
#include <stop_token>
#include <vector>

namespace tarka::backend {
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
    using tarka::native::theory_lra;
    using tarka::native::theory_uf;

    class native {
    public:
        using native_term_t = Term;
        using native_sort_t = Sort;

        using combination_t = theory_combination<
            theory_uf,
            theory_bv,
            theory_lra,
            theory_dl,
            theory_array
        >;

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

        void assert_formula(Term t) { assertions_.push_back(t); }

        [[nodiscard]] std::expected<SatResult, SmtError> check_sat() {
            return run(std::function<bool()>{});
        }

        [[nodiscard]] std::expected<SatResult, SmtError>
        check_sat_cancelable(Term t, std::stop_token tok) {
            if (t.valid()) assert_formula(t);
            return run([tok] { return tok.stop_requested(); });
        }

        [[nodiscard]] std::expected<SmtValue, SmtError> get_value(Term t) {
            if (!solved_) {
                return std::unexpected(SmtError{SmtError::Kind::Internal, "no model: call check_sat first"});
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
        // Guard rejects constructs beyond quantifier-free supported theories
        [[nodiscard]] static std::expected<void, SmtError> guard(Term t) {
            switch (t.op()) {
                case Op::Forall:
                case Op::Exists:
                    return std::unexpected(SmtError{SmtError::Kind::Unsupported, "native: quantifiers unsupported"});
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

        [[nodiscard]] std::expected<SatResult, SmtError> run(const std::function<bool()>& stop) {
            for (Term t : assertions_) {
                if (auto g = guard(t); !g) return std::unexpected(g.error());
            }

            sat_.reset();
            reg_.reset();
            theories_.reset();

            cnf_encoder enc(sat_, reg_);
            dpllt<combination_t> driver(sat_, reg_, theories_);

            for (Term t : assertions_) enc.assert_formula(t);
            driver.register_all_atoms();

            const LBool r = driver.solve(stop);
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
