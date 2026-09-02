#pragma once
// =============================================================================
// tarka/native/cnf_encoder.hpp — Tseitin / Plaisted-Greenbaum CNF encoding
//
// Namespace:  tarka::native
// Provides:   cnf_encoder — lowers a tarka Boolean-skeleton Term into CNF
//             clauses over a cdcl_solver, interning theory atoms as opaque
//             SAT variables via atom_registry.
//
// Design:
//   - No virtual, no macros. Header-only, C++23.
//   - Structure-sharing: every subterm is encoded once (memoized on Term.hash);
//     re-encoding a shared node reuses its literal. Purely functional walk over
//     the immutable interned DAG — no term mutation.
//   - Plaisted-Greenbaum polarity optimization: for a subformula appearing only
//     positively (resp. negatively) we emit only the implications needed in that
//     polarity, roughly halving clause count vs. full Tseitin. When a node is
//     seen in both polarities we fall back to the full bi-implication.
//   - Connectives handled here (the Boolean skeleton): True/False, Not, And, Or,
//     Xor, Implies, Ite(bool), Eq(bool)/Distinct(bool). Everything else is a
//     THEORY ATOM: interned once, represented by a fresh SAT var, and left for
//     the theory layer to constrain. Eq/Distinct over non-Bool sorts are theory
//     atoms (EUF/arith), NOT Boolean equivalence.
//
// Polarity:
//   +1  formula must be forced (asserted true / positive context)
//   -1  negative context
//   both bits => full bi-implication (Tseitin)
// =============================================================================

