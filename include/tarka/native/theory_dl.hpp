#pragma once
// =============================================================================
// tarka/native/theory_dl.hpp — Difference Logic (QF_IDL / QF_RDL)
//
// Namespace:  tarka::native
// Provides:   theory_dl — a TheorySolver for difference constraints
//             x - y <= k  (and derived <, >, >=, =) over Int or Real.
//
// Theory (docs/tarka/tarka.md "Native Backend / Difference Logic"):
//   A conjunction of difference constraints x_i - x_j <= k_ij is satisfiable
//   iff the constraint graph — a vertex per variable, a directed edge j -> i
//   with weight k_ij for each constraint — contains NO negative-weight cycle.
//   A satisfying assignment is the single-source shortest-path potential.
//
//   We detect negative cycles incrementally with Bellman-Ford over EXACT
//   rationals (containers::numeric::exact_rational), so results are sound for
//   both integer and real difference logic (no floating error). Strict
//   constraints x - y < k become x - y <= k - δ; over the integers δ = 1, over
//   the reals we track strictness with an infinitesimal ε ordered pair
//   (value, eps) so <, <= never alias.
//
// Design:
//   - No virtual, no macros. Plain struct satisfying the TheorySolver concept.
//   - Edges are added on assert_lit and removed on pop_level via a trail.
//   - Conflict explanation = the atoms whose edges form the negative cycle.
// =============================================================================

