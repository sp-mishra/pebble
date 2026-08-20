#pragma once
// =============================================================================
// tarka/native/cdcl_solver.hpp — CDCL propositional SAT core
//
// Namespace:  tarka::native
// Provides:   cdcl_solver — modern conflict-driven clause-learning SAT engine.
//
// Algorithms (theory in docs/tarka/tarka.md, "Native Backend"):
//   - Two-watched-literal unit propagation (BCP).
//   - 1UIP conflict analysis with clause learning + non-chronological backjump.
//   - VSIDS branching (activity bump on conflict, geometric decay).
//   - Luby restarts + phase saving.
//
// Design:
//   - No virtual, no macros. Clause DB is a flat arena of literals; ClauseRef
//     indexes it. Watch lists are per-literal vectors of ClauseRef.
//   - Theory hook (optional): a caller-supplied callable invoked at fixpoint to
//     let a theory solver check consistency / propagate. The SAT core stays a
//     pure DPLL engine when no hook is set — pay only for what you use.
// =============================================================================

#include "tarka/native/ids.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <span>
#include <utility>
#include <vector>

namespace tarka::native {
    // =========================================================================
    // Value of a variable
    // =========================================================================

    enum class LBool : std::uint8_t { False = 0, True = 1, Undef = 2 };

    [[nodiscard]] constexpr LBool lbool_from_sign(bool negated) noexcept {
        return negated ? LBool::False : LBool::True;
    }

    // =========================================================================
    // Clause — stored in a flat literal arena
    // =========================================================================

    struct ClauseHeader {
        std::uint32_t size;
        std::uint32_t offset; // start index in lits_ arena
        bool learnt = false;
        bool deleted = false;
        std::uint16_t lbd = 0;
        float activity = 0.0f;
    };

    // Result of a theory check at a propositional fixpoint.
    enum class TheoryCheckResult : std::uint8_t {
        Consistent, // model is theory-consistent; keep going
        Conflict, // theory produced a conflict clause (added via add_learnt_conflict)
        Propagated // theory added lemma clauses / literals; re-run BCP
    };

    // =========================================================================
    // cdcl_solver
    // =========================================================================

    class cdcl_solver {
    public:
        cdcl_solver() = default;

        // Theory hook: called when BCP reaches a fixpoint with no conflict.
        // The callback may push conflict/lemma clauses via the provided handle
        // (see theory_interface below) and returns a TheoryCheckResult.
        using theory_hook_t = std::function<TheoryCheckResult(cdcl_solver&)>;
        // Called on assignment of each literal so theories can track the trail.
        using assign_hook_t = std::function<void(Lit)>;

        void set_theory_hook(theory_hook_t h) { theory_hook_ = std::move(h); }
        void set_assign_hook(assign_hook_t h) { assign_hook_ = std::move(h); }

        // ---------------------------------------------------------------------
        // Variable / clause construction
        // ---------------------------------------------------------------------

        [[nodiscard]] Var new_var() {
            const std::uint32_t idx = static_cast<std::uint32_t>(assign_.size());
            assign_.push_back(LBool::Undef);
            reason_.push_back(kNullClause);
            level_.push_back(0);
            activity_.push_back(0.0);
            phase_.push_back(false);
            watches_.emplace_back();
            watches_.emplace_back();
            return Var{idx};
        }

        [[nodiscard]] std::size_t num_vars() const noexcept { return assign_.size(); }

        void ensure_var(Var v) {
            while (assign_.size() <= var_index(v)) (void)new_var();
        }

