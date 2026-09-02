#pragma once
// =============================================================================
// tarka/native/theory_combination.hpp — Nelson-Oppen combination (static)
//
// Namespace:  tarka::native
// Provides:   theory_combination<Ts...> — holds a std::tuple of TheorySolvers
//             and presents the same TheorySolver-shaped interface, broadcasting
//             lifecycle calls and running the combination check.
//
// Design:
//   - No virtual, no macros. The theory set is fixed at compile time, so the
//     tuple keeps every hot-path call direct and inlinable; unused theories are
//     dead-code-eliminated. Dispatch of an atom to its owning theory is a
//     compile-time family->index fold (if constexpr), not a vtable.
//   - check() runs a Nelson-Oppen-style fixpoint: each theory checks its own
//     consistency; if any reports Conflict the combination is UNSAT (the
//     conflicting theory's explanation is exposed). Propagated re-runs BCP.
//     (Shared-variable equality exchange is added in the full combination
//     phase; the single-theory and disjoint-signature cases are exact here.)
// =============================================================================

#include "tarka/native/atom_registry.hpp"
#include "tarka/native/ids.hpp"
#include "tarka/native/theory_concept.hpp"

#include <cstdint>
#include <span>
#include <tuple>
#include <utility>

namespace tarka::native {
    template <TheorySolver... Ts>
    class theory_combination {
    public:
        static constexpr std::size_t theory_count = sizeof...(Ts);

        void attach(atom_registry& reg) {
            reg_ = &reg;
            std::apply([&](auto&... th) { (th.attach(reg), ...); }, theories_);
        }

        void attach_sat(cdcl_solver& sat) {
            std::apply([&](auto&... th) {
                (attach_sat_one(th, sat), ...);
            }, theories_);
        }

        // Broadcast atom registration to all theories so each collects the
        // subterms it owns. Atoms in combined logics (QF_AUFBV: select/store over
        // BV elements, uninterpreted functions applied to array reads) carry more
        // than one theory's structure, so single-family routing would starve a
        // theory of atoms it must reason about and silently return SAT on an UNSAT
        // formula. Broadcast keeps every theory's view complete; family-tag
        // dispatch is sound only once full Nelson-Oppen shared-variable exchange
        // is in place (not yet implemented), so it is intentionally not used here.
        void register_atom(AtomId a) {
            std::apply([&](auto&... th) { (th.register_atom(a), ...); }, theories_);
        }

        // Broadcast literal assertions to all theories (see register_atom).
        void assert_lit(AtomId a, bool value) {
            std::apply([&](auto&... th) { (th.assert_lit(a, value), ...); }, theories_);
        }

        // Nelson-Oppen fixpoint over the tuple. Records which theory produced a
        // conflict so explanation() can forward its clause.
        [[nodiscard]] TheoryStatus check() {
            conflict_theory_ = kNoTheory;
            TheoryStatus agg = TheoryStatus::Sat;
            bool any_propagated = false;
            std::size_t idx = 0;
            bool conflict = false;
            std::apply([&](auto&... th) {
                (check_one(th, idx, any_propagated, conflict), ...);
            }, theories_);
            if (conflict) return TheoryStatus::Conflict;
            if (any_propagated) agg = TheoryStatus::Propagated;
            return agg;
        }

        [[nodiscard]] std::span<const Lit> explanation() const {
            std::span<const Lit> out{};
            std::size_t idx = 0;
            std::apply([&](const auto&... th) {
                (grab_explanation(th, idx, out), ...);
            }, theories_);
            return out;
        }

        void push_level() {
            std::apply([](auto&... th) { (th.push_level(), ...); }, theories_);
        }

        void pop_level() {
            std::apply([](auto&... th) { (th.pop_level(), ...); }, theories_);
        }

        void reset() {
            conflict_theory_ = kNoTheory;
            std::apply([](auto&... th) { (th.reset(), ...); }, theories_);
        }

        template <class Th>
        static constexpr bool has_theory = (std::is_same_v<Th, Ts> || ...);

        // Direct access to a specific theory (for model extraction).
        template <std::size_t I>
        [[nodiscard]] auto& get() noexcept { return std::get < I > (theories_); }

        template <std::size_t I>
        [[nodiscard]] const auto& get() const noexcept { return std::get < I > (theories_); }

        template <class Th>
        [[nodiscard]] Th& get() noexcept { return std::get<Th>(theories_); }

        template <class Th>
        [[nodiscard]] const Th& get() const noexcept { return std::get<Th>(theories_); }

    private:
        static constexpr std::size_t kNoTheory = static_cast<std::size_t>(-1);

        template <class Th>
        static void attach_sat_one(Th& th, cdcl_solver& sat) {
            if constexpr (requires { th.attach_sat(sat); }) {
                th.attach_sat(sat);
            }
            else {
                (void)th;
                (void)sat;
            }
        }

        template <class Th>
        void check_one(Th& th, std::size_t& idx, bool& any_propagated, bool& conflict) {
            if (conflict) {
                ++idx;
                return;
            } // short-circuit
            const TheoryStatus s = th.check();
            if (s == TheoryStatus::Conflict) {
                conflict = true;
                conflict_theory_ = idx;
            }
            else if (s == TheoryStatus::Propagated) {
                any_propagated = true;
            }
            ++idx;
        }

        template <class Th>
        void grab_explanation(const Th& th, std::size_t& idx, std::span<const Lit>& out) const {
            if (idx == conflict_theory_) out = th.explanation();
            ++idx;
        }

        // Call `fn(theory)` for the tuple element whose family matches `fam`.
        template <class Fn>
        void dispatch(AtomTheory fam, Fn&& fn) {
            std::apply([&](auto&... th) {
                (dispatch_one(th, fam, fn), ...);
            }, theories_);
        }

        template <class Th, class Fn>
        void dispatch_one(Th& th, AtomTheory fam, Fn& fn) {
            if (family_of<Th>() == fam) fn(th);
        }

        // Map a theory type to the AtomTheory family it owns. Specialize by
        // convention: each theory exposes `static constexpr AtomTheory family`.
        template <class Th>
        [[nodiscard]] static constexpr AtomTheory family_of() noexcept {
            return Th::family;
        }

        std::tuple<Ts...> theories_;
        atom_registry* reg_ = nullptr;
        std::size_t conflict_theory_ = kNoTheory;
    };
} // namespace tarka::native