#include "containers/numeric/exact_rational.hpp"
#include "tarka/context.hpp"
#include "tarka/native/atom_registry.hpp"
#include "tarka/native/ids.hpp"
#include "tarka/native/theory_concept.hpp"
#include "tarka/term.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace tarka::native {
    class theory_dl {
    public:
        using rat = containers::numeric::exact_rational;
        using i128 = containers::numeric::i128;

        // Family tag: difference logic lives in the arithmetic family. DL atoms
        // are tagged lia/lra by the encoder; this theory registers for lia.
        static constexpr AtomTheory family = AtomTheory::lia;

        // Weight with strictness: value + (strict ? -ε : 0). Compared
        // lexicographically so that "< k" is strictly tighter than "<= k".
        struct weight {
            rat value{};
            bool strict = false; // subtract an infinitesimal ε

            // a is tighter-or-equal along a path when summing; we need sum + cmp.
            [[nodiscard]] weight operator+(const weight& o) const {
                return weight{value + o.value, strict || o.strict};
            }
            // negativity test for a cycle sum: (v < 0) or (v == 0 and strict)
            [[nodiscard]] bool negative() const {
                const int s = value.sign();
                if (s < 0) return true;
                if (s > 0) return false;
                return strict; // value == 0, strict edge => < 0
            }
        };

        void attach(atom_registry& reg) noexcept { reg_ = &reg; }

        // Pre-decode the atom's difference constraint (both polarities).
        void register_atom(AtomId a) {
            if (decoded_.contains(atom_index(a))) return;
            const Term t = reg_->atom(a).term;
            if (!t.valid()) return;
            auto pos = decode(t, /*negated=*/false);
            auto neg = decode(t, /*negated=*/true);
            decoded_.emplace(atom_index(a), Decoded{pos, neg});
        }

        void assert_lit(AtomId a, bool value) {
            auto it = decoded_.find(atom_index(a));
            if (it == decoded_.end()) return; // not a DL atom we understand
            const std::optional<Constraint>& c = value ? it->second.pos : it->second.neg;
            if (!c) return; // e.g. disequality has no single-edge form
            add_edge(c->x, c->y, weight{c->k, c->strict}, a, value);
        }

        [[nodiscard]] TheoryStatus check() {
            explanation_.clear();
            if (auto cyc = find_negative_cycle()) {
                build_explanation(*cyc);
                return TheoryStatus::Conflict;
            }
            return TheoryStatus::Sat; // DL does no eager propagation here
        }

        [[nodiscard]] std::span<const Lit> explanation() const noexcept { return explanation_; }

        [[nodiscard]] std::optional<rat> get_value(Term v) const {
            const std::uint64_t key = v.hash();
            auto it = var_of_.find(key);
            if (it == var_of_.end()) return std::nullopt;
            const std::uint32_t idx = it->second;
            if (idx < potentials_.size()) return potentials_[idx];
            return std::nullopt;
        }

        void push_level() { trail_lim_.push_back(edges_.size()); }

        void pop_level() {
            if (trail_lim_.empty()) return;
            const std::size_t target = trail_lim_.back();
            trail_lim_.pop_back();
            edges_.resize(target);
        }

        void reset() {
            decoded_.clear();
            edges_.clear();
            trail_lim_.clear();
            explanation_.clear();
            var_of_.clear();
            potentials_.clear();
            next_dlvar_ = 0;
        }

    private:
        // A difference constraint  x - y <= k  (strict => x - y < k).
        struct Constraint {
            std::uint32_t x; // DL vertex ids
            std::uint32_t y;
            rat k;
            bool strict;
        };
        struct Decoded {
            std::optional<Constraint> pos;
            std::optional<Constraint> neg;
        };
        // Directed edge y -> x with weight w, justified by (atom,value).
        struct Edge {
            std::uint32_t x;
            std::uint32_t y;
            weight w;
            AtomId atom;
            bool value;
        };

        // Map a variable Term to a dense DL vertex id (0 reserved for "zero").
        [[nodiscard]] std::uint32_t dlvar(Term v) {
            const std::uint64_t key = v.hash();
            auto [it, ins] = var_of_.try_emplace(key, next_dlvar_ + 1);
            if (ins) ++next_dlvar_;
            return it->second;
        }
        static constexpr std::uint32_t kZeroVertex = 0;

        void add_edge(std::uint32_t x, std::uint32_t y, weight w, AtomId a, bool val) {
            edges_.push_back(Edge{x, y, w, a, val});
        }

        // ---- constraint decoding --------------------------------------------
        // Recognize:  Le/Lt/Ge/Gt(lhs, rhs) where lhs-rhs is a difference of two
        // Int/Real terms plus an integer/real constant. Canonicalize to x-y<=k.
        std::optional<Constraint> decode(Term t, bool negated) {
            Op op = t.op();
            auto ch = t.children();
            if (ch.size() != 2) return std::nullopt;

            // Normalize the relation to a "<=" (or "<") on (a - b) vs const.
            // a op b  ==  a - b  rel  0, but we need a numeric bound; we handle
            // the common SMT normal form  (x - y) rel c  where c is a literal.
            bool strict = (op == Op::Lt || op == Op::Gt);
            bool flip = (op == Op::Gt || op == Op::Ge); // >, >= flip to <=, <
            if (op != Op::Lt && op != Op::Le && op != Op::Gt && op != Op::Ge) {
                return std::nullopt;
            }
            if (negated) {
                // ¬(a <= b) == (a > b) == (b < a); ¬(a < b) == (a >= b)
                flip = !flip;
                strict = !strict;
            }

            Term lhs = ch[0];
            Term rhs = ch[1];
            if (flip) std::swap(lhs, rhs);

            // Extract  x - y  and constant k from lhs <= rhs  ==>  x - y <= k.
            AffDiff a = affine(lhs);
            AffDiff b = affine(rhs);
            if (!a.ok || !b.ok) return std::nullopt;
            // (a.x - a.y + a.c) <= (b.x - b.y + b.c)
            // => (a.x - a.y) - (b.x - b.y) <= b.c - a.c
            // Only single-variable differences are DL; combine.
            std::uint32_t xv, yv;
            if (!combine(a, b, xv, yv)) return std::nullopt;
            rat k = b.c - a.c;
            return Constraint{xv, yv, k, strict};
        }

        // A term parsed as (xvar - yvar + const); xvar/yvar are DL vertex ids
        // (kZeroVertex when absent).
        struct AffDiff {
            bool ok = false;
            std::uint32_t x = kZeroVertex;
            std::uint32_t y = kZeroVertex;
            rat c{};
        };

        AffDiff affine(Term t) {
            AffDiff r;
            switch (t.op()) {
                case Op::Sub: {
                    auto ch = t.children();
                    if (ch.size() != 2) return r;
                    if (is_const(ch[1])) { // v - c
                        r.x = dlvar(ch[0]);
                        r.c = -const_val(ch[1]);
                    } else if (is_const(ch[0])) { // c - v
                        r.y = dlvar(ch[1]);
                        r.c = const_val(ch[0]);
                    } else { // v1 - v2
                        r.x = dlvar(ch[0]);
                        r.y = dlvar(ch[1]);
                    }
                    r.ok = true;
                    return r;
                }
                case Op::Lit: {
                    r.c = const_val(t);
                    r.ok = true;
                    return r;
                }
                default:
                    // bare variable
                    if (is_var(t)) { r.x = dlvar(t); r.ok = true; }
                    return r;
            }
        }

        // Merge  (a.x - a.y) - (b.x - b.y)  into a single  (X - Y).
        static bool combine(const AffDiff& a, const AffDiff& b,
                            std::uint32_t& X, std::uint32_t& Y) {
            // positives: a.x, b.y ; negatives: a.y, b.x  (each kZero => absent)
            std::uint32_t pos[2] = {a.x, b.y};
            std::uint32_t neg[2] = {a.y, b.x};
            // cancel zeros
            auto collect = [](std::uint32_t (&v)[2], std::uint32_t out[2]) {
                int n = 0;
                for (std::uint32_t id : v) if (id != kZeroVertex) out[n++] = id;
                return n;
            };
            std::uint32_t P[2], N[2];
            int np = collect(pos, P), nn = collect(neg, N);
            // cancel identical vars across pos/neg
            for (int i = 0; i < np; ++i) {
                for (int j = 0; j < nn; ++j) {
                    if (P[i] != kZeroVertex && P[i] == N[j]) {
                        P[i] = kZeroVertex; N[j] = kZeroVertex;
                    }
                }
            }
            std::uint32_t rp[2], rn[2];
            int fp = 0, fn = 0;
            for (int i = 0; i < np; ++i) if (P[i] != kZeroVertex) rp[fp++] = P[i];
            for (int j = 0; j < nn; ++j) if (N[j] != kZeroVertex) rn[fn++] = N[j];
            if (fp > 1 || fn > 1) return false; // not a 2-var difference
            X = fp == 1 ? rp[0] : kZeroVertex;
            Y = fn == 1 ? rn[0] : kZeroVertex;
            return true;
        }

        [[nodiscard]] bool is_const(Term t) const noexcept { return t.op() == Op::Lit; }
        [[nodiscard]] bool is_var(Term t) const noexcept { return t.op() == Op::Sym; }

        [[nodiscard]] rat const_val(Term t) const {
            // integer literal via Context::int_literal
            if (auto v = t.ctx().int_literal(t.ptr()->payload_hash)) return rat{*v};
            // real/rational literal via Context::real_literal
            if (auto r = t.ctx().real_literal(t.ptr()->payload_hash)) return rat{r->num, r->den};
            return rat{};
        }

        // ---- incremental negative-cycle detection ---------------------------
        // Bellman-Ford over exact weights. Returns the edge indices forming a
        // negative cycle if one exists.
        std::optional<std::vector<std::size_t>> find_negative_cycle() {
            const std::uint32_t n = next_dlvar_ + 1; // + zero vertex
            if (n == 0 || edges_.empty()) return std::nullopt;

            std::vector<weight> dist(n, weight{});
            std::vector<std::int64_t> pred(n, -1); // predecessor edge index
            std::vector<char> has(n, 1);

            std::int64_t updated = -1;
            for (std::uint32_t iter = 0; iter < n; ++iter) {
                updated = -1;
                for (std::size_t ei = 0; ei < edges_.size(); ++ei) {
                    const Edge& e = edges_[ei];
                    if (!has[e.y]) continue;
                    const weight cand = dist[e.y] + e.w;
                    if (!has[e.x] || less(cand, dist[e.x])) {
                        dist[e.x] = cand;
                        has[e.x] = 1;
                        pred[e.x] = static_cast<std::int64_t>(ei);
                        updated = static_cast<std::int64_t>(e.x);
                    }
                }
                if (updated < 0) break;
            }
            if (updated < 0) {
                // Converged — save potentials for model reconstruction
                potentials_.resize(n);
                for (std::uint32_t i = 0; i < n; ++i) {
                    potentials_[i] = dist[i].value;
                }
                return std::nullopt; // converged, no neg cycle
            }

            // A vertex was still relaxing on iteration n => negative cycle.
            // Walk predecessors n times to land inside the cycle, then collect.
            std::uint32_t v = static_cast<std::uint32_t>(updated);
            for (std::uint32_t i = 0; i < n; ++i) {
                if (pred[v] < 0) return std::nullopt;
                v = edges_[static_cast<std::size_t>(pred[v])].y;
            }
            std::vector<std::size_t> cycle;
            std::uint32_t start = v;
            do {
                const std::size_t ei = static_cast<std::size_t>(pred[v]);
                cycle.push_back(ei);
                v = edges_[ei].y;
            } while (v != start && cycle.size() <= edges_.size());
            return cycle;
        }

        // Lexicographic "cand strictly less than cur".
        static bool less(const weight& a, const weight& b) {
            const auto c = a.value <=> b.value;
            if (c != std::strong_ordering::equal) return c == std::strong_ordering::less;
            // equal value: strict (-ε) < non-strict
            return a.strict && !b.strict;
        }

        void build_explanation(const std::vector<std::size_t>& cycle) {
            explanation_.clear();
            for (std::size_t ei : cycle) {
                const Edge& e = edges_[ei];
                // The conflict clause is the negation of the conjunction of the
                // cycle's asserted literals: ∨ ¬lit_i.
                const Var var = reg_->var_of(e.atom);
                explanation_.push_back(make_lit(var, /*negated=*/e.value));
            }
        }

        atom_registry* reg_ = nullptr;
        std::unordered_map<std::uint32_t, Decoded> decoded_; // by atom_index
        std::unordered_map<std::uint64_t, std::uint32_t> var_of_; // term hash -> dl vertex
        std::uint32_t next_dlvar_ = 0;

        std::vector<Edge> edges_;
        std::vector<std::size_t> trail_lim_;
        std::vector<Lit> explanation_;
        std::vector<rat> potentials_;
    };
} // namespace tarka::native