        // Add a clause (list of literals). Returns false if the formula becomes
        // trivially UNSAT (empty clause or conflicting unit at level 0).
        bool add_clause(std::span<const Lit> lits) {
            // Copy + dedup + tautology check
            tmp_.assign(lits.begin(), lits.end());
            std::sort(tmp_.begin(), tmp_.end(),
                      [](Lit a, Lit b) { return lit_index(a) < lit_index(b); });
            std::size_t j = 0;
            for (std::size_t i = 0; i < tmp_.size(); ++i) {
                if (i + 1 < tmp_.size() && tmp_[i] == lit_neg(tmp_[i + 1])) return true; // tautology
                if (j == 0 || tmp_[j - 1] != tmp_[i]) tmp_[j++] = tmp_[i];
            }
            tmp_.resize(j);

            if (tmp_.empty()) { unsat_ = true; return false; }
            if (tmp_.size() == 1) {
                if (!enqueue(tmp_[0], kNullClause)) { unsat_ = true; return false; }
                return true;
            }

            const ClauseRef cr = alloc_clause(tmp_, /*learnt=*/false);
            attach_watches(cr);
            return true;
        }

        // Convenience overloads
        bool add_clause(std::initializer_list<Lit> lits) {
            return add_clause(std::span<const Lit>{lits.begin(), lits.size()});
        }

        // Theory API: add a conflict/lemma clause during the theory hook.
        // Returns the new clause ref (or kNullClause for units handled inline).
        void add_theory_clause(std::span<const Lit> lits) {
            pending_theory_.emplace_back(lits.begin(), lits.end());
        }

        // ---------------------------------------------------------------------
        // Solve
        // ---------------------------------------------------------------------

        // stop: optional cancellation predicate (returns true → abort as Undef).
        [[nodiscard]] LBool solve(const std::function<bool()>& stop = {}) {
            if (unsat_) return LBool::False;

            std::uint64_t conflicts = 0;
            std::uint64_t restart_limit = luby(++restart_count_) * kRestartUnit;

            for (;;) {
                if (stop && stop()) return LBool::Undef;

                const ClauseRef confl = propagate();
                if (confl != kNullClause) {
                    ++conflicts;
                    if (decision_level() == 0) return LBool::False;
                    analyze_and_backjump(confl);
                    decay_activity();
                    if (conflicts % 2000 == 0) {
                        reduce_db();
                    }
                    if (conflicts >= restart_limit) {
                        conflicts = 0;
                        restart_limit = luby(++restart_count_) * kRestartUnit;
                        backjump_to(0);
                    }
                    continue;
                }

                // BCP fixpoint reached — invoke theory hook if present.
                if (theory_hook_) {
                    const TheoryCheckResult tr = run_theory();
                    if (tr == TheoryCheckResult::Propagated) continue;
                    if (tr == TheoryCheckResult::Conflict) {
                        if (!flush_theory_clauses()) return LBool::False;
                        continue;
                    }
                    // Consistent → fall through to decide
                }

                if (!decide()) return LBool::True; // all vars assigned, consistent
            }
        }

        // ---------------------------------------------------------------------
        // Model queries
        // ---------------------------------------------------------------------

        [[nodiscard]] LBool value(Var v) const noexcept {
            return var_index(v) < assign_.size() ? assign_[var_index(v)] : LBool::Undef;
        }

        [[nodiscard]] LBool value(Lit l) const noexcept {
            const LBool v = value(lit_var(l));
            if (v == LBool::Undef) return LBool::Undef;
            const bool truth = (v == LBool::True);
            return (truth != lit_sign(l)) ? LBool::True : LBool::False;
        }

        [[nodiscard]] bool is_true(Lit l) const noexcept { return value(l) == LBool::True; }

        [[nodiscard]] std::uint32_t decision_level() const noexcept {
            return static_cast<std::uint32_t>(trail_lim_.size());
        }

        // ---------------------------------------------------------------------
        // Push/pop for incremental solving (assumption scopes)
        // ---------------------------------------------------------------------

        void reset() {
            assign_.clear(); reason_.clear(); level_.clear(); activity_.clear();
            phase_.clear(); watches_.clear(); clauses_.clear(); lits_.clear();
            trail_.clear(); trail_lim_.clear(); pending_theory_.clear();
            qhead_ = 0; unsat_ = false; restart_count_ = 0; var_inc_ = 1.0;
        }

