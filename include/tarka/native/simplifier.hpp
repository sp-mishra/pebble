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
#include <unordered_map>
#include <vector>

namespace tarka::native {
    class simplifier {
    public:
        [[nodiscard]] static Term simplify(Term t) {
            if (!t.valid()) return t;
            std::unordered_map<std::uint64_t, Term> memo;
            return simplify_rec(t, memo);
        }

    private:
        [[nodiscard]] static Term simplify_rec(Term t, std::unordered_map<std::uint64_t, Term>& memo) {
            if (t.children().empty()) return t;

            const std::uint64_t key = t.hash();
            if (auto it = memo.find(key); it != memo.end()) return it->second;

            Context& ctx = t.ctx();
            auto orig_ch = t.children();
            std::vector<Term> ch;
            ch.reserve(orig_ch.size());
            bool ch_changed = false;

            for (Term c : orig_ch) {
                Term sc = simplify_rec(c, memo);
                if (sc.ptr() != c.ptr()) ch_changed = true;
                ch.push_back(sc);
            }

            Term res = ch_changed ? ctx.make_term(t.op(), t.sort(), ch, t.ptr()->payload_hash) : t;

            switch (res.op()) {
                case Op::Not: {
                    if (ch[0].op() == Op::True) return memo[key] = ctx.make_bool(false);
                    if (ch[0].op() == Op::False) return memo[key] = ctx.make_bool(true);
                    if (ch[0].op() == Op::Not) return memo[key] = ch[0].children()[0];
                    break;
                }
                case Op::And: {
                    for (Term c : ch) if (c.op() == Op::False) return memo[key] = ctx.make_bool(false);
                    std::vector<Term> non_true;
                    for (Term c : ch) if (c.op() != Op::True) non_true.push_back(c);
                    if (non_true.empty()) return memo[key] = ctx.make_bool(true);
                    if (non_true.size() == 1) return memo[key] = non_true[0];
                    if (non_true.size() != ch.size()) return memo[key] = ctx.make_term(Op::And, ctx.bool_sort(), non_true);
                    break;
                }
                case Op::Or: {
                    for (Term c : ch) if (c.op() == Op::True) return memo[key] = ctx.make_bool(true);
                    std::vector<Term> non_false;
                    for (Term c : ch) if (c.op() != Op::False) non_false.push_back(c);
                    if (non_false.empty()) return memo[key] = ctx.make_bool(false);
                    if (non_false.size() == 1) return memo[key] = non_false[0];
                    if (non_false.size() != ch.size()) return memo[key] = ctx.make_term(Op::Or, ctx.bool_sort(), non_false);
                    break;
                }
                case Op::Eq: {
                    if (ch.size() == 2 && ch[0].ptr() == ch[1].ptr()) return memo[key] = ctx.make_bool(true);
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
                default: break;
            }

            return memo[key] = res;
        }
    };
} // namespace tarka::native
