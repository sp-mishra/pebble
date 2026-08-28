#pragma once
// =============================================================================
// tarka/native/theory_uf.hpp — Equality & Uninterpreted Functions (EUF / QF_UF)
//
// Namespace:  tarka::native
// Provides:   theory_uf — a TheorySolver implementing Congruence Closure
//             with proof tracking, backtrackable union-find, signature indexing,
//             and conflict explanation.
//
// Theory (docs/tarka/tarka.md "Native Backend / EUF"):
//   Implements Nelson-Oppen / Downey-Sethi-Tarjan style congruence closure:
//   - Terms: constants, symbols, and function applications (Op::Apply).
//   - Invariant: if a = b then f(..., a, ...) = f(..., b, ...).
//   - Backtracking: undo trail for union-find merges and signature table updates.
//   - Conflict explanation: proof forest path extraction between conflicting terms.
//
// Design:
//   - No virtual, no macros. Header-only, C++23.
//   - Satisfies TheorySolver concept.
// =============================================================================

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
    class theory_uf {
    public:
        static constexpr AtomTheory family = AtomTheory::uf;

        void attach(atom_registry& reg) noexcept { reg_ = &reg; }

        // Recursively register a term and its subterms into the congruence graph
        void register_atom(AtomId a) {
            const Term t = reg_->atom(a).term;
            if (!t.valid()) return;
            register_term_tree(t);
        }

        void assert_lit(AtomId a, bool value) {
            const Term t = reg_->atom(a).term;
            if (!t.valid()) return;

            if (t.op() == Op::Eq) {
                auto ch = t.children();
                if (ch.size() == 2) {
                    const std::uint32_t u = term_id(ch[0]);
                    const std::uint32_t v = term_id(ch[1]);
                    if (value) {
                        // Equality: u == v
                        pending_equalities_.push_back(EqualityAssert{u, v, a, true});
                    } else {
                        // Disequality: u != v
                        disequalities_.push_back(Disequality{u, v, a, false, level_});
                    }
                }
            } else if (t.op() == Op::Distinct) {
                auto ch = t.children();
                if (value) {
                    // Distinct(a, b, c...) => pairwise disequalities
                    for (std::size_t i = 0; i < ch.size(); ++i) {
                        for (std::size_t j = i + 1; j < ch.size(); ++j) {
                            disequalities_.push_back(Disequality{term_id(ch[i]), term_id(ch[j]), a, true, level_});
                        }
                    }
                }
            }
        }

        [[nodiscard]] TheoryStatus check() {
            explanation_.clear();

            // Process all pending equalities and propagate congruences
            while (!pending_equalities_.empty()) {
                auto eq = pending_equalities_.back();
                pending_equalities_.pop_back();
                merge(eq.u, eq.v, eq.atom, eq.value);
            }

            // Check for disequality violations: if u != v is asserted but find(u) == find(v)
            for (const auto& deq : disequalities_) {
                if (find(deq.u) == find(deq.v)) {
                    // Conflict found!
                    explain_conflict(deq);
                    return TheoryStatus::Conflict;
                }
            }

            return TheoryStatus::Sat;
        }

        [[nodiscard]] std::span<const Lit> explanation() const noexcept {
            return explanation_;
        }

        [[nodiscard]] std::uint32_t get_representative(Term t) {
            const std::uint32_t id = term_id(t);
            return find(id);
        }

        void push_level() {
            ++level_;
            trail_limits_.push_back(trail_.size());
            deq_limits_.push_back(disequalities_.size());
        }

        void pop_level() {
            if (level_ == 0 || trail_limits_.empty()) return;
            --level_;

            const std::size_t target_trail = trail_limits_.back();
            trail_limits_.pop_back();
            while (trail_.size() > target_trail) {
                undo_step(trail_.back());
                trail_.pop_back();
            }

            const std::size_t target_deq = deq_limits_.back();
            deq_limits_.pop_back();
            disequalities_.resize(target_deq);

            pending_equalities_.clear();
            explanation_.clear();
        }

        void reset() {
            nodes_.clear();
            term_to_id_.clear();
            sig_table_.clear();
            disequalities_.clear();
            pending_equalities_.clear();
            trail_.clear();
            trail_limits_.clear();
            deq_limits_.clear();
            explanation_.clear();
            level_ = 0;
        }

    private:
        struct Node {
            Term term;
            std::uint32_t parent; // Union-find parent
            std::uint32_t rank;   // Union-find rank
            Op op;
            std::vector<std::uint32_t> args;    // argument term IDs
            std::vector<std::uint32_t> parents; // terms that have this node as argument
            // Proof tracking
            std::uint32_t proof_parent = 0;
            AtomId proof_atom = kNullAtom;
            bool proof_val = false;
        };

        struct Signature {
            Op op;
            std::uint64_t func_hash;
            std::vector<std::uint32_t> arg_roots;

            [[nodiscard]] bool operator==(const Signature& o) const noexcept {
                return op == o.op && func_hash == o.func_hash && arg_roots == o.arg_roots;
            }
        };

        struct SignatureHash {
            [[nodiscard]] std::size_t operator()(const Signature& s) const noexcept {
                std::size_t h = static_cast<std::size_t>(s.op) ^ (s.func_hash * 0x9e3779b97f4a7c15ULL);
                for (std::uint32_t r : s.arg_roots) {
                    h = (h ^ static_cast<std::size_t>(r)) * 0x100000001B3ULL;
                }
                return h;
            }
        };

        struct Disequality {
            std::uint32_t u;
            std::uint32_t v;
            AtomId atom;
            bool value;
            std::uint32_t level;
        };

        struct EqualityAssert {
            std::uint32_t u;
            std::uint32_t v;
            AtomId atom;
            bool value;
        };

        enum class UndoKind : std::uint8_t {
            Union,
            SigTableInsert,
            SigTableOverwrite
        };

        struct UndoEntry {
            UndoKind kind;
            std::uint32_t u;
            std::uint32_t v;
            std::uint32_t prev_rank;
            Signature sig;
            std::uint32_t old_target;
        };

        std::uint32_t register_term_tree(Term t) {
            auto it = term_to_id_.find(t.hash());
            if (it != term_to_id_.end()) {
                if (nodes_[it->second].term.ptr() == t.ptr()) return it->second;
            }

            // Register children first
            std::vector<std::uint32_t> child_ids;
            for (Term c : t.children()) {
                child_ids.push_back(register_term_tree(c));
            }

            const std::uint32_t id = static_cast<std::uint32_t>(nodes_.size());
            Node n;
            n.term = t;
            n.parent = id;
            n.rank = 0;
            n.op = t.op();
            n.args = child_ids;
            n.proof_parent = id;
            nodes_.push_back(std::move(n));
            term_to_id_[t.hash()] = id;

            // Link parent in children
            for (std::uint32_t c_id : child_ids) {
                nodes_[c_id].parents.push_back(id);
            }

            // Insert into signature table if it's an application
            if (t.op() == Op::Apply) {
                Signature sig = compute_sig(id);
                auto [sit, inserted] = sig_table_.try_emplace(sig, id);
                if (inserted) {
                    trail_.push_back(UndoEntry{UndoKind::SigTableInsert, id, 0, 0, sig, 0});
                }
            }

            return id;
        }

        std::uint32_t term_id(Term t) {
            return register_term_tree(t);
        }

        Signature compute_sig(std::uint32_t id) {
            const Node& n = nodes_[id];
            Signature sig;
            sig.op = n.op;
            sig.func_hash = n.term.ptr() ? n.term.ptr()->payload_hash : 0;
            sig.arg_roots.reserve(n.args.size());
            for (std::uint32_t arg : n.args) {
                sig.arg_roots.push_back(find(arg));
            }
            return sig;
        }

        std::uint32_t find(std::uint32_t u) {
            while (nodes_[u].parent != u) {
                u = nodes_[u].parent;
            }
            return u;
        }

        void merge(std::uint32_t u, std::uint32_t v, AtomId reason_atom, bool reason_val) {
            std::uint32_t root_u = find(u);
            std::uint32_t root_v = find(v);
            if (root_u == root_v) return;

            // Union by rank
            if (nodes_[root_u].rank < nodes_[root_v].rank) {
                std::swap(root_u, root_v);
                std::swap(u, v);
            }

            const std::uint32_t prev_rank = nodes_[root_u].rank;
            nodes_[root_v].parent = root_u;
            nodes_[root_v].proof_parent = root_u;
            nodes_[root_v].proof_atom = reason_atom;
            nodes_[root_v].proof_val = reason_val;

            if (nodes_[root_u].rank == nodes_[root_v].rank) {
                ++nodes_[root_u].rank;
            }

            trail_.push_back(UndoEntry{UndoKind::Union, root_u, root_v, prev_rank, {}, 0});

            // Propagate congruences across parents
            // For every parent of root_v, check if it now matches a signature in sig_table
            std::vector<std::uint32_t> parents_v = nodes_[root_v].parents;
            for (std::uint32_t p : parents_v) {
                if (nodes_[p].op != Op::Apply) continue;
                Signature old_sig = compute_sig(p);
                Signature new_sig = old_sig; // will use updated roots
                for (std::size_t i = 0; i < new_sig.arg_roots.size(); ++i) {
                    new_sig.arg_roots[i] = find(nodes_[p].args[i]);
                }

                auto sit = sig_table_.find(new_sig);
                if (sit != sig_table_.end() && sit->second != p) {
                    // Congruence found! p and sit->second have identical signatures
                    pending_equalities_.push_back(EqualityAssert{p, sit->second, reason_atom, reason_val});
                } else {
                    auto [it, ins] = sig_table_.try_emplace(new_sig, p);
                    if (ins) {
                        trail_.push_back(UndoEntry{UndoKind::SigTableInsert, p, 0, 0, new_sig, 0});
                    }
                }
            }
        }

        void undo_step(const UndoEntry& e) {
            switch (e.kind) {
                case UndoKind::Union: {
                    nodes_[e.v].parent = e.v;
                    nodes_[e.v].proof_parent = e.v;
                    nodes_[e.v].proof_atom = kNullAtom;
                    nodes_[e.u].rank = e.prev_rank;
                    break;
                }
                case UndoKind::SigTableInsert: {
                    sig_table_.erase(e.sig);
                    break;
                }
                case UndoKind::SigTableOverwrite: {
                    sig_table_[e.sig] = e.old_target;
                    break;
                }
            }
        }

        void explain_conflict(const Disequality& deq) {
            explanation_.clear();

            // The conflict clause is: (¬reason(deq)) ∨ (¬path_reasons(u, v))
            if (deq.atom != kNullAtom) {
                const Var v = reg_->var_of(deq.atom);
                explanation_.push_back(make_lit(v, /*negated=*/deq.value));
            }

            // Extract proof path between deq.u and deq.v in the union-find tree
            extract_path_explanation(deq.u, deq.v);
        }

        void extract_path_explanation(std::uint32_t u, std::uint32_t v) {
            // Find root and record path for u and v
            std::vector<std::uint32_t> path_u;
            std::vector<std::uint32_t> path_v;

            std::uint32_t curr = u;
            while (nodes_[curr].proof_parent != curr) {
                path_u.push_back(curr);
                curr = nodes_[curr].proof_parent;
            }
            path_u.push_back(curr);

            curr = v;
            while (nodes_[curr].proof_parent != curr) {
                path_v.push_back(curr);
                curr = nodes_[curr].proof_parent;
            }
            path_v.push_back(curr);

            // Find lowest common ancestor in union-find proof tree
            std::int64_t i = static_cast<std::int64_t>(path_u.size()) - 1;
            std::int64_t j = static_cast<std::int64_t>(path_v.size()) - 1;
            while (i >= 0 && j >= 0 && path_u[static_cast<std::size_t>(i)] == path_v[static_cast<std::size_t>(j)]) {
                --i;
                --j;
            }

            // Collect proof atoms from u up to LCA
            for (std::int64_t k = 0; k <= i; ++k) {
                const Node& n = nodes_[path_u[static_cast<std::size_t>(k)]];
                if (n.proof_atom != kNullAtom) {
                    const Var var = reg_->var_of(n.proof_atom);
                    explanation_.push_back(make_lit(var, /*negated=*/n.proof_val));
                }
            }

            // Collect proof atoms from v up to LCA
            for (std::int64_t k = 0; k <= j; ++k) {
                const Node& n = nodes_[path_v[static_cast<std::size_t>(k)]];
                if (n.proof_atom != kNullAtom) {
                    const Var var = reg_->var_of(n.proof_atom);
                    explanation_.push_back(make_lit(var, /*negated=*/n.proof_val));
                }
            }
        }

        atom_registry* reg_ = nullptr;
        std::vector<Node> nodes_;
        std::unordered_map<std::uint64_t, std::uint32_t> term_to_id_;
        std::unordered_map<Signature, std::uint32_t, SignatureHash> sig_table_;
        std::vector<Disequality> disequalities_;
        std::vector<EqualityAssert> pending_equalities_;

        std::vector<UndoEntry> trail_;
        std::vector<std::size_t> trail_limits_;
        std::vector<std::size_t> deq_limits_;
        std::vector<Lit> explanation_;
        std::uint32_t level_ = 0;
    };
} // namespace tarka::native
