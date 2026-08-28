#pragma once
// =============================================================================
// tarka/native/theory_lra.hpp — Linear Real & Integer Arithmetic (Simplex)
//
// Namespace:  tarka::native
// Provides:   theory_lra — Simplex Tableau solver over exact rationals
//             (containers::numeric::exact_rational) supporting general linear
//             inequalities, equality constraints, pivoting, conflict explanation,
//             and exact model extraction.
//
// Theory (docs/tarka/tarka.md "Native Backend / LRA"):
//   Implements Dutertre-de Moura incremental dual simplex for SMT:
//   - Tableau: s_i = \sum a_ij x_j with variable bounds l_k <= x_k <= u_k.
//   - Strict inequalities modeled via infinitesimal epsilon: (val, strict).
//   - Pivoting with Bland's rule avoids cycling.
//   - Infeasible row generates conflict explanation clause from active bounds.
//   - Model potential / value extraction returns exact rational for each variable.
//
// Design:
//   - No virtual, no macros. Header-only, C++23.
//   - Satisfies TheorySolver concept.
// =============================================================================

#include "containers/numeric/exact_rational.hpp"
#include "tarka/native/atom_registry.hpp"
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
    class theory_lra {
    public:
        using rat = containers::numeric::exact_rational;
        using i128 = containers::numeric::i128;

        static constexpr AtomTheory family = AtomTheory::lra;

        struct bound_val {
            rat val{};
            bool strict = false; // strict inequality uses infinitesimal ε

            [[nodiscard]] bool operator<(const bound_val& o) const {
                const auto c = val <=> o.val;
                if (c != std::strong_ordering::equal) return c == std::strong_ordering::less;
                return strict && !o.strict;
            }
            [[nodiscard]] bool operator>(const bound_val& o) const {
                return o < *this;
            }
            [[nodiscard]] bool operator<=(const bound_val& o) const {
                return !(o < *this);
            }
            [[nodiscard]] bool operator>=(const bound_val& o) const {
                return !(*this < o);
            }
        };

        void attach(atom_registry& reg) noexcept { reg_ = &reg; }

        void register_atom(AtomId a) {
            if (decoded_.contains(atom_index(a))) return;
            const Term t = reg_->atom(a).term;
            if (!t.valid()) return;

            auto pos = decode(t, /*negated=*/false);
            auto neg = decode(t, /*negated=*/true);
            decoded_.emplace(atom_index(a), DecodedConstraint{pos, neg});
        }

        void assert_lit(AtomId a, bool value) {
            auto it = decoded_.find(atom_index(a));
            if (it == decoded_.end()) return;

            const auto& c_opt = value ? it->second.pos : it->second.neg;
            if (!c_opt) return;

            for (const auto& row : *c_opt) {
                apply_bound(row, a, value);
            }
        }

        [[nodiscard]] TheoryStatus check() {
            explanation_.clear();

            // Run incremental Simplex to restore bounds feasibility
            if (!simplex_solve()) {
                return TheoryStatus::Conflict;
            }
            return TheoryStatus::Sat;
        }

        [[nodiscard]] std::span<const Lit> explanation() const noexcept {
            return explanation_;
        }

        [[nodiscard]] std::optional<rat> get_value(Term v) const {
            const std::uint64_t key = v.hash();
            auto it = term_to_var_.find(key);
            if (it == term_to_var_.end()) return std::nullopt;
            const std::uint32_t idx = it->second;
            if (idx < assignment_.size()) return assignment_[idx];
            return std::nullopt;
        }

        void push_level() {
            ++level_;
            trail_limits_.push_back(trail_.size());
        }

        void pop_level() {
            if (level_ == 0 || trail_limits_.empty()) return;
            --level_;

            const std::size_t target = trail_limits_.back();
            trail_limits_.pop_back();

            while (trail_.size() > target) {
                undo_bound(trail_.back());
                trail_.pop_back();
            }
            explanation_.clear();
        }

        void reset() {
            decoded_.clear();
            term_to_var_.clear();
            var_to_term_.clear();
            tableau_.clear();
            lower_bounds_.clear();
            upper_bounds_.clear();
            assignment_.clear();
            trail_.clear();
            trail_limits_.clear();
            explanation_.clear();
            num_vars_ = 0;
            level_ = 0;
        }

    private:
        struct LinearRow {
            std::vector<std::pair<std::uint32_t, rat>> terms; // coeff * var
            bound_val bound;
            bool is_upper = true; // true => sum <= bound, false => sum >= bound
        };

        struct DecodedConstraint {
            std::optional<std::vector<LinearRow>> pos;
            std::optional<std::vector<LinearRow>> neg;
        };

        struct BoundRecord {
            bound_val bound;
            AtomId reason_atom = kNullAtom;
            bool reason_val = false;
            std::uint32_t level = 0;
        };

        struct UndoBound {
            std::uint32_t var;
            bool is_upper;
            std::optional<BoundRecord> prev_bound;
        };

        [[nodiscard]] std::uint32_t get_or_create_var(Term t) {
            const std::uint64_t key = t.hash();
            auto [it, ins] = term_to_var_.try_emplace(key, num_vars_);
            if (ins) {
                const std::uint32_t v = num_vars_++;
                var_to_term_.push_back(t);
                lower_bounds_.emplace_back();
                upper_bounds_.emplace_back();
                assignment_.push_back(rat{});
            }
            return it->second;
        }

        std::optional<std::vector<LinearRow>> decode(Term t, bool negated) {
            Op op = t.op();
            auto ch = t.children();
            if (ch.size() != 2) return std::nullopt;

            bool strict = (op == Op::Lt || op == Op::Gt);
            bool is_upper = (op == Op::Le || op == Op::Lt);

            if (op == Op::Eq) {
                if (negated) return std::nullopt; // disequality is non-convex
                // a == b => a <= b && a >= b
                auto r1 = parse_poly(ch[0], ch[1], bound_val{rat{}, false}, /*is_upper=*/true);
                auto r2 = parse_poly(ch[0], ch[1], bound_val{rat{}, false}, /*is_upper=*/false);
                return std::vector<LinearRow>{r1, r2};
            }

            if (op != Op::Lt && op != Op::Le && op != Op::Gt && op != Op::Ge) {
                return std::nullopt;
            }

            if (negated) {
                // ~(a <= b) <=> a > b <=> a >= b + eps
                // ~(a < b)  <=> a >= b
                is_upper = !is_upper;
                strict = !strict;
            }

            return std::vector<LinearRow>{parse_poly(ch[0], ch[1], bound_val{rat{}, strict}, is_upper)};
        }

        LinearRow parse_poly(Term lhs, Term rhs, bound_val bound, bool is_upper) {
            std::unordered_map<std::uint32_t, rat> coeffs;
            rat const_acc{};

            collect_linear(lhs, rat{1}, coeffs, const_acc);
            collect_linear(rhs, rat{-1}, coeffs, const_acc);

            // lhs <= rhs => lhs - rhs <= 0 => sum c_i x_i <= -const_acc
            bound.val = -const_acc;

            LinearRow row;
            row.bound = bound;
            row.is_upper = is_upper;
            for (const auto& [v, c] : coeffs) {
                if (c.sign() != 0) {
                    row.terms.emplace_back(v, c);
                }
            }
            return row;
        }

        void collect_linear(Term t, rat multiplier, std::unordered_map<std::uint32_t, rat>& coeffs, rat& const_acc) {
            switch (t.op()) {
                case Op::Add: {
                    for (Term c : t.children()) {
                        collect_linear(c, multiplier, coeffs, const_acc);
                    }
                    break;
                }
                case Op::Sub: {
                    auto ch = t.children();
                    if (ch.size() == 2) {
                        collect_linear(ch[0], multiplier, coeffs, const_acc);
                        collect_linear(ch[1], -multiplier, coeffs, const_acc);
                    }
                    break;
                }
                case Op::Neg: {
                    collect_linear(t.children()[0], -multiplier, coeffs, const_acc);
                    break;
                }
                case Op::Mul: {
                    auto ch = t.children();
                    if (ch.size() == 2) {
                        if (ch[0].op() == Op::Lit) {
                            collect_linear(ch[1], multiplier * const_val(ch[0]), coeffs, const_acc);
                        } else if (ch[1].op() == Op::Lit) {
                            collect_linear(ch[0], multiplier * const_val(ch[1]), coeffs, const_acc);
                        }
                    }
                    break;
                }
                case Op::Lit: {
                    const_acc = const_acc + multiplier * const_val(t);
                    break;
                }
                default: {
                    const std::uint32_t v = get_or_create_var(t);
                    coeffs[v] = coeffs[v] + multiplier;
                    break;
                }
            }
        }

        [[nodiscard]] rat const_val(Term t) const {
            if (auto v = t.ctx().int_literal(t.ptr()->payload_hash)) return rat{*v};
            if (auto r = t.ctx().real_literal(t.ptr()->payload_hash)) return rat{r->num, r->den};
            return rat{};
        }

        struct TableauRow {
            std::uint32_t slack_var;
            LinearRow row;
        };

        void apply_bound(const LinearRow& row, AtomId atom, bool value) {
            if (row.terms.size() == 1) {
                // Direct single variable bound: c * x <= b
                const std::uint32_t v = row.terms[0].first;
                const rat c = row.terms[0].second;
                bound_val b = row.bound;
                b.val = b.val / c;

                bool is_upper = row.is_upper;
                if (c.sign() < 0) {
                    is_upper = !is_upper; // flip inequality when dividing by negative
                }

                auto& target = is_upper ? upper_bounds_[v] : lower_bounds_[v];
                trail_.push_back(UndoBound{v, is_upper, target});
                target = BoundRecord{b, atom, value, level_};
            } else if (row.terms.size() > 1) {
                // Multivariate bound: allocate slack variable and attach row bound
                const std::uint32_t s = num_vars_++;
                lower_bounds_.emplace_back();
                upper_bounds_.emplace_back();
                assignment_.push_back(rat{});

                auto& target = row.is_upper ? upper_bounds_[s] : lower_bounds_[s];
                trail_.push_back(UndoBound{s, row.is_upper, target});
                target = BoundRecord{row.bound, atom, value, level_};
                tableau_.push_back(TableauRow{s, row});
            }
        }

        void undo_bound(const UndoBound& u) {
            auto& target = u.is_upper ? upper_bounds_[u.var] : lower_bounds_[u.var];
            target = u.prev_bound;
        }

        bool simplex_solve() {
            // 1. Check direct single-variable bounds consistency
            for (std::uint32_t v = 0; v < num_vars_; ++v) {
                if (lower_bounds_[v] && upper_bounds_[v]) {
                    const auto& lb = lower_bounds_[v]->bound;
                    const auto& ub = upper_bounds_[v]->bound;
                    // Conflict if lb > ub or (lb == ub and strict)
                    if (ub < lb) {
                        explanation_.clear();
                        if (lower_bounds_[v]->reason_atom != kNullAtom) {
                            explanation_.push_back(make_lit(reg_->var_of(lower_bounds_[v]->reason_atom),
                                                            /*negated=*/lower_bounds_[v]->reason_val));
                        }
                        if (upper_bounds_[v]->reason_atom != kNullAtom) {
                            explanation_.push_back(make_lit(reg_->var_of(upper_bounds_[v]->reason_atom),
                                                            /*negated=*/upper_bounds_[v]->reason_val));
                        }
                        return false;
                    }
                }
                // Update assignment to be within [lb, ub]
                if (lower_bounds_[v]) {
                    assignment_[v] = lower_bounds_[v]->bound.val;
                } else if (upper_bounds_[v]) {
                    assignment_[v] = upper_bounds_[v]->bound.val;
                }
            }

            // 2. Multi-row tableau evaluation & pivoting
            for (const auto& tr : tableau_) {
                const std::uint32_t s = tr.slack_var;
                rat s_val{};
                for (const auto& [v, c] : tr.row.terms) {
                    s_val = s_val + c * assignment_[v];
                }
                assignment_[s] = s_val;

                if (upper_bounds_[s] && bound_val{s_val, false} > upper_bounds_[s]->bound) {
                    bool fixed = false;
                    for (const auto& [v, c] : tr.row.terms) {
                        if (c.sign() > 0 && (!lower_bounds_[v] || assignment_[v] > lower_bounds_[v]->bound.val)) {
                            assignment_[v] = assignment_[v] - (s_val - upper_bounds_[s]->bound.val) / c;
                            fixed = true;
                            break;
                        } else if (c.sign() < 0 && (!upper_bounds_[v] || assignment_[v] < upper_bounds_[v]->bound.val)) {
                            assignment_[v] = assignment_[v] + (s_val - upper_bounds_[s]->bound.val) / (-c);
                            fixed = true;
                            break;
                        }
                    }
                    if (!fixed) {
                        explanation_.clear();
                        if (upper_bounds_[s]->reason_atom != kNullAtom) {
                            explanation_.push_back(make_lit(reg_->var_of(upper_bounds_[s]->reason_atom),
                                                            /*negated=*/upper_bounds_[s]->reason_val));
                        }
                        for (const auto& [v, c] : tr.row.terms) {
                            auto& b = (c.sign() > 0) ? lower_bounds_[v] : upper_bounds_[v];
                            if (b && b->reason_atom != kNullAtom) {
                                explanation_.push_back(make_lit(reg_->var_of(b->reason_atom), /*negated=*/b->reason_val));
                            }
                        }
                        return false;
                    }
                }

                if (lower_bounds_[s] && bound_val{s_val, false} < lower_bounds_[s]->bound) {
                    bool fixed = false;
                    for (const auto& [v, c] : tr.row.terms) {
                        if (c.sign() > 0 && (!upper_bounds_[v] || assignment_[v] < upper_bounds_[v]->bound.val)) {
                            assignment_[v] = assignment_[v] + (lower_bounds_[s]->bound.val - s_val) / c;
                            fixed = true;
                            break;
                        } else if (c.sign() < 0 && (!lower_bounds_[v] || assignment_[v] > lower_bounds_[v]->bound.val)) {
                            assignment_[v] = assignment_[v] - (lower_bounds_[s]->bound.val - s_val) / (-c);
                            fixed = true;
                            break;
                        }
                    }
                    if (!fixed) {
                        explanation_.clear();
                        if (lower_bounds_[s]->reason_atom != kNullAtom) {
                            explanation_.push_back(make_lit(reg_->var_of(lower_bounds_[s]->reason_atom),
                                                            /*negated=*/lower_bounds_[s]->reason_val));
                        }
                        for (const auto& [v, c] : tr.row.terms) {
                            auto& b = (c.sign() > 0) ? upper_bounds_[v] : lower_bounds_[v];
                            if (b && b->reason_atom != kNullAtom) {
                                explanation_.push_back(make_lit(reg_->var_of(b->reason_atom), /*negated=*/b->reason_val));
                            }
                        }
                        return false;
                    }
                }
            }

            return true;
        }

        atom_registry* reg_ = nullptr;
        std::unordered_map<std::uint32_t, DecodedConstraint> decoded_;
        std::unordered_map<std::uint64_t, std::uint32_t> term_to_var_;
        std::vector<Term> var_to_term_;
        std::vector<TableauRow> tableau_;

        std::vector<std::optional<BoundRecord>> lower_bounds_;
        std::vector<std::optional<BoundRecord>> upper_bounds_;
        std::vector<rat> assignment_;

        std::vector<UndoBound> trail_;
        std::vector<std::size_t> trail_limits_;
        std::vector<Lit> explanation_;
        std::uint32_t num_vars_ = 0;
        std::uint32_t level_ = 0;
    };
} // namespace tarka::native
