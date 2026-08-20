#pragma once
// =============================================================================
// tarka/native/theory_concept.hpp — TheorySolver concept + shared result types
//
// Namespace:  tarka::native
// Provides:   TheoryResult, theory_lit, TheorySolver concept.
//
// Design:
//   - No virtual, no macros. Theories are plain structs; the DPLL(T) driver and
//     theory_combination call them through this concept, so dispatch is fully
//     static (inlinable, dead-code-eliminated when unused). No vtables.
//   - A theory owns the semantics of the atoms tagged for it (see AtomTheory).
//     The Boolean engine tells the theory which atoms became true/false via
//     assert_lit; the theory reports consistency in check().
//   - Backtracking mirrors the SAT trail: push_level / pop_level bracket each
//     decision level. Theories that use non-rollbackable structures (union_find)
//     keep their own per-level undo trail.
// =============================================================================

#include "tarka/native/atom_registry.hpp"
#include "tarka/native/ids.hpp"

#include <concepts>
#include <cstdint>
#include <span>

namespace tarka::native {
    // Outcome of a theory consistency check at a Boolean fixpoint.
    enum class TheoryStatus : std::uint8_t {
        Sat, // theory is consistent with the current partial assignment
        Conflict, // inconsistent — a conflict clause is available via explain()
        Propagated, // theory implied new literals — re-run BCP
    };

    // A theory literal: an AtomId plus the truth value it was asserted at.
    struct theory_lit {
        AtomId atom;
        bool value; // true == atom asserted positively
    };

    // The static interface every native theory must provide.
    //
    //   attach(reg)        — bind to the atom_registry (once, before solving).
    //   register_atom(a)   — called for each atom tagged for this theory so it
    //                        can pre-build its internal representation.
    //   assert_lit(a,val)  — the SAT core assigned atom `a` to `val`.
    //   check()            — verify consistency at the current fixpoint.
    //   explain()          — after Conflict, the falsified clause (theory lemma)
    //                        as literals over the shared Vars; after Propagated,
    //                        the implied unit literals.
    //   push_level/pop_level — bracket SAT decision levels for backtracking.
    //   reset()            — clear all state.
    template <class T>
    concept TheorySolver = requires(T t, const T ct, atom_registry& reg, AtomId a, bool val) {
        { t.attach(reg) } -> std::same_as<void>;
        { t.register_atom(a) } -> std::same_as<void>;
        { t.assert_lit(a, val) } -> std::same_as<void>;
        { t.check() } -> std::same_as<TheoryStatus>;
        { ct.explanation() } -> std::convertible_to<std::span<const Lit>>;
        { t.push_level() } -> std::same_as<void>;
        { t.pop_level() } -> std::same_as<void>;
        { t.reset() } -> std::same_as<void>;
    };
} // namespace tarka::native