    private:
        // ---- data -----------------------------------------------------------
        std::vector<LBool> assign_;
        std::vector<ClauseRef> reason_;
        std::vector<std::uint32_t> level_;
        std::vector<double> activity_;
        std::vector<bool> phase_;
        std::vector<std::vector<ClauseRef>> watches_; // indexed by lit_index

        std::vector<ClauseHeader> clauses_;
        std::vector<Lit> lits_; // flat clause literal arena

        std::vector<Lit> trail_;
        std::vector<std::uint32_t> trail_lim_;
        std::size_t qhead_ = 0;

        std::vector<std::vector<Lit>> pending_theory_;
        theory_hook_t theory_hook_;
        assign_hook_t assign_hook_;

        bool unsat_ = false;
        double var_inc_ = 1.0;
        double var_decay_ = 0.95;
        std::uint64_t restart_count_ = 0;
        static constexpr std::uint64_t kRestartUnit = 100;

        std::vector<Lit> tmp_;
        std::vector<Lit> analyze_tmp_;
        std::vector<bool> seen_;

        // ---- clause storage --------------------------------------------------

        [[nodiscard]] std::uint32_t compute_lbd(const std::vector<Lit>& lits) const noexcept {
            std::uint64_t seen_mask = 0;
            std::uint32_t count = 0;
            for (Lit l : lits) {
                const std::uint32_t lv = level_[var_index(lit_var(l))];
                if (lv < 64) {
                    if (!(seen_mask & (1ULL << lv))) {
                        seen_mask |= (1ULL << lv);
                        ++count;
                    }
                } else {
                    ++count;
                }
            }
            return count == 0 ? 1 : count;
        }

        [[nodiscard]] ClauseRef alloc_clause(const std::vector<Lit>& lits, bool learnt) {
            ClauseHeader h;
            h.size = static_cast<std::uint32_t>(lits.size());
            h.offset = static_cast<std::uint32_t>(lits_.size());
            h.learnt = learnt;
            h.deleted = false;
            h.lbd = learnt ? static_cast<std::uint16_t>(compute_lbd(lits)) : 0;
            h.activity = 0.0f;
            const ClauseRef cr{static_cast<std::uint32_t>(clauses_.size())};
            clauses_.push_back(h);
            lits_.insert(lits_.end(), lits.begin(), lits.end());
            return cr;
        }

        void reduce_db() {
            std::vector<ClauseRef> candidates;
            for (std::size_t ci = 0; ci < clauses_.size(); ++ci) {
                const ClauseHeader& h = clauses_[ci];
                if (!h.deleted && h.learnt && h.size > 2 && h.lbd > 2) {
                    candidates.push_back(ClauseRef{static_cast<std::uint32_t>(ci)});
                }
            }
            if (candidates.size() <= 100) return;
            std::sort(candidates.begin(), candidates.end(), [&](ClauseRef a, ClauseRef b) {
                return clauses_[clause_index(a)].activity < clauses_[clause_index(b)].activity;
            });
            const std::size_t remove_count = candidates.size() / 2;
            for (std::size_t k = 0; k < remove_count; ++k) {
                const ClauseRef cr = candidates[k];
                const auto ls = clause_lits(cr);
                const Var v = lit_var(ls[0]);
                if (reason_[var_index(v)] == cr) continue;
                clauses_[clause_index(cr)].deleted = true;
            }
        }

        [[nodiscard]] std::span<Lit> clause_lits(ClauseRef cr) {
            const ClauseHeader& h = clauses_[clause_index(cr)];
            return {lits_.data() + h.offset, h.size};
        }

        [[nodiscard]] std::span<const Lit> clause_lits(ClauseRef cr) const {
            const ClauseHeader& h = clauses_[clause_index(cr)];
            return {lits_.data() + h.offset, h.size};
        }

        void attach_watches(ClauseRef cr) {
            auto ls = clause_lits(cr);
            watches_[lit_index(ls[0])].push_back(cr);
            watches_[lit_index(ls[1])].push_back(cr);
        }

