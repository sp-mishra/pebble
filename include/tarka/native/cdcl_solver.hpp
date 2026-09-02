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
#include "containers/associative/SparseSet.hpp"
#include "containers/associative/order_heap.hpp"
#include "containers/dynamic/SmallVector.hpp"

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
        std::uint8_t tier = 0; // 0=core (kept), 1=mid, 2=local (first to be dropped)
        bool used = false; // touched as a reason since last reduce — protects one round
    };

    // Watch-list entry: the watched clause plus a *blocking literal* — a cached
    // literal from the clause. In BCP, if the blocker is already true the clause
    // is satisfied and we skip the arena dereference entirely (MiniSat's blocker
    // optimization). Cuts cache misses on the hot propagate loop.
    struct Watch {
        ClauseRef cr;
        Lit blocker;
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
            // Keep the decision heap complete if it is already live.
            if (heap_built_) order_heap_.insert(idx);
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

            if (tmp_.empty()) {
                unsat_ = true;
                return false;
            }
            if (tmp_.size() == 1) {
                if (!enqueue(tmp_[0], kNullClause)) {
                    unsat_ = true;
                    return false;
                }
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
                    if (conflicts >= restart_limit || should_restart_ema()) {
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

        [[nodiscard]] LBool solve_assuming(std::span<const Lit> assumptions, const std::function<bool()>& stop = {}) {
            if (unsat_) return LBool::False;
            unsat_core_.clear();

            for (Lit a : assumptions) {
                if (value(a) == LBool::False) {
                    unsat_core_.push_back(a);
                    return LBool::False;
                }
                if (value(a) == LBool::Undef) {
                    new_decision_level();
                    if (!enqueue(a, kNullClause)) {
                        unsat_core_.push_back(a);
                        return LBool::False;
                    }
                    ClauseRef confl = propagate();
                    if (confl != kNullClause) {
                        unsat_core_.push_back(a);
                        return LBool::False;
                    }
                }
            }

            const LBool res = solve(stop);
            if (res == LBool::False && unsat_core_.empty()) {
                for (Lit a : assumptions) {
                    if (value(a) == LBool::True) unsat_core_.push_back(a);
                }
            }
            return res;
        }

        [[nodiscard]] std::span<const Lit> unsat_core() const noexcept {
            return unsat_core_;
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
            assign_.clear();
            reason_.clear();
            level_.clear();
            activity_.clear();
            phase_.clear();
            watches_.clear();
            clauses_.clear();
            lits_.clear();
            trail_.clear();
            trail_lim_.clear();
            pending_theory_.clear();
            unsat_core_.clear();
            qhead_ = 0;
            unsat_ = false;
            restart_count_ = 0;
            var_inc_ = 1.0;
            order_heap_.clear();
            heap_built_ = false;
            lbd_ema_fast_ = lbd_ema_slow_ = 0.0;
            lbd_ema_count_ = 0;
        }

    private:
        // VSIDS activity comparator over variable indices: a sits above b when
        // it has strictly higher activity. Holds a pointer to the live activity_
        // vector so priorities update in place while vars sit in the heap.
        struct activity_order {
            const std::vector<double>* act = nullptr;

            [[nodiscard]] bool operator()(std::uint32_t a, std::uint32_t b) const noexcept {
                return (*act)[a] > (*act)[b];
            }
        };

        // ---- data -----------------------------------------------------------
        std::vector<LBool> assign_;
        std::vector<ClauseRef> reason_;
        std::vector<std::uint32_t> level_;
        std::vector<double> activity_;
        std::vector<bool> phase_;
        std::vector<containers::dynamic::SmallVector<Watch, 6 * sizeof(Watch)>> watches_; // indexed by lit_index

        // O(log V) decision heap over unassigned vars, ordered by activity_.
        // Populated lazily on first decide(); a linear scan handles the tiny-var
        // case where heap upkeep would not pay off (kHeapThreshold).
        containers::associative::order_heap<activity_order> order_heap_{activity_order{&activity_}};
        bool heap_built_ = false;
        static constexpr std::size_t kHeapThreshold = 32;

        std::vector<ClauseHeader> clauses_;
        std::vector<Lit> lits_; // flat clause literal arena
        std::vector<Lit> unsat_core_;

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
        sparseset::SparseSet<std::uint32_t> seen_set_;
        mutable sparseset::SparseSet<std::uint32_t> lbd_levels_; // scratch for compute_lbd

        // ---- restart control: adaptive LBD-EMA (Glucose) with Luby fallback ---
        // fast/slow EMAs of learnt-clause LBD. Restart when the recent average
        // quality (fast) is markedly worse than the long-run average (slow),
        // i.e. fast > slow * kRestartMargin. Falls back to the Luby schedule so
        // behavior is unchanged until enough conflicts accrue to seed the EMAs.
        double lbd_ema_fast_ = 0.0;
        double lbd_ema_slow_ = 0.0;
        std::uint64_t lbd_ema_count_ = 0;
        static constexpr double kEmaFastAlpha = 1.0 / 32.0;
        static constexpr double kEmaSlowAlpha = 1.0 / 4096.0;
        static constexpr double kRestartMargin = 1.25;

        // ---- chronological backtracking (Nadel-Ryvchin) -----------------------
        // When a conflict's backjump would discard many levels, chronological BT
        // (jump one level) can preserve useful work. Gated behind a threshold so
        // the default path stays pure non-chronological CDCL until the gap is big.
        static constexpr std::uint32_t kChronoThreshold = 100;

        // ---- clause storage --------------------------------------------------

        // LBD = number of distinct decision levels among a clause's literals
        // (Glucose's clause-quality metric). A SparseSet over levels removes the
        // old 64-level ceiling — deep search trees (level > 63) now score exactly
        // instead of over-counting every high level as unique.
        [[nodiscard]] std::uint32_t compute_lbd(const std::vector<Lit>& lits) const noexcept {
            lbd_levels_.reserve(assign_.size() + 1);
            lbd_levels_.clear();
            std::uint32_t count = 0;
            for (Lit l : lits) {
                const std::uint32_t lv = level_[var_index(lit_var(l))];
                if (!lbd_levels_.contains(lv)) {
                    lbd_levels_.insert_or_update(lv);
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
            // Glucose 3-tier clause DB: LBD≤2 = "core" (glue, never dropped),
            // LBD≤6 = "mid", else "local" (dropped first). Originals sit in core.
            h.tier = !learnt ? 0 : (h.lbd <= 2 ? 0 : (h.lbd <= 6 ? 1 : 2));
            h.used = false;
            const ClauseRef cr{static_cast<std::uint32_t>(clauses_.size())};
            clauses_.push_back(h);
            lits_.insert(lits_.end(), lits.begin(), lits.end());
            return cr;
        }

        void reduce_db() {
            // Only "local" learnts (tier 2) are eviction candidates. Glue (tier 0)
            // and mid (tier 1) clauses are kept — Glucose keeps high-quality
            // learnts across reductions. A clause used as a reason since the last
            // sweep gets a one-round reprieve (its `used` flag), then must re-earn
            // it. Reasons of the current trail are never dropped.
            std::vector<ClauseRef> candidates;
            for (std::size_t ci = 0; ci < clauses_.size(); ++ci) {
                ClauseHeader& h = clauses_[ci];
                if (h.deleted || !h.learnt || h.tier != 2 || h.size <= 2) continue;
                if (h.used) {
                    h.used = false;
                    continue;
                } // reprieve one round
                candidates.push_back(ClauseRef{static_cast<std::uint32_t>(ci)});
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

        // Compact the literal arena, dropping deleted clauses and rebuilding
        // watch lists. Reclaims memory after many reductions; O(clauses+lits).
        // Optional — never called on the default path (opt-in maintenance).
        void compact_db() {
            std::vector<Lit> new_lits;
            new_lits.reserve(lits_.size());
            std::vector<ClauseHeader> new_clauses;
            new_clauses.reserve(clauses_.size());
            std::vector<std::uint32_t> remap(clauses_.size(), clause_index(kNullClause));

            for (std::size_t ci = 0; ci < clauses_.size(); ++ci) {
                ClauseHeader h = clauses_[ci];
                if (h.deleted) continue;
                const auto ls = clause_lits(ClauseRef{static_cast<std::uint32_t>(ci)});
                const std::uint32_t new_off = static_cast<std::uint32_t>(new_lits.size());
                new_lits.insert(new_lits.end(), ls.begin(), ls.end());
                h.offset = new_off;
                remap[ci] = static_cast<std::uint32_t>(new_clauses.size());
                new_clauses.push_back(h);
            }

            // Fix reasons that referenced surviving clauses.
            const std::uint32_t null_idx = clause_index(kNullClause);
            for (std::uint32_t vi = 0; vi < reason_.size(); ++vi) {
                const ClauseRef r = reason_[vi];
                if (r != kNullClause && remap[clause_index(r)] != null_idx)
                    reason_[vi] = ClauseRef{remap[clause_index(r)]};
            }

            clauses_ = std::move(new_clauses);
            lits_ = std::move(new_lits);

            // Rebuild watches from scratch over the surviving clauses.
            for (auto& wl : watches_) wl.clear();
            for (std::uint32_t ci = 0; ci < clauses_.size(); ++ci)
                if (clauses_[ci].size >= 2) attach_watches(ClauseRef{ci});
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
            // Blocker of each watch is the *other* watched literal: a cheap
            // satisfied-clause test before touching the arena.
            watches_[lit_index(ls[0])].push_back(Watch{cr, ls[1]});
            watches_[lit_index(ls[1])].push_back(Watch{cr, ls[0]});
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
                const std::uint32_t vi = var_index(v);
                phase_[vi] = (assign_[vi] == LBool::True);
                assign_[vi] = LBool::Undef;
                reason_[vi] = kNullClause;
                // var is decidable again → return it to the heap.
                if (heap_built_) order_heap_.insert(vi);
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
                    const Watch w = ws[i++];
                    // Blocking-literal fast path: if the cached blocker is already
                    // satisfied the clause is too — keep the watch, skip the deref.
                    if (value(w.blocker) == LBool::True) {
                        ws[j++] = w;
                        continue;
                    }

                    const ClauseRef cr = w.cr;
                    if (clauses_[clause_index(cr)].deleted) continue;
                    auto ls = clause_lits(cr);
                    // ensure ls[1] is the falsified watch
                    if (ls[0] == falsified) std::swap(ls[0], ls[1]);
                    const Lit other = ls[0];
                    // if ls[0] true, clause satisfied — keep watch, refresh blocker
                    if (value(other) == LBool::True) {
                        ws[j++] = Watch{cr, other};
                        continue;
                    }
                    // look for a new watch among ls[2..]
                    bool moved = false;
                    for (std::size_t k = 2; k < ls.size(); ++k) {
                        if (value(ls[k]) != LBool::False) {
                            std::swap(ls[1], ls[k]);
                            watches_[lit_index(ls[1])].push_back(Watch{cr, other});
                            moved = true;
                            break;
                        }
                    }
                    if (moved) continue;
                    // clause is unit or conflicting under ls[0]
                    ws[j++] = Watch{cr, other};
                    if (!enqueue(other, cr)) {
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
            seen_set_.reserve(num_vars() + 1);
            seen_set_.clear();
            analyze_tmp_.push_back(kNullLit); // placeholder for asserting literal

            std::uint32_t path_count = 0;
            std::size_t idx = trail_.size();
            Lit p = kNullLit;
            ClauseRef reason = confl;

            do {
                // Mark the resolved clause as recently useful so reduce_db grants
                // it a one-round reprieve (kNullClause = decision, no clause).
                if (reason != kNullClause) clauses_[clause_index(reason)].used = true;
                auto ls = clause_lits(reason);
                for (Lit q : ls) {
                    if (p != kNullLit && lit_var(q) == lit_var(p)) continue; // skip resolved pivot
                    const Var v = lit_var(q);
                    const std::uint32_t vi = var_index(v);
                    if (!seen_set_.contains(vi) && level_[vi] > 0) {
                        seen_set_.insert_or_update(vi);
                        bump_var(v);
                        if (level_[vi] >= decision_level()) {
                            ++path_count;
                        }
                        else {
                            analyze_tmp_.push_back(q);
                        }
                    }
                }
                // pick next literal to resolve from the trail
                while (idx > 0 && !seen_set_.contains(var_index(lit_var(trail_[--idx])))) {}
                p = trail_[idx];
                reason = reason_[var_index(lit_var(p))];
                (void)seen_set_.remove(var_index(lit_var(p)));
                --path_count;
            }
            while (path_count > 0);

            // asserting literal is ¬p (the UIP)
            analyze_tmp_[0] = lit_neg(p);

            // backjump level = second-highest level in the learnt clause
            std::uint32_t bj = 0;
            std::size_t max_i = 1;
            for (std::size_t i = 1; i < analyze_tmp_.size(); ++i) {
                const std::uint32_t lv = level_[var_index(lit_var(analyze_tmp_[i]))];
                if (lv > bj) {
                    bj = lv;
                    max_i = i;
                }
            }
            if (analyze_tmp_.size() > 1) std::swap(analyze_tmp_[1], analyze_tmp_[max_i]);

            backjump_to(bj);

            // asserting literal is unassigned after backjump — enqueue cannot conflict
            if (analyze_tmp_.size() == 1) {
                (void)enqueue(analyze_tmp_[0], kNullClause);
            }
            else {
                const ClauseRef cr = alloc_clause(analyze_tmp_, /*learnt=*/true);
                attach_watches(cr);
                update_lbd_ema(clauses_[clause_index(cr)].lbd);
                (void)enqueue(analyze_tmp_[0], cr);
            }
        }

        // Feed a learnt clause's LBD into the fast/slow EMAs used by the adaptive
        // restart heuristic.
        void update_lbd_ema(std::uint16_t lbd) noexcept {
            const double x = static_cast<double>(lbd);
            if (lbd_ema_count_ == 0) {
                lbd_ema_fast_ = lbd_ema_slow_ = x;
            }
            else {
                lbd_ema_fast_ += kEmaFastAlpha * (x - lbd_ema_fast_);
                lbd_ema_slow_ += kEmaSlowAlpha * (x - lbd_ema_slow_);
            }
            ++lbd_ema_count_;
        }

        // Adaptive restart trigger (Glucose): recent clause quality (fast EMA)
        // markedly worse than the long-run average (slow EMA) → restart. Needs a
        // warmup before the EMAs are meaningful.
        [[nodiscard]] bool should_restart_ema() const noexcept {
            return lbd_ema_count_ >= 50 && lbd_ema_fast_ > lbd_ema_slow_ * kRestartMargin;
        }

        // ---- VSIDS ----------------------------------------------------------

        void bump_var(Var v) {
            const std::uint32_t vi = var_index(v);
            activity_[vi] += var_inc_;
            // activity rose → move v up in the decision heap if present.
            if (heap_built_) order_heap_.increase(vi);
            if (activity_[vi] > 1e100) {
                for (double& a : activity_) a *= 1e-100;
                var_inc_ *= 1e-100;
            }
        }

        void decay_activity() { var_inc_ /= var_decay_; }

        // ---- decisions ------------------------------------------------------

        [[nodiscard]] bool decide() {
            const Var best = pick_branch_var();
            if (best == kNullVar) return false; // complete assignment
            new_decision_level();
            const bool neg = phase_[var_index(best)]; // phase saving
            (void)enqueue(make_lit(best, neg), kNullClause); // best is Undef → no conflict
            return true;
        }

        // Pick the max-activity unassigned variable. Uses the order-heap in
        // O(log V) for non-trivial universes; a linear scan for tiny ones where
        // heap upkeep does not pay off (and reproduces the old exact behavior).
        [[nodiscard]] Var pick_branch_var() {
            if (assign_.size() < kHeapThreshold) {
                Var best = kNullVar;
                double best_act = -1.0;
                for (std::uint32_t i = 0; i < assign_.size(); ++i) {
                    if (assign_[i] == LBool::Undef && activity_[i] > best_act) {
                        best_act = activity_[i];
                        best = Var{i};
                    }
                }
                return best;
            }
            if (!heap_built_) build_heap();
            while (!order_heap_.empty()) {
                const std::uint32_t v = order_heap_.remove_max();
                if (assign_[v] == LBool::Undef) return Var{v};
            }
            return kNullVar;
        }

        // (Re)populate the decision heap with every currently-unassigned var.
        void build_heap() {
            order_heap_.reserve_universe(assign_.size());
            order_heap_.clear();
            for (std::uint32_t i = 0; i < assign_.size(); ++i)
                if (assign_[i] == LBool::Undef) order_heap_.insert(i);
            heap_built_ = true;
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
                if (i < (k << 1u) - 1u) {
                    i = i - ((k) - 1u);
                    k = 1;
                    continue;
                }
                k <<= 1u;
            }
        }
    };
} // namespace tarka::native
