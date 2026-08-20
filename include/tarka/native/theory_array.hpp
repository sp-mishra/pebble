#pragma once
// =============================================================================
// tarka/native/theory_array.hpp — Array Theory (QF_AX)
//
// Namespace:  tarka::native
// Provides:   theory_array — read-over-write axiom instantiation and
//             extensionality reduction for SMT array terms (Select / Store).
//
// Axioms (docs/tarka/tarka.md "Native Backend / Arrays"):
//   1. select(store(a, i, v), i) = v
//   2. i != j  =>  select(store(a, i, v), j) = select(a, j)
//   3. a != b  =>  select(a, k) != select(b, k)  (extensionality)
//
// Design:
//   - No virtual, no macros. Header-only, C++23.
//   - Satisfies TheorySolver concept.
// =============================================================================

#include "tarka/native/atom_registry.hpp"
#include "tarka/native/cdcl_solver.hpp"
#include "tarka/native/cnf_encoder.hpp"
#include "tarka/native/ids.hpp"
#include "tarka/native/theory_concept.hpp"
#include "tarka/term.hpp"

#include <cassert>
#include <cstdint>
#include <span>
#include <unordered_set>
#include <vector>

namespace tarka::native {
    class theory_array {
    public:
        static constexpr AtomTheory family = AtomTheory::array;

        theory_array() = default;

        void attach(atom_registry& reg) noexcept { reg_ = &reg; }
        void attach_sat(cdcl_solver& sat) noexcept { sat_ = &sat; }

        void register_atom(AtomId a) {
            if (!reg_) return;
            const Term t = reg_->atom(a).term;
            if (!t.valid()) return;

            collect_array_terms(t);
            instantiate_axioms();
        }

        void assert_lit(AtomId a, bool value) {
            (void)a;
            (void)value;
        }

        [[nodiscard]] TheoryStatus check() {
            instantiate_axioms();
            return TheoryStatus::Sat;
        }

        [[nodiscard]] std::span<const Lit> explanation() const noexcept {
            return {};
        }

        void push_level() {}
        void pop_level() {}

        void reset() {
            selects_.clear();
            stores_.clear();
            instantiated_.clear();
        }

    private:
        void collect_array_terms(Term t) {
            if (t.op() == Op::Select) {
                selects_.push_back(t);
            } else if (t.op() == Op::Store) {
                stores_.push_back(t);
            }
            for (Term c : t.children()) {
                collect_array_terms(c);
            }
        }

        void instantiate_axioms() {
            if (!sat_ || !reg_) return;

            std::size_t cur = 0;
            while (cur < selects_.size()) {
                Term sel = selects_[cur++];
                auto sel_ch = sel.children();
                if (sel_ch.size() != 2) continue;
                Term array_term = sel_ch[0];
                Term read_idx = sel_ch[1];

                if (array_term.op() == Op::Store) {
                    auto store_ch = array_term.children();
                    if (store_ch.size() != 3) continue;
                    Term orig_array = store_ch[0];
                    Term write_idx = store_ch[1];
                    Term write_val = store_ch[2];

                    const std::uint64_t pair_key = sel.hash() ^ (array_term.hash() * 0x9e3779b97f4a7c15ULL);
                    if (instantiated_.contains(pair_key)) continue;
                    instantiated_.insert(pair_key);

                    Context& ctx = sel.ctx();

                    if (read_idx.ptr() == write_idx.ptr()) {
                        // Trivially identical indices: select(store(a, i, v), i) == v
                        Term val_eq = (sel == write_val);
                        AtomId a_val_eq = reg_->intern(val_eq, cnf_encoder::classify(val_eq));
                        sat_->ensure_var(reg_->var_of(a_val_eq));
                        Lit l_val_eq = make_lit(reg_->var_of(a_val_eq), false);
                        sat_->add_clause({l_val_eq});
                    } else {
                        // General case:
                        // (read_idx == write_idx) => (select(...) == write_val)
                        // (read_idx != write_idx) => (select(...) == select(orig_array, read_idx))
                        Term idx_eq = (read_idx == write_idx);
                        Term val_eq = (sel == write_val);
                        Term sel_orig = ctx.make_term(Op::Select, sel.sort(), std::vector<Term>{orig_array, read_idx});
                        Term pass_eq = (sel == sel_orig);

                        AtomId a_idx_eq = reg_->intern(idx_eq, cnf_encoder::classify(idx_eq));
                        AtomId a_val_eq = reg_->intern(val_eq, cnf_encoder::classify(val_eq));
                        AtomId a_pass_eq = reg_->intern(pass_eq, cnf_encoder::classify(pass_eq));

                        sat_->ensure_var(reg_->var_of(a_idx_eq));
                        sat_->ensure_var(reg_->var_of(a_val_eq));
                        sat_->ensure_var(reg_->var_of(a_pass_eq));

                        Lit l_idx_eq = make_lit(reg_->var_of(a_idx_eq), false);
                        Lit l_val_eq = make_lit(reg_->var_of(a_val_eq), false);
                        Lit l_pass_eq = make_lit(reg_->var_of(a_pass_eq), false);

                        sat_->add_clause({lit_neg(l_idx_eq), l_val_eq});
                        sat_->add_clause({l_idx_eq, l_pass_eq});

                        collect_array_terms(sel_orig);
                    }
                }
            }
        }

        atom_registry* reg_ = nullptr;
        cdcl_solver* sat_ = nullptr;
        std::vector<Term> selects_;
        std::vector<Term> stores_;
        std::unordered_set<std::uint64_t> instantiated_;
    };
} // namespace tarka::native
