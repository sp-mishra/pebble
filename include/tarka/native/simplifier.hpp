// =============================================================================
// Tarka — Zero-Overhead Multi-Solver SMT Substrate
// include/tarka/native/simplifier.hpp
//
// Pre-encoding algebraic simplification pass & constant folder.
// Simplifies term DAG before Tseitin encoding to reduce auxiliary SAT variables.
// =============================================================================

#pragma once

#include <tarka/term.hpp>
#include <tarka/context.hpp>
#include "containers/dynamic/SmallVector.hpp"
#include <unordered_map>

namespace tarka::native {
    class simplifier {
    public:
        [[nodiscard]] static Term simplify(Term t) {
            if (!t.valid()) return t;
            // Memo keyed on the interned node identity, not hash(). Hash-consing
            // guarantees one TermImpl* per distinct term, so the pointer is a
            // collision-free key; hash() aliased distinct terms → wrong rewrites.
            std::unordered_map<const TermImpl*, Term> memo;
            return simplify_rec(t, memo);
        }

    private:
        // SBO term list: most nodes have few children, so this stays stack-resident.
        using TermVec = containers::dynamic::SmallVector<Term, 8 * sizeof(Term)>;

        [[nodiscard]] static Term simplify_rec(Term t, std::unordered_map<const TermImpl*, Term>& memo) {
            if (t.children().empty()) return t;

            const TermImpl* key = t.ptr();
            if (auto it = memo.find(key); it != memo.end()) return it->second;

            Context& ctx = t.ctx();
            auto orig_ch = t.children();
            TermVec ch;
            ch.reserve(orig_ch.size());
            bool ch_changed = false;

            for (Term c : orig_ch) {
                Term sc = simplify_rec(c, memo);
                if (sc.ptr() != c.ptr()) ch_changed = true;
                ch.push_back(sc);
            }

            Term res = ch_changed
                           ? ctx.make_term(t.op(), t.sort(),
                                           std::span<const Term>(ch.data(), ch.size()),
                                           t.ptr()->payload_hash)
                           : t;

            switch (res.op()) {
                case Op::Not: {
                    if (ch[0].op() == Op::True) return memo[key] = ctx.make_bool(false);
                    if (ch[0].op() == Op::False) return memo[key] = ctx.make_bool(true);
                    if (ch[0].op() == Op::Not) return memo[key] = ch[0].children()[0];
                    break;
                }
                case Op::And: {
                    for (Term c : ch) if (c.op() == Op::False) return memo[key] = ctx.make_bool(false);
                    TermVec non_true;
                    for (Term c : ch) if (c.op() != Op::True) non_true.push_back(c);
                    if (non_true.empty()) return memo[key] = ctx.make_bool(true);
                    if (non_true.size() == 1) return memo[key] = non_true[0];
                    if (non_true.size() != ch.size())
                        return memo[key] = ctx.make_term(Op::And, ctx.bool_sort(),
                                                         std::span<const Term>(non_true.data(), non_true.size()));
                    break;
                }
                case Op::Or: {
                    for (Term c : ch) if (c.op() == Op::True) return memo[key] = ctx.make_bool(true);
                    TermVec non_false;
                    for (Term c : ch) if (c.op() != Op::False) non_false.push_back(c);
                    if (non_false.empty()) return memo[key] = ctx.make_bool(false);
                    if (non_false.size() == 1) return memo[key] = non_false[0];
                    if (non_false.size() != ch.size())
                        return memo[key] = ctx.make_term(Op::Or, ctx.bool_sort(),
                                                         std::span<const Term>(non_false.data(), non_false.size()));
                    break;
                }
                case Op::Eq: {
                    if (ch.size() == 2 && ch[0].ptr() == ch[1].ptr()) return memo[key] = ctx.make_bool(true);
                    // fold two concrete Int literals (item 24)
                    std::int64_t a, b;
                    if (ch.size() == 2 && int_val(ctx, ch[0], a) && int_val(ctx, ch[1], b))
                        return memo[key] = ctx.make_bool(a == b);
                    break;
                }
                case Op::Distinct: {
                    if (ch.size() == 2 && ch[0].ptr() == ch[1].ptr()) return memo[key] = ctx.make_bool(false);
                    break;
                }
                case Op::BvNot: {
                    if (ch[0].op() == Op::BvNot) return memo[key] = ch[0].children()[0];
                    break;
                }
                case Op::BvXor: {
                    if (ch.size() == 2 && ch[0].ptr() == ch[1].ptr()) {
                        return memo[key] = ctx.make_value(0, t.sort());
                    }
                    break;
                }
                case Op::BvSub: {
                    if (ch.size() == 2 && ch[0].ptr() == ch[1].ptr()) {
                        return memo[key] = ctx.make_value(0, t.sort());
                    }
                    break;
                }
                case Op::Select: {
                    // select(store(a, i, v), i) -> v
                    if (ch.size() == 2 && ch[0].op() == Op::Store) {
                        auto store_ch = ch[0].children();
                        if (store_ch.size() == 3 && store_ch[1].ptr() == ch[1].ptr()) {
                            return memo[key] = store_ch[2];
                        }
                    }
                    break;
                }
                // --- integer constant folding (item 24) -------------------------
                // Fold arithmetic and relational ops whose operands are all
                // concrete Int literals. Shrinks the term DAG before Tseitin so
                // fewer theory atoms reach the LIA solver.
                case Op::Neg: {
                    std::int64_t a;
                    if (ch.size() == 1 && int_val(ctx, ch[0], a))
                        return memo[key] = ctx.make_int(-a, t.sort());
                    break;
                }
                case Op::Add: case Op::Sub: case Op::Mul: {
                    std::int64_t acc;
                    if (fold_arith(ctx, res.op(), ch, acc))
                        return memo[key] = ctx.make_int(acc, t.sort());
                    break;
                }
                case Op::Lt: case Op::Le: case Op::Gt: case Op::Ge: {
                    std::int64_t a, b;
                    if (ch.size() == 2 && int_val(ctx, ch[0], a) && int_val(ctx, ch[1], b)) {
                        const bool r = res.op() == Op::Lt ? (a < b)
                                     : res.op() == Op::Le ? (a <= b)
                                     : res.op() == Op::Gt ? (a > b)
                                                          : (a >= b);
                        return memo[key] = ctx.make_bool(r);
                    }
                    break;
                }
                default: break;
            }

            return memo[key] = res;
        }

