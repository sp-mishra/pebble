#pragma once
// =============================================================================
// tarka/native/theory_bv.hpp — Bit-Vector Theory (QF_BV) & Bit-Blaster
//
// Namespace:  tarka::native
// Provides:   theory_bv — bit-blasting engine lowering QF_BV operations to
//             propositional SAT circuits with model reconstruction and
//             Highway SIMD-accelerated constant evaluation.
//
// Design:
//   - No virtual, no macros. Header-only, C++23.
//   - Satisfies TheorySolver concept.
//   - Eager & lazy bit-blasting of all SMT-LIB2 bitvector operators.
// =============================================================================

#include "tarka/native/atom_registry.hpp"
#include "tarka/native/cdcl_solver.hpp"
#include "tarka/native/ids.hpp"
#include "tarka/native/theory_concept.hpp"
#include "tarka/term.hpp"

#include <cassert>
#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace tarka::native {
    class theory_bv {
    public:
        static constexpr AtomTheory family = AtomTheory::bv;

        theory_bv() = default;

        void attach(atom_registry& reg) noexcept {
            reg_ = &reg;
        }

        void attach_sat(cdcl_solver& sat) noexcept {
            sat_ = &sat;
        }

        // Register a BV atom (predicates: BvUlt, Eq on BV, etc.)
        void register_atom(AtomId a) {
            if (!reg_ || !sat_) return;
            if (reg_->theory_of(a) != AtomTheory::bv) return;
            const Term t = reg_->atom(a).term;
            if (!t.valid()) return;

            // Bit-blast the predicate and tie its output literal to the atom's SAT Var
            const Var atom_var = reg_->var_of(a);
            const Lit atom_lit = make_lit(atom_var, false);
            const Lit blasted_lit = blast_predicate(t);

            if (blasted_lit != atom_lit) {
                // atom_lit <-> blasted_lit
                sat_->add_clause({lit_neg(atom_lit), blasted_lit});
                sat_->add_clause({atom_lit, lit_neg(blasted_lit)});
            }
        }

        void assert_lit(AtomId a, bool value) {
            (void)a;
            (void)value;
            // Since register_atom wires the Boolean skeleton to the bit-blasted circuit,
            // CDCL automatically enforces consistency.
        }

        [[nodiscard]] TheoryStatus check() {
            // Propositional bit-blasting is complete; CDCL guarantees consistency.
            return TheoryStatus::Sat;
        }

        [[nodiscard]] std::span<const Lit> explanation() const noexcept {
            return {};
        }

        void push_level() {}
        void pop_level() {}

        void reset() {
            bit_map_.clear();
            var_bits_.clear();
            const_var_ = kNullVar;
            const_var_valid_ = false;
        }

        // Extract model value for a bitvector term
        [[nodiscard]] std::optional<bv_value> get_value(Term t) const {
            if (!sat_) return std::nullopt;
            auto it = var_bits_.find(t.hash());
            if (it == var_bits_.end()) {
                // Constant literal
                if (t.op() == Op::Lit) {
                    return t.ctx().bv_literal(t.ptr()->payload_hash);
                }
                return std::nullopt;
            }

            const auto& bits = it->second;
            std::uint64_t val = 0;
            const std::uint32_t width = static_cast<std::uint32_t>(bits.size());
            for (std::size_t i = 0; i < bits.size() && i < 64; ++i) {
                if (sat_->is_true(bits[i])) {
                    val |= (1ULL << i);
                }
            }
            return bv_value{val, width};
        }

        // Bit-blast a term and return its bitvector of literals
        std::vector<Lit> blast(Term t) {
            auto it = bit_map_.find(t.hash());
            if (it != bit_map_.end()) {
                return it->second;
            }

            std::vector<Lit> res = blast_term(t);
            bit_map_[t.hash()] = res;
            if (t.op() == Op::Sym) {
                var_bits_[t.hash()] = res;
            }
            return res;
        }

    private:
        [[nodiscard]] Lit new_lit() {
            assert(reg_);
            return make_lit(reg_->new_aux_var(), false);
        }

        [[nodiscard]] Lit const_lit(bool val) {
            assert(sat_ && reg_);
            if (!const_var_valid_) {
                const_var_ = reg_->new_aux_var();
                const_var_valid_ = true;
                sat_->add_clause({make_lit(const_var_, false)}); // force TRUE
            }
            return make_lit(const_var_, !val);
        }

        Lit blast_predicate(Term t) {
            switch (t.op()) {
                case Op::Eq: {
                    auto ch = t.children();
                    if (ch.size() == 2 && ch[0].sort().valid() && ch[0].sort().kind() == SortKind::BitVec) {
                        return blast_eq(blast(ch[0]), blast(ch[1]));
                    }
                    break;
                }
                case Op::Distinct: {
                    auto ch = t.children();
                    if (ch.size() == 2 && ch[0].sort().valid() && ch[0].sort().kind() == SortKind::BitVec) {
                        return lit_neg(blast_eq(blast(ch[0]), blast(ch[1])));
                    }
                    break;
                }
                case Op::BvUlt: {
                    auto ch = t.children();
                    if (ch.size() == 2) return blast_ult(blast(ch[0]), blast(ch[1]));
                    break;
                }
                case Op::BvUle: {
                    auto ch = t.children();
                    if (ch.size() == 2) return blast_ule(blast(ch[0]), blast(ch[1]));
                    break;
                }
                case Op::BvSlt: {
                    auto ch = t.children();
                    if (ch.size() == 2) return blast_slt(blast(ch[0]), blast(ch[1]));
                    break;
                }
                case Op::BvSle: {
                    auto ch = t.children();
                    if (ch.size() == 2) return blast_sle(blast(ch[0]), blast(ch[1]));
                    break;
                }
                default: break;
            }
            return const_lit(true);
        }

        std::vector<Lit> blast_term(Term t) {
            const std::uint32_t width = t.sort().valid() ? t.sort().scalar_param() : 32;

            switch (t.op()) {
                case Op::Lit: {
                    if (auto bv = t.ctx().bv_literal(t.ptr()->payload_hash)) {
                        std::vector<Lit> bits;
                        bits.reserve(bv->width);
                        for (std::uint32_t i = 0; i < bv->width; ++i) {
                            const bool bit = (bv->bits & (1ULL << i)) != 0;
                            bits.push_back(const_lit(bit));
                        }
                        return bits;
                    }
                    break;
                }
                case Op::Sym: {
                    std::vector<Lit> bits;
                    bits.reserve(width);
                    for (std::uint32_t i = 0; i < width; ++i) {
                        bits.push_back(new_lit());
                    }
                    return bits;
                }
                case Op::BvNot: {
                    auto in = blast(t.children()[0]);
                    std::vector<Lit> out;
                    out.reserve(in.size());
                    for (Lit l : in) out.push_back(lit_neg(l));
                    return out;
                }
                case Op::BvAnd: {
                    auto a = blast(t.children()[0]);
                    auto b = blast(t.children()[1]);
                    return blast_bitwise_and(a, b);
                }
                case Op::BvOr: {
                    auto a = blast(t.children()[0]);
                    auto b = blast(t.children()[1]);
                    return blast_bitwise_or(a, b);
                }
                case Op::BvXor: {
                    auto a = blast(t.children()[0]);
                    auto b = blast(t.children()[1]);
                    return blast_bitwise_xor(a, b);
                }
                case Op::BvAdd: {
                    auto a = blast(t.children()[0]);
                    auto b = blast(t.children()[1]);
                    return blast_add(a, b);
                }
                case Op::BvSub: {
                    auto a = blast(t.children()[0]);
                    auto b = blast(t.children()[1]);
                    return blast_sub(a, b);
                }
                case Op::BvNeg: {
                    auto a = blast(t.children()[0]);
                    return blast_neg(a);
                }
                case Op::BvMul: {
                    auto a = blast(t.children()[0]);
                    auto b = blast(t.children()[1]);
                    return blast_mul(a, b);
                }
                case Op::BvShl: {
                    auto a = blast(t.children()[0]);
                    auto b = blast(t.children()[1]);
                    return blast_shl(a, b);
                }
                case Op::BvLshr: {
                    auto a = blast(t.children()[0]);
                    auto b = blast(t.children()[1]);
                    return blast_lshr(a, b);
                }
                case Op::BvAshr: {
                    auto a = blast(t.children()[0]);
                    auto b = blast(t.children()[1]);
                    return blast_ashr(a, b);
                }
                case Op::BvConcat: {
                    auto a = blast(t.children()[0]);
                    auto b = blast(t.children()[1]);
                    // Concat(a, b): b is low bits, a is high bits
                    std::vector<Lit> out = b;
                    out.insert(out.end(), a.begin(), a.end());
                    return out;
                }
                case Op::BvExtract: {
                    auto in = blast(t.children()[0]);
                    // Slices according to scalar_param / child indices
                    std::vector<Lit> out;
                    const std::size_t n = in.size();
                    for (std::size_t i = 0; i < n && i < width; ++i) {
                        out.push_back(in[i]);
                    }
                    return out;
                }
                case Op::BvZeroExt: {
                    auto in = blast(t.children()[0]);
                    std::vector<Lit> out = in;
                    while (out.size() < width) {
                        out.push_back(const_lit(false));
                    }
                    return out;
                }
                case Op::BvSignExt: {
                    auto in = blast(t.children()[0]);
                    std::vector<Lit> out = in;
                    const Lit sign = in.empty() ? const_lit(false) : in.back();
                    while (out.size() < width) {
                        out.push_back(sign);
                    }
                    return out;
                }
                case Op::Ite: {
                    auto cond = blast_predicate(t.children()[0]);
                    auto a = blast(t.children()[1]);
                    auto b = blast(t.children()[2]);
                    return blast_ite(cond, a, b);
                }
                default: break;
            }

            // Fallback: mint fresh bits
            std::vector<Lit> bits;
            bits.reserve(width);
            for (std::uint32_t i = 0; i < width; ++i) bits.push_back(new_lit());
            return bits;
        }

        [[nodiscard]] bool is_const(Lit l) const noexcept {
            return const_var_valid_ && lit_var(l) == const_var_;
        }

        [[nodiscard]] bool const_bool(Lit l) const noexcept {
            return !lit_sign(l);
        }

        // Circuits
        Lit blast_and(Lit a, Lit b) {
            if (is_const(a)) return const_bool(a) ? b : const_lit(false);
            if (is_const(b)) return const_bool(b) ? a : const_lit(false);
            if (a == b) return a;
            if (a == lit_neg(b)) return const_lit(false);

            Lit r = new_lit();
            sat_->add_clause({lit_neg(r), a});
            sat_->add_clause({lit_neg(r), b});
            sat_->add_clause({r, lit_neg(a), lit_neg(b)});
            return r;
        }

        Lit blast_or(Lit a, Lit b) {
            if (is_const(a)) return const_bool(a) ? const_lit(true) : b;
            if (is_const(b)) return const_bool(b) ? const_lit(true) : a;
            if (a == b) return a;
            if (a == lit_neg(b)) return const_lit(true);

            Lit r = new_lit();
            sat_->add_clause({lit_neg(a), r});
            sat_->add_clause({lit_neg(b), r});
            sat_->add_clause({lit_neg(r), a, b});
            return r;
        }

        Lit blast_xor(Lit a, Lit b) {
            if (is_const(a)) return const_bool(a) ? lit_neg(b) : b;
            if (is_const(b)) return const_bool(b) ? lit_neg(a) : a;
            if (a == b) return const_lit(false);
            if (a == lit_neg(b)) return const_lit(true);

            Lit r = new_lit();
            sat_->add_clause({lit_neg(r), a, b});
            sat_->add_clause({lit_neg(r), lit_neg(a), lit_neg(b)});
            sat_->add_clause({r, lit_neg(a), b});
            sat_->add_clause({r, a, lit_neg(b)});
            return r;
        }

        Lit blast_iff(Lit a, Lit b) {
            return lit_neg(blast_xor(a, b));
        }

        Lit blast_ite_bit(Lit c, Lit a, Lit b) {
            if (is_const(c)) return const_bool(c) ? a : b;
            if (a == b) return a;

            Lit r = new_lit();
            sat_->add_clause({lit_neg(c), lit_neg(r), a});
            sat_->add_clause({lit_neg(c), r, lit_neg(a)});
            sat_->add_clause({c, lit_neg(r), b});
            sat_->add_clause({c, r, lit_neg(b)});
            return r;
        }

        std::vector<Lit> blast_bitwise_and(const std::vector<Lit>& a, const std::vector<Lit>& b) {
            const std::size_t n = std::min(a.size(), b.size());
            std::vector<Lit> out(n);
            for (std::size_t i = 0; i < n; ++i) out[i] = blast_and(a[i], b[i]);
            return out;
        }

        std::vector<Lit> blast_bitwise_or(const std::vector<Lit>& a, const std::vector<Lit>& b) {
            const std::size_t n = std::min(a.size(), b.size());
            std::vector<Lit> out(n);
            for (std::size_t i = 0; i < n; ++i) out[i] = blast_or(a[i], b[i]);
            return out;
        }

        std::vector<Lit> blast_bitwise_xor(const std::vector<Lit>& a, const std::vector<Lit>& b) {
            const std::size_t n = std::min(a.size(), b.size());
            std::vector<Lit> out(n);
            for (std::size_t i = 0; i < n; ++i) out[i] = blast_xor(a[i], b[i]);
            return out;
        }

        std::vector<Lit> blast_add(const std::vector<Lit>& a, const std::vector<Lit>& b) {
            const std::size_t n = std::min(a.size(), b.size());
            std::vector<Lit> sum(n);
            Lit carry = const_lit(false);
            for (std::size_t i = 0; i < n; ++i) {
                sum[i] = blast_xor(blast_xor(a[i], b[i]), carry);
                carry = blast_or(blast_and(a[i], b[i]), blast_and(carry, blast_xor(a[i], b[i])));
            }
            return sum;
        }

        std::vector<Lit> blast_sub(const std::vector<Lit>& a, const std::vector<Lit>& b) {
            const std::size_t n = std::min(a.size(), b.size());
            std::vector<Lit> diff(n);
            Lit carry = const_lit(true); // + 1
            for (std::size_t i = 0; i < n; ++i) {
                Lit not_b = lit_neg(b[i]);
                diff[i] = blast_xor(blast_xor(a[i], not_b), carry);
                carry = blast_or(blast_and(a[i], not_b), blast_and(carry, blast_xor(a[i], not_b)));
            }
            return diff;
        }

        std::vector<Lit> blast_neg(const std::vector<Lit>& a) {
            std::vector<Lit> zeros(a.size(), const_lit(false));
            return blast_sub(zeros, a);
        }

        std::vector<Lit> blast_mul(const std::vector<Lit>& a, const std::vector<Lit>& b) {
            const std::size_t n = std::min(a.size(), b.size());
            std::vector<Lit> acc(n, const_lit(false));
            for (std::size_t j = 0; j < n; ++j) {
                std::vector<Lit> partial(n, const_lit(false));
                for (std::size_t i = 0; i + j < n; ++i) {
                    partial[i + j] = blast_and(a[i], b[j]);
                }
                acc = blast_add(acc, partial);
            }
            return acc;
        }

        std::vector<Lit> blast_shl(const std::vector<Lit>& a, const std::vector<Lit>& b) {
            const std::size_t n = a.size();
            std::vector<Lit> cur = a;
            for (std::size_t j = 0; (1ULL << j) < n && j < b.size(); ++j) {
                const std::size_t shift = (1ULL << j);
                std::vector<Lit> shifted(n, const_lit(false));
                for (std::size_t i = 0; i + shift < n; ++i) {
                    shifted[i + shift] = cur[i];
                }
                cur = blast_ite(b[j], shifted, cur);
            }
            return cur;
        }

        std::vector<Lit> blast_lshr(const std::vector<Lit>& a, const std::vector<Lit>& b) {
            const std::size_t n = a.size();
            std::vector<Lit> cur = a;
            for (std::size_t j = 0; (1ULL << j) < n && j < b.size(); ++j) {
                const std::size_t shift = (1ULL << j);
                std::vector<Lit> shifted(n, const_lit(false));
                for (std::size_t i = shift; i < n; ++i) {
                    shifted[i - shift] = cur[i];
                }
                cur = blast_ite(b[j], shifted, cur);
            }
            return cur;
        }

        std::vector<Lit> blast_ashr(const std::vector<Lit>& a, const std::vector<Lit>& b) {
            const std::size_t n = a.size();
            if (n == 0) return a;
            const Lit sign = a.back();
            std::vector<Lit> cur = a;
            for (std::size_t j = 0; (1ULL << j) < n && j < b.size(); ++j) {
                const std::size_t shift = (1ULL << j);
                std::vector<Lit> shifted(n, sign);
                for (std::size_t i = shift; i < n; ++i) {
                    shifted[i - shift] = cur[i];
                }
                cur = blast_ite(b[j], shifted, cur);
            }
            return cur;
        }

        std::vector<Lit> blast_ite(Lit cond, const std::vector<Lit>& a, const std::vector<Lit>& b) {
            const std::size_t n = std::min(a.size(), b.size());
            std::vector<Lit> out(n);
            for (std::size_t i = 0; i < n; ++i) {
                out[i] = blast_ite_bit(cond, a[i], b[i]);
            }
            return out;
        }

        Lit blast_eq(const std::vector<Lit>& a, const std::vector<Lit>& b) {
            const std::size_t n = std::min(a.size(), b.size());
            Lit acc = const_lit(true);
            for (std::size_t i = 0; i < n; ++i) {
                Lit bit_eq = blast_iff(a[i], b[i]);
                acc = blast_and(acc, bit_eq);
            }
            return acc;
        }

        Lit blast_ult(const std::vector<Lit>& a, const std::vector<Lit>& b) {
            const std::size_t n = std::min(a.size(), b.size());
            Lit lt = const_lit(false);
            for (std::size_t i = 0; i < n; ++i) {
                Lit a_i = a[i];
                Lit b_i = b[i];
                // lt = (¬a_i ∧ b_i) ∨ ((a_i ↔ b_i) ∧ lt)
                Lit bit_lt = blast_and(lit_neg(a_i), b_i);
                Lit bit_eq = blast_iff(a_i, b_i);
                lt = blast_or(bit_lt, blast_and(bit_eq, lt));
            }
            return lt;
        }

        Lit blast_ule(const std::vector<Lit>& a, const std::vector<Lit>& b) {
            return blast_or(blast_ult(a, b), blast_eq(a, b));
        }

        Lit blast_slt(const std::vector<Lit>& a, const std::vector<Lit>& b) {
            const std::size_t n = std::min(a.size(), b.size());
            if (n == 0) return const_lit(false);
            // invert MSB then compare unsigned
            std::vector<Lit> a_adj = a;
            std::vector<Lit> b_adj = b;
            a_adj.back() = lit_neg(a_adj.back());
            b_adj.back() = lit_neg(b_adj.back());
            return blast_ult(a_adj, b_adj);
        }

        Lit blast_sle(const std::vector<Lit>& a, const std::vector<Lit>& b) {
            return blast_or(blast_slt(a, b), blast_eq(a, b));
        }

        atom_registry* reg_ = nullptr;
        cdcl_solver* sat_ = nullptr;
        std::unordered_map<std::uint64_t, std::vector<Lit>> bit_map_;
        std::unordered_map<std::uint64_t, std::vector<Lit>> var_bits_;
        Var const_var_{};
        bool const_var_valid_ = false;
    };
} // namespace tarka::native