        // ---- assignment / trail ---------------------------------------------

        [[nodiscard]] bool enqueue(Lit l, ClauseRef reason) {
            const LBool v = value(l);
            if (v == LBool::True) return true;
            if (v == LBool::False) return false; // conflict
            const std::uint32_t vi = var_index(lit_var(l));
            assign_[vi] = lbool_from_sign(lit_sign(l));
            reason_[vi] = reason;
            level_[vi] = decision_level();
            trail_.push_back(l);
            if (assign_hook_) assign_hook_(l);
            return true;
        }

        void new_decision_level() { trail_lim_.push_back(static_cast<std::uint32_t>(trail_.size())); }

        void backjump_to(std::uint32_t lvl) {
            if (decision_level() <= lvl) return;
            const std::size_t target = trail_lim_[lvl];
            for (std::size_t i = trail_.size(); i-- > target;) {
                const Var v = lit_var(trail_[i]);
                phase_[var_index(v)] = (assign_[var_index(v)] == LBool::True);
                assign_[var_index(v)] = LBool::Undef;
                reason_[var_index(v)] = kNullClause;
            }
            trail_.resize(target);
            trail_lim_.resize(lvl);
            qhead_ = target;
        }

        // ---- BCP ------------------------------------------------------------

        [[nodiscard]] ClauseRef propagate() {
            while (qhead_ < trail_.size()) {
                const Lit p = trail_[qhead_++];
                const Lit falsified = lit_neg(p);
                auto& ws = watches_[lit_index(falsified)];

                std::size_t i = 0, j = 0;
                while (i < ws.size()) {
                    const ClauseRef cr = ws[i++];
                    if (clauses_[clause_index(cr)].deleted) continue;
                    auto ls = clause_lits(cr);
                    // ensure ls[1] is the falsified watch
                    if (ls[0] == falsified) std::swap(ls[0], ls[1]);
                    // if ls[0] true, clause satisfied — keep watch
                    if (value(ls[0]) == LBool::True) { ws[j++] = cr; continue; }
                    // look for a new watch among ls[2..]
                    bool moved = false;
                    for (std::size_t k = 2; k < ls.size(); ++k) {
                        if (value(ls[k]) != LBool::False) {
                            std::swap(ls[1], ls[k]);
                            watches_[lit_index(ls[1])].push_back(cr);
                            moved = true;
                            break;
                        }
                    }
                    if (moved) continue;
                    // clause is unit or conflicting under ls[0]
                    ws[j++] = cr;
                    if (!enqueue(ls[0], cr)) {
                        // conflict — copy back rest of watch list and return
                        while (i < ws.size()) ws[j++] = ws[i++];
                        ws.resize(j);
                        return cr;
                    }
                }
                ws.resize(j);
            }
            return kNullClause;
        }

        // ---- 1UIP conflict analysis -----------------------------------------