        // Read a term's concrete Int-literal value; false if not an int literal.
        [[nodiscard]] static bool int_val(Context& ctx, Term t, std::int64_t& out) {
            if (t.op() != Op::Lit) return false;
            if (auto v = ctx.int_literal(t.ptr()->payload_hash)) { out = *v; return true; }
            return false;
        }

        // Fold Add/Sub/Mul over an all-literal child list. Sub with one operand
        // is unary negate-style; Add/Mul use their identity as the seed.
        [[nodiscard]] static bool fold_arith(Context& ctx, Op op,
                                             const TermVec& ch, std::int64_t& out) {
            if (ch.empty()) return false;
            std::int64_t vals[64];
            const std::size_t n = ch.size();
            if (n > 64) return false;
            for (std::size_t i = 0; i < n; ++i)
                if (!int_val(ctx, ch[i], vals[i])) return false;
            if (op == Op::Add) {
                std::int64_t s = 0;
                for (std::size_t i = 0; i < n; ++i) s += vals[i];
                out = s;
            } else if (op == Op::Mul) {
                std::int64_t p = 1;
                for (std::size_t i = 0; i < n; ++i) p *= vals[i];
                out = p;
            } else { // Op::Sub — (a - b - c ...) ; unary (- a) not reached here
                std::int64_t s = vals[0];
                for (std::size_t i = 1; i < n; ++i) s -= vals[i];
                out = s;
            }
            return true;
        }
    };
} // namespace tarka::native
