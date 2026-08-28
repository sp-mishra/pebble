#pragma once
// =============================================================================
// tarka/native/dpllt.hpp — DPLL(T) driver
//
// Namespace:  tarka::native
// Provides:   dpllt<Combination> — glues the CDCL Boolean core (cdcl_solver) to
//             a theory_combination, implementing the lazy DPLL(T) loop.
//
// Loop (theory hook fires at every BCP fixpoint):
//   1. cdcl_solver runs 2-watched BCP; on a Boolean conflict it does 1UIP
//      analysis + non-chronological backjump entirely on its own.
//   2. At a Boolean fixpoint the solver calls our theory hook. We have already
//      mirrored every literal assignment into the theories via the assign hook
//      (assert_lit), tracking decision levels through push_level/pop_level.
//   3. theories.check() runs the Nelson-Oppen fixpoint:
//        - Conflict  => push the theory explanation as a learnt clause and ask
//                       the SAT core to re-derive (TheoryCheckResult::Conflict).
//        - Propagated => TheoryCheckResult::Propagated (re-run BCP).
//        - Sat        => Consistent; the SAT core proceeds to decide().
//
// Design:
//   - No virtual, no macros. The hook captures `this`; theory dispatch is the
//     static tuple fold in theory_combination. Decision-level tracking keeps the
//     theory trail in lock-step with the SAT trail: we compare the SAT decision
//     level seen on each assignment against the theory level and push/pop to
//     match, so backjumps unwind theory state correctly.
// =============================================================================

#include "tarka/native/atom_registry.hpp"
#include "tarka/native/cdcl_solver.hpp"
#include "tarka/native/ids.hpp"
#include "tarka/native/theory_concept.hpp"

#include <cstdint>
#include <functional>

namespace tarka::native {
    template <class Combination>
    class dpllt {
    public:
        dpllt(cdcl_solver& sat, atom_registry& reg, Combination& theories)
            : sat_(sat), reg_(reg), theories_(theories) {
            theories_.attach(reg_);
            if constexpr (requires { theories_.attach_sat(sat_); }) {
                theories_.attach_sat(sat_);
            }
            install_hooks();
        }

        // Register every interned atom with its owning theory. Call after all
        // formulas are encoded (so the registry is fully populated).
        void register_all_atoms() {
            const std::size_t n = reg_.num_atoms();
            for (std::size_t i = 0; i < n; ++i) {
                const AtomId a{static_cast<std::uint32_t>(i)};
                if (reg_.atom(a).term.valid()) theories_.register_atom(a);
            }
        }

        [[nodiscard]] LBool solve(const std::function<bool()>& stop = {}) {
            return sat_.solve(stop);
        }

    private:
        void install_hooks() {
            // Mirror each SAT assignment into the theories, keeping the theory
            // decision-level trail synced to the SAT trail.
            sat_.set_assign_hook([this](Lit l) {
                sync_levels(sat_.decision_level());
                const AtomId a = reg_.atom_of_var(lit_var(l));
                if (a != kNullAtom && reg_.atom(a).term.valid()) {
                    theories_.assert_lit(a, /*value=*/!lit_sign(l));
                }
            });

            // At each BCP fixpoint, run the theory combination.
            sat_.set_theory_hook([this](cdcl_solver& s) -> TheoryCheckResult {
                sync_levels(s.decision_level());
                const TheoryStatus st = theories_.check();
                if (st == TheoryStatus::Conflict) {
                    explain_buf_.clear();
                    for (const Lit cl : theories_.explanation()) explain_buf_.push_back(cl);
                    s.add_theory_clause(std::span<const Lit>{explain_buf_.data(), explain_buf_.size()});
                    return TheoryCheckResult::Conflict;
                }
                if (st == TheoryStatus::Propagated) return TheoryCheckResult::Propagated;
                return TheoryCheckResult::Consistent;
            });
        }

        // Push or pop the theory trail so it matches the SAT decision level.
        void sync_levels(std::uint32_t sat_level) {
            while (theory_level_ < sat_level) { theories_.push_level(); ++theory_level_; }
            while (theory_level_ > sat_level) { theories_.pop_level(); --theory_level_; }
        }

        cdcl_solver& sat_;
        atom_registry& reg_;
        Combination& theories_;
        std::uint32_t theory_level_ = 0;
        std::vector<Lit> explain_buf_;
    };
} // namespace tarka::native