        void analyze_and_backjump(ClauseRef confl) {
            analyze_tmp_.clear();
            seen_.assign(num_vars(), false);
            analyze_tmp_.push_back(kNullLit); // placeholder for asserting literal

            std::uint32_t path_count = 0;
            std::size_t idx = trail_.size();
            Lit p = kNullLit;
            ClauseRef reason = confl;

            do {
                auto ls = clause_lits(reason);
                for (Lit q : ls) {
                    if (p != kNullLit && lit_var(q) == lit_var(p)) continue; // skip resolved pivot
                    const Var v = lit_var(q);
                    if (!seen_[var_index(v)] && level_[var_index(v)] > 0) {
                        seen_[var_index(v)] = true;
                        bump_var(v);
                        if (level_[var_index(v)] >= decision_level()) {
                            ++path_count;
                        } else {
                            analyze_tmp_.push_back(q);
                        }
                    }
                }
                // pick next literal to resolve from the trail
                while (idx > 0 && !seen_[var_index(lit_var(trail_[--idx]))]) {}
                p = trail_[idx];
                reason = reason_[var_index(lit_var(p))];
                seen_[var_index(lit_var(p))] = false;
                --path_count;
            } while (path_count > 0);

            // asserting literal is ¬p (the UIP)
            analyze_tmp_[0] = lit_neg(p);

            // backjump level = second-highest level in the learnt clause
            std::uint32_t bj = 0;
            std::size_t max_i = 1;
            for (std::size_t i = 1; i < analyze_tmp_.size(); ++i) {
                const std::uint32_t lv = level_[var_index(lit_var(analyze_tmp_[i]))];
                if (lv > bj) { bj = lv; max_i = i; }
            }
            if (analyze_tmp_.size() > 1) std::swap(analyze_tmp_[1], analyze_tmp_[max_i]);

            backjump_to(bj);

            // asserting literal is unassigned after backjump — enqueue cannot conflict
            if (analyze_tmp_.size() == 1) {
                (void)enqueue(analyze_tmp_[0], kNullClause);
            } else {
                const ClauseRef cr = alloc_clause(analyze_tmp_, /*learnt=*/true);
                attach_watches(cr);
                (void)enqueue(analyze_tmp_[0], cr);
            }
        }

        // ---- VSIDS ----------------------------------------------------------

        void bump_var(Var v) {
            activity_[var_index(v)] += var_inc_;
            if (activity_[var_index(v)] > 1e100) {
                for (double& a : activity_) a *= 1e-100;
                var_inc_ *= 1e-100;
            }
        }

        void decay_activity() { var_inc_ /= var_decay_; }

        // ---- decisions ------------------------------------------------------

        [[nodiscard]] bool decide() {
            Var best = kNullVar;
            double best_act = -1.0;
            for (std::uint32_t i = 0; i < assign_.size(); ++i) {
                if (assign_[i] == LBool::Undef && activity_[i] > best_act) {
                    best_act = activity_[i];
                    best = Var{i};
                }
            }
            if (best == kNullVar) return false; // complete assignment
            new_decision_level();
            const bool neg = phase_[var_index(best)]; // phase saving
            (void)enqueue(make_lit(best, neg), kNullClause); // best is Undef → no conflict
            return true;
        }

        // ---- theory integration ---------------------------------------------

        [[nodiscard]] TheoryCheckResult run_theory() {
            pending_theory_.clear();
            return theory_hook_(*this);
        }

        [[nodiscard]] bool handle_theory_conflict(std::span<const Lit> c) {
            if (c.empty()) {
                unsat_ = true;
                return false;
            }

            // Check if all literals are false at level 0
            bool all_zero = true;
            for (Lit l : c) {
                if (level_[var_index(lit_var(l))] > 0) {
                    all_zero = false;
                    break;
                }
            }
            if (all_zero || decision_level() == 0) {
                unsat_ = true;
                return false;
            }

            if (c.size() == 1) {
                backjump_to(0);
                if (!enqueue(c[0], kNullClause)) {
                    unsat_ = true;
                    return false;
                }
                return true;
            }

            std::vector<Lit> cl(c.begin(), c.end());
            const ClauseRef cr = alloc_clause(cl, /*learnt=*/true);
            attach_watches(cr);
            analyze_and_backjump(cr);
            decay_activity();
            return true;
        }

        [[nodiscard]] bool flush_theory_clauses() {
            for (const auto& c : pending_theory_) {
                if (!handle_theory_conflict(std::span<const Lit>{c.data(), c.size()})) {
                    return false;
                }
            }
            pending_theory_.clear();
            return true;
        }

        // ---- Luby restart sequence ------------------------------------------

        [[nodiscard]] static std::uint64_t luby(std::uint64_t i) {
            std::uint64_t k = 1;
            while (true) {
                if (i == (k << 1u) - 1u) return k;
                if (i < (k << 1u) - 1u) { i = i - ((k) - 1u); k = 1; continue; }
                k <<= 1u;
            }
        }
    };
} // namespace tarka::native