#include "tarka/native/atom_registry.hpp"
#include "tarka/native/cdcl_solver.hpp"
#include "tarka/native/ids.hpp"
#include "tarka/term.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace tarka::native {
    // Polarity bitset: bit0 = positive occurrence, bit1 = negative occurrence.
    enum class Polarity : std::uint8_t { pos = 1, neg = 2, both = 3 };

    [[nodiscard]] constexpr Polarity operator|(Polarity a, Polarity b) noexcept {
        return static_cast<Polarity>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
    }

    [[nodiscard]] constexpr bool has_pos(Polarity p) noexcept {
        return (static_cast<std::uint8_t>(p) & 1u) != 0u;
    }

    [[nodiscard]] constexpr bool has_neg(Polarity p) noexcept {
        return (static_cast<std::uint8_t>(p) & 2u) != 0u;
    }

    [[nodiscard]] constexpr Polarity flip(Polarity p) noexcept {
        const std::uint8_t v = static_cast<std::uint8_t>(p);
        return static_cast<Polarity>(((v & 1u) << 1) | ((v & 2u) >> 1));
    }

    class cnf_encoder {
    public:
        cnf_encoder(cdcl_solver& sat, atom_registry& reg) : sat_(sat), reg_(reg) {
            reg_.set_var_minter([&sat] { return sat.new_var(); });
        }

        // Route an atom to the theory that owns its semantics, from op + sorts.
        [[nodiscard]] static AtomTheory classify(Term t) noexcept {
            switch (t.op()) {
            case Op::BvUlt:
            case Op::BvUle:
            case Op::BvSlt:
            case Op::BvSle:
                return AtomTheory::bv;
            case Op::Select:
            case Op::Store:
                return AtomTheory::array;
            case Op::Apply:
                return AtomTheory::uf;
            case Op::Lt:
            case Op::Le:
            case Op::Gt:
            case Op::Ge:
                return arith_theory(t);
            case Op::Eq:
            case Op::Distinct:
                return eq_theory(t);
            default:
                return AtomTheory::core; // Bool Sym / opaque predicate
            }
        }

        // Arithmetic relation: Real -> LRA, Int -> LIA.
        [[nodiscard]] static AtomTheory arith_theory(Term t) noexcept {
            auto ch = t.children();
            if (!ch.empty() && ch[0].sort().valid()) {
                return ch[0].sort().kind() == SortKind::Real ? AtomTheory::lra : AtomTheory::lia;
            }
            return AtomTheory::lia;
        }

        // Eq/Distinct dispatch on argument sort.
        [[nodiscard]] static AtomTheory eq_theory(Term t) noexcept {
            auto ch = t.children();
            if (ch.empty() || !ch[0].sort().valid()) return AtomTheory::core;
            switch (ch[0].sort().kind()) {
            case SortKind::BitVec: return AtomTheory::bv;
            case SortKind::Real: return AtomTheory::lra;
            case SortKind::Int: return AtomTheory::lia;
            case SortKind::Array: return AtomTheory::array;
            default: return AtomTheory::uf; // uninterpreted / function-sorted
            }
        }

        // Encode `t` and force it to be true (top-level assertion).
        void assert_formula(Term t) {
            const Lit l = encode(t, Polarity::pos);
            sat_.add_clause({l});
        }

        // Encode `t` returning the literal that is true iff `t` holds. `pol`
        // records the polarity context so PG can emit half the clauses.
        Lit encode(Term t, Polarity pol) {
            const std::uint64_t h = t.hash();
            if (auto it = memo_.find(h); it != memo_.end()) {
                CacheEntry& e = it->second;
                if (e.term.ptr() == t.ptr()) {
                    // widen polarity: emit any clauses newly required
                    const Polarity added = static_cast<Polarity>(
                        static_cast<std::uint8_t>(pol) & ~static_cast<std::uint8_t>(e.pol));
                    if (added != static_cast<Polarity>(0)) {
                        e.pol = e.pol | pol;
                        emit_definition(t, e.lit, added);
                    }
                    return e.lit;
                }
            }
            const Lit l = encode_fresh(t, pol);
            memo_[h] = CacheEntry{t, l, pol};
            return l;
        }

    private:
        struct CacheEntry {
            Term term;
            Lit lit;
            Polarity pol;
        };

        // Encode a not-yet-seen node: allocate its literal, then emit the
        // polarity-appropriate defining clauses.
        Lit encode_fresh(Term t, Polarity pol) {
            switch (t.op()) {
            case Op::True: return const_lit(true);
            case Op::False: return const_lit(false);
            case Op::Not: {
                // ¬ is free: encode child in flipped polarity, negate literal.
                return lit_neg(encode(t.children()[0], flip(pol)));
            }
            default: break;
            }

            if (is_boolean_connective(t)) {
                const Lit l = make_lit(reg_.new_aux_var(), false);
                emit_definition(t, l, pol);
                return l;
            }

            // Theory atom (or a Boolean symbol/leaf): intern, no defining clauses.
            const AtomId a = reg_.intern(t, classify(t));
            return make_lit(reg_.var_of(a), false);
        }

        // Emit the defining clauses relating aux literal `l` to node `t` for the
        // freshly-added polarity bits in `pol`.
        void emit_definition(Term t, Lit l, Polarity pol) {
            switch (t.op()) {
            case Op::And: emit_and(t, l, pol);
                break;
            case Op::Or: emit_or(t, l, pol);
                break;
            case Op::Implies: emit_implies(t, l, pol);
                break;
            case Op::Xor: emit_xor(t, l, pol);
                break;
            case Op::Ite: emit_ite(t, l, pol);
                break;
            case Op::Eq: emit_iff(t, l, pol);
                break; // Bool Eq == iff (checked by caller)
            case Op::Distinct: emit_xor(t, l, pol);
                break; // Bool Distinct == xor
            default: break;
            }
        }

        // l <-> AND(c_i). pos: l -> c_i. neg: (¬c_1 ∨ … ∨ ¬c_n) -> ¬l  i.e.  l ∨ ...
        void emit_and(Term t, Lit l, Polarity pol) {
            auto ch = t.children();
            if (has_pos(pol)) {
                for (Term c : ch) sat_.add_clause({lit_neg(l), encode(c, Polarity::pos)});
            }
            if (has_neg(pol)) {
                std::vector<Lit> cl;
                cl.reserve(ch.size() + 1);
                cl.push_back(l);
                for (Term c : ch) cl.push_back(lit_neg(encode(c, Polarity::neg)));
                sat_.add_clause(std::span<const Lit>{cl});
            }
        }

        // l <-> OR(c_i). pos: (¬l ∨ c_1 ∨ … ). neg: c_i -> l.
        void emit_or(Term t, Lit l, Polarity pol) {
            auto ch = t.children();
            if (has_pos(pol)) {
                std::vector<Lit> cl;
                cl.reserve(ch.size() + 1);
                cl.push_back(lit_neg(l));
                for (Term c : ch) cl.push_back(encode(c, Polarity::pos));
                sat_.add_clause(std::span<const Lit>{cl});
            }
            if (has_neg(pol)) {
                for (Term c : ch) sat_.add_clause({lit_neg(encode(c, Polarity::neg)), l});
            }
        }

        // l <-> (a -> b) = (¬a ∨ b). Binary.
        void emit_implies(Term t, Lit l, Polarity pol) {
            const Lit a = encode(t.children()[0], Polarity::both);
            const Lit b = encode(t.children()[1], Polarity::both);
            if (has_pos(pol)) {
                sat_.add_clause({lit_neg(l), lit_neg(a), b});
            }
            if (has_neg(pol)) {
                sat_.add_clause({l, a});
                sat_.add_clause({l, lit_neg(b)});
            }
        }

        // l <-> (a XOR b). Binary (n-ary xor folds pairwise via nested nodes).
        void emit_xor(Term t, Lit l, Polarity pol) {
            auto ch = t.children();
            Lit a = encode(ch[0], Polarity::both);
            for (std::size_t i = 1; i < ch.size(); ++i) {
                const Lit b = encode(ch[i], Polarity::both);
                const Lit out = (i + 1 == ch.size()) ? l : make_lit(reg_.new_aux_var(), false);
                // out <-> a xor b   (full, both polarities — xor has no cheap PG form)
                sat_.add_clause({lit_neg(out), a, b});
                sat_.add_clause({lit_neg(out), lit_neg(a), lit_neg(b)});
                sat_.add_clause({out, lit_neg(a), b});
                sat_.add_clause({out, a, lit_neg(b)});
                a = out;
            }
            (void)pol;
        }

        // l <-> (a <-> b), Boolean equality. Full bi-implication.
        void emit_iff(Term t, Lit l, Polarity pol) {
            const Lit a = encode(t.children()[0], Polarity::both);
            const Lit b = encode(t.children()[1], Polarity::both);
            sat_.add_clause({lit_neg(l), lit_neg(a), b});
            sat_.add_clause({lit_neg(l), a, lit_neg(b)});
            sat_.add_clause({l, a, b});
            sat_.add_clause({l, lit_neg(a), lit_neg(b)});
            (void)pol;
        }

        // l <-> ite(c, a, b). Full (both polarities): (c -> (l<->a)) & (¬c -> (l<->b)).
        void emit_ite(Term t, Lit l, Polarity pol) {
            const Lit c = encode(t.children()[0], Polarity::both);
            const Lit a = encode(t.children()[1], Polarity::both);
            const Lit b = encode(t.children()[2], Polarity::both);
            sat_.add_clause({lit_neg(c), lit_neg(l), a});
            sat_.add_clause({lit_neg(c), l, lit_neg(a)});
            sat_.add_clause({c, lit_neg(l), b});
            sat_.add_clause({c, l, lit_neg(b)});
            (void)pol;
        }

        // ---- helpers --------------------------------------------------------

        [[nodiscard]] Lit const_lit(bool truth) {
            if (!const_var_valid_) {
                const_var_ = reg_.new_aux_var();
                const_var_valid_ = true;
                sat_.add_clause({make_lit(const_var_, false)}); // force TRUE
            }
            return make_lit(const_var_, /*negated=*/!truth);
        }

        [[nodiscard]] static bool is_boolean_connective(Term t) noexcept {
            switch (t.op()) {
            case Op::And:
            case Op::Or:
            case Op::Xor:
            case Op::Implies:
                return true;
            case Op::Ite: {
                // Only Bool-sorted ite is a formula connective; arithmetic /
                // BV ite is a term handled inside a theory atom.
                return t.sort().valid() && t.sort().kind() == SortKind::Bool;
            }
            case Op::Eq:
            case Op::Distinct: {
                // Boolean equality is iff (a connective); Eq over other sorts
                // is a theory atom.
                auto ch = t.children();
                return !ch.empty() && ch[0].sort().valid() &&
                    ch[0].sort().kind() == SortKind::Bool;
            }
            default:
                return false;
            }
        }


        cdcl_solver& sat_;
        atom_registry& reg_;
        std::unordered_map<std::uint64_t, CacheEntry> memo_;
        Var const_var_{};
        bool const_var_valid_ = false;
    };
} // namespace tarka::native
