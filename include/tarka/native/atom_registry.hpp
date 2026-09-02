#pragma once
// =============================================================================
// tarka/native/atom_registry.hpp — Term <-> AtomId <-> Var interning
//
// Namespace:  tarka::native
// Provides:   atom_registry — stable, non-evicting bijection between theory/
//             Boolean atoms (interned tarka::Term) and SAT variables.
//
// Design:
//   - No virtual, no macros. Header-only, C++23.
//   - Each distinct atom Term (keyed by Term.hash(), pointer-checked on
//     collision) maps to one AtomId and one SAT Var. The mapping is stable:
//     unlike a cache, atoms are never evicted — a Var, once minted, keeps its
//     meaning for the whole solve.
//   - Every atom carries a theory tag (which theory owns its semantics) taken
//     from op_descriptor theory_bits at registration time. The Boolean skeleton
//     uses tag == core.
//   - Fresh (Tseitin) auxiliary variables have no backing Term; mint them with
//     new_aux_var(). They are core-tagged and excluded from theory dispatch.
// =============================================================================

#include "tarka/native/ids.hpp"
#include "tarka/term.hpp"

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

namespace tarka::native {
    // Which theory owns an atom's semantics. Mirrors tarka::theory_family but
    // narrowed to the families the native backend implements.
    enum class AtomTheory : std::uint8_t {
        core = 0, // Boolean / Eq / Distinct / Ite skeleton
        bv,
        lra,
        lia,
        array,
        uf,
    };

    struct AtomInfo {
        Term term; // backing term (invalid() for aux vars)
        Var var; // SAT variable representing this atom's truth
        AtomTheory theory;
    };

    class atom_registry {
    public:
        // Var minter: supplies fresh SAT variables. Set by the owner (encoder)
        // so the registry and the cdcl_solver share one variable numbering.
        // When unset, the registry mints its own contiguous indices.
        using var_minter_t = std::function<Var()>;

        void set_var_minter(var_minter_t m) { minter_ = std::move(m); }

        // Register (or look up) the atom for `t`, tagged for `theory`. Returns
        // the stable AtomId; idempotent for equal terms.
        AtomId intern(Term t, AtomTheory theory) {
            const std::uint64_t h = t.hash();
            auto [it, inserted] = by_hash_.try_emplace(h, kNullAtom);
            if (!inserted) {
                // hash hit — confirm identity (pointer equality on interned node)
                const AtomId existing = it->second;
                if (atoms_[atom_index(existing)].term.ptr() == t.ptr()) return existing;
                // rare hash collision on distinct terms: fall through to a
                // linear probe in the collision chain.
                for (const AtomId a : collisions_) {
                    if (atoms_[atom_index(a)].term.ptr() == t.ptr()) return a;
                }
            }
            const AtomId id = mint(t, theory);
            if (inserted) {
                it->second = id;
            }
            else {
                collisions_.push_back(id);
            }
            return id;
        }

        // Mint a fresh Boolean variable with no backing term (Tseitin aux).
        [[nodiscard]] Var new_aux_var() {
            const Var v = alloc_var();
            const AtomId id{static_cast<std::uint32_t>(atoms_.size())};
            atoms_.push_back(AtomInfo{Term{}, v, AtomTheory::core});
            aux_only_.push_back(id);
            var_to_atom_[var_index(v)] = id;
            return v;
        }

        // ---- queries --------------------------------------------------------

        [[nodiscard]] std::size_t num_atoms() const noexcept { return atoms_.size(); }
        [[nodiscard]] std::size_t num_vars() const noexcept { return next_var_; }

        [[nodiscard]] const AtomInfo& atom(AtomId a) const noexcept {
            return atoms_[atom_index(a)];
        }

        [[nodiscard]] Var var_of(AtomId a) const noexcept { return atoms_[atom_index(a)].var; }
        [[nodiscard]] AtomTheory theory_of(AtomId a) const noexcept { return atoms_[atom_index(a)].theory; }

        // Reverse: SAT Var -> AtomId (kNullAtom if the Var is unmapped).
        [[nodiscard]] AtomId atom_of_var(Var v) const noexcept {
            const std::uint32_t vi = var_index(v);
            return vi < var_to_atom_.size() ? var_to_atom_[vi] : kNullAtom;
        }

        [[nodiscard]] AtomId atom_of(Var v) const noexcept { return atom_of_var(v); }

        void reset() {
            by_hash_.clear();
            collisions_.clear();
            atoms_.clear();
            aux_only_.clear();
            var_to_atom_.clear();
            next_var_ = 0;
        }

    private:
        [[nodiscard]] Var alloc_var() {
            if (minter_) {
                const Var v = minter_();
                if (var_index(v) >= var_to_atom_.size()) var_to_atom_.resize(var_index(v) + 1, kNullAtom);
                next_var_ = var_to_atom_.size();
                return v;
            }
            const Var v{next_var_++};
            var_to_atom_.push_back(kNullAtom);
            return v;
        }

        [[nodiscard]] AtomId mint(Term t, AtomTheory theory) {
            const Var v = alloc_var();
            const AtomId id{static_cast<std::uint32_t>(atoms_.size())};
            atoms_.push_back(AtomInfo{t, v, theory});
            var_to_atom_[var_index(v)] = id;
            return id;
        }

        std::unordered_map<std::uint64_t, AtomId> by_hash_;
        std::vector<AtomId> collisions_; // atoms whose hash collided
        std::vector<AtomInfo> atoms_;
        std::vector<AtomId> aux_only_;
        std::vector<AtomId> var_to_atom_; // indexed by var_index
        std::uint32_t next_var_ = 0;
        var_minter_t minter_;
    };
} // namespace tarka::native
