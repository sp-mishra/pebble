// =============================================================================
// Tarka — Zero-Overhead Multi-Solver SMT Substrate
// include/tarka/native/theory_quant.hpp
//
// Quantifier Instantiation Engine (E-matching & Skolemization).
// Instantiates Op::Forall / Op::Exists over active EUF ground equivalence classes.
// C++23, zero virtual, header-only.
// =============================================================================

#pragma once

#include "tarka/context.hpp"
#include "tarka/native/atom_registry.hpp"
#include "tarka/native/cdcl_solver.hpp"
#include "tarka/native/cnf_encoder.hpp"
#include "tarka/native/ids.hpp"
#include "tarka/native/simplifier.hpp"
#include "tarka/term.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tarka::native {
    class theory_quant {
    public:
        theory_quant() = default;

        void attach(atom_registry& reg) noexcept { reg_ = &reg; }
        void attach_sat(cdcl_solver& sat) noexcept { sat_ = &sat; }

        void register_term(Term t) {
            if (!t.valid()) return;
            collect_quantifiers_and_ground(t);
        }

        void instantiate_all(cnf_encoder& enc) {
            if (!reg_ || !sat_) return;

            // 1. Skolemize all existential quantifiers: (exists x. P(x)) => P(skolem_x)
            for (Term q : exists_terms_) {
                if (instantiated_quants_.contains(q.hash())) continue;
                instantiated_quants_.insert(q.hash());

                auto ch = q.children();
                if (ch.size() < 2) continue;
                Context& ctx = q.ctx();

                std::unordered_map<const TermImpl*, Term> subst;
                for (std::size_t i = 0; i + 1 < ch.size(); ++i) {
                    Term bound_var = ch[i];
                    std::string sk_name = "sk_" + std::to_string(bound_var.hash()) + "_" + std::to_string(q.hash());
                    Term skolem_const = ctx.make_symbol(sk_name, bound_var.sort());
                    subst[bound_var.ptr()] = skolem_const;
                }

                Term body = ch.back();
                Term inst_body = substitute(body, subst, ctx);
                Term impl = (!q) || inst_body;
                enc.assert_formula(simplifier::simplify(impl));
            }

            // 2. E-matching instantiation for universal quantifiers: (forall x. P(x)) => P(ground_t)
            for (Term q : forall_terms_) {
                auto ch = q.children();
                if (ch.size() < 2) continue;
                Context& ctx = q.ctx();

                Term bound_var = ch[0];
                Sort var_sort = bound_var.sort();
                Term body = ch.back();

                auto git = ground_terms_by_sort_.find(var_sort.hash());
                if (git == ground_terms_by_sort_.end() || git->second.empty()) {
                    std::string def_name = "def_" + std::to_string(var_sort.hash());
                    Term def_const = ctx.make_symbol(def_name, var_sort);
                    ground_terms_by_sort_[var_sort.hash()].push_back(def_const);
                    git = ground_terms_by_sort_.find(var_sort.hash());
                }

                for (Term ground_t : git->second) {
                    const std::uint64_t inst_key = q.hash() ^ (ground_t.hash() * 0x9e3779b97f4a7c15ULL);
                    if (instantiated_quants_.contains(inst_key)) continue;
                    instantiated_quants_.insert(inst_key);

                    std::unordered_map<const TermImpl*, Term> subst;
                    subst[bound_var.ptr()] = ground_t;
                    Term inst_body = substitute(body, subst, ctx);
                    Term impl = (!q) || inst_body;
                    enc.assert_formula(simplifier::simplify(impl));
                }
            }
        }

        void reset() {
            forall_terms_.clear();
            exists_terms_.clear();
            ground_terms_by_sort_.clear();
            instantiated_quants_.clear();
        }

    private:
        void collect_quantifiers_and_ground(Term t) {
            if (t.op() == Op::Forall) {
                forall_terms_.push_back(t);
            } else if (t.op() == Op::Exists) {
                exists_terms_.push_back(t);
            }
            if (t.sort().valid()) {
                ground_terms_by_sort_[t.sort().hash()].push_back(t);
            }

            for (Term c : t.children()) {
                collect_quantifiers_and_ground(c);
            }
        }

        Term substitute(Term t, const std::unordered_map<const TermImpl*, Term>& subst, Context& ctx) {
            auto it = subst.find(t.ptr());
            if (it != subst.end()) return it->second;

            if (t.children().empty()) return t;

            std::vector<Term> new_ch;
            new_ch.reserve(t.children().size());
            bool changed = false;

            for (Term c : t.children()) {
                Term new_c = substitute(c, subst, ctx);
                if (new_c.ptr() != c.ptr()) changed = true;
                new_ch.push_back(new_c);
            }

            if (!changed) return t;
            return ctx.make_term(t.op(), t.sort(), new_ch);
        }

        atom_registry* reg_ = nullptr;
        cdcl_solver* sat_ = nullptr;
        std::vector<Term> forall_terms_;
        std::vector<Term> exists_terms_;
        std::unordered_map<std::uint64_t, std::vector<Term>> ground_terms_by_sort_;
        std::unordered_set<std::uint64_t> instantiated_quants_;
    };
} // namespace tarka::native
