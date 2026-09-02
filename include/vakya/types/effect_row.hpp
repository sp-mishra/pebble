#pragma once

// =============================================================================
// vakya/types/effect_row.hpp — effect polymorphism / effect rows (opt-in)
//
// C++23, header-only, no virtual, no macros.
// Namespace: vakya::types
//
// The flat effect_mask gives functions a fixed effect set. An effect ROW makes the effect component polymorphic:
//   row ::= { concrete_mask | ρ }
// where concrete_mask is the known effects and ρ (tail_var) is a polymorphic tail
// standing for "whatever effects the caller's arguments carry". This lets a
// higher-order combinator like map : (a →{ρ} b) → [a] →{ρ} [b] be effect-generic
// instead of over-approximating to "all effects".
//
// Subsumption ε₁ ⊑ ε₂ ("ε₁'s effects are permitted by ε₂"):
//   concrete⊆concrete AND (ε₁ closed OR ε₂ open)     — Rémy/Leijen row rule
//   * both closed, concrete⊆concrete                 → provable here (rule solver)
//   * ε₂ open (has tail)                             → provable here
//   * ε₁ open, ε₂ closed                             → refuted (leaks tail)
//   * tails are distinct symbolic vars              → deferred to SMT band
//
// effect_row_ref (tag in opt_handles.hpp) indexes a small side-arena; the handle
// lives in analysis_record::effect_row.
//
// Reuse: effect_mask + builtin masks from vakya/types/effect.hpp; slot_map arena.
//
// Dependencies: vakya/types/effect.hpp, vakya/constraints.hpp,
//               containers/associative/slot_map.hpp
// =============================================================================

#include "vakya/types/effect.hpp"
#include "vakya/types/opt_handles.hpp"
#include "vakya/constraints.hpp"
#include "containers/associative/slot_map.hpp"

#include <cstdint>
#include <limits>
#include <span>
#include <unordered_map>

namespace vakya::types {
    // ============================================================================
    // effect_row_var — polymorphic tail variable id (kNoEffectTail = closed row)
    // ============================================================================

    using effect_row_var = std::uint32_t;
    inline constexpr effect_row_var kNoEffectTail = std::numeric_limits<effect_row_var>::max();

    // ============================================================================
    // effect_row_node — { concrete_mask | tail_var }
    // ============================================================================

    struct effect_row_node {
        effect_mask concrete = 0;
        effect_row_var tail_var = kNoEffectTail; // kNoEffectTail == closed row

        [[nodiscard]] bool is_open() const noexcept { return tail_var != kNoEffectTail; }

        [[nodiscard]] bool operator==(const effect_row_node& o) const noexcept {
            return concrete == o.concrete && tail_var == o.tail_var;
        }
    };

    // ============================================================================
    // effect_row_arena — interns effect rows to effect_row_ref handles.
    // ============================================================================

    class effect_row_arena {
    public:
        effect_row_arena() = default;

        // Closed row over a concrete mask.
        [[nodiscard]] effect_row_ref intern_effect_row(effect_mask concrete) {
            return intern(effect_row_node{concrete, kNoEffectTail});
        }

        // Open row { concrete | ρ }.
        [[nodiscard]] effect_row_ref intern_open_row(effect_mask concrete, effect_row_var tail) {
            return intern(effect_row_node{concrete, tail});
        }

        [[nodiscard]] const effect_row_node* get(effect_row_ref r) const noexcept {
            return store_.find(r);
        }

        // Fresh polymorphic tail variable.
        [[nodiscard]] effect_row_var fresh_tail() noexcept { return next_tail_++; }

        [[nodiscard]] std::size_t size() const noexcept { return store_.size(); }

    private:
        [[nodiscard]] effect_row_ref intern(const effect_row_node& n) {
            const std::uint64_t h =
                (static_cast<std::uint64_t>(n.tail_var) << 1) ^ (n.concrete * 1099511628211ULL);
            if (const auto it = intern_.find(h); it != intern_.end()) {
                if (const effect_row_node* e = store_.find(it->second); e && *e == n)
                    return it->second;
            }
            const effect_row_ref ref = store_.insert(n);
            intern_.emplace(h, ref);
            return ref;
        }

        containers::slot_map<effect_row_node, effect_row_ref> store_;
        std::unordered_map<std::uint64_t, effect_row_ref> intern_;
        effect_row_var next_tail_ = 0;
    };

    // ============================================================================
    // subsumption result — decidable here, or deferred to SMT.
    // ============================================================================

    enum class row_subsume_result : std::uint8_t {
        subsumes = 0, // ε₁ ⊑ ε₂ provable
        not_subsumes = 1, // refuted (concrete leak, or open ε₁ into closed ε₂)
        deferred = 2, // distinct symbolic tails — needs SMT
    };

    // ============================================================================
    // subsumes(sub, sup) — is every effect of `sub` permitted by `sup`?
    // ============================================================================

    [[nodiscard]] inline row_subsume_result
    subsumes(const effect_row_node& sub, const effect_row_node& sup) noexcept {
        // Concrete effects of sub must be permitted by sup's concrete set,
        // UNLESS sup is open (its tail absorbs the excess).
        const effect_mask leaked = sub.concrete & ~sup.concrete;
        if (leaked != 0 && !sup.is_open()) return row_subsume_result::not_subsumes;

        if (sub.is_open()) {
            // sub's tail must be absorbable by sup's tail.
            if (!sup.is_open()) return row_subsume_result::not_subsumes; // leak
            if (sub.tail_var == sup.tail_var) return row_subsume_result::subsumes;
            return row_subsume_result::deferred; // distinct symbolic tails → SMT
        }
        return row_subsume_result::subsumes;
    }

    // Arena-keyed overload.
    [[nodiscard]] inline row_subsume_result
    subsumes(const effect_row_arena& arena, effect_row_ref sub, effect_row_ref sup) noexcept {
        const effect_row_node* a = arena.get(sub);
        const effect_row_node* b = arena.get(sup);
        if (!a || !b) return row_subsume_result::deferred;
        return subsumes(*a, *b);
    }

    // ============================================================================
    // kEffectSubKind — ext-band constraint "row A ⊑ row B" (routes to rule solver;
    // symbolic-tail residual falls through to SMT band). extension band +21.
    // ============================================================================

    inline constexpr constraint_kind kEffectSubKind =
        static_cast<constraint_kind>(kConstraintKindExtensionBase + 21);

    // Payload packs the two effect_row_ref indices: low32 = sub, high32 = sup.
    [[nodiscard]] inline constraint
    make_effect_bound_constraint(effect_row_ref sub, effect_row_ref sup) noexcept {
        constraint c;
        c.kind = kEffectSubKind;
        c.payload = (static_cast<std::uint64_t>(sub.index) & 0xFFFFFFFFULL) |
            (static_cast<std::uint64_t>(sup.index) << 32);
        return c;
    }

    // ============================================================================
    // effect_row_solver — constraint_solver for kEffectSubKind over an arena.
    // ============================================================================

    class effect_row_solver {
    public:
        effect_row_solver() = default;
        explicit effect_row_solver(effect_row_arena& arena) noexcept : arena_(&arena) {}

        [[nodiscard]] bool handles(constraint_kind k) const noexcept {
            return k == kEffectSubKind;
        }

        [[nodiscard]] solve_result solve(std::span<const constraint> batch,
                                         solve_context /*ctx*/) {
            solve_result r;
            if (!arena_) {
                r.status = solve_status::deferred;
                return r;
            }

            for (const constraint& c : batch) {
                if (!handles(c.kind)) continue;
                const auto sub = resolve(static_cast<std::uint32_t>(c.payload & 0xFFFFFFFFULL));
                const auto sup = resolve(static_cast<std::uint32_t>(c.payload >> 32));
                if (sub.is_null() || sup.is_null()) {
                    r.status = join_status(r.status, solve_status::deferred);
                    continue;
                }
                switch (subsumes(*arena_, sub, sup)) {
                case row_subsume_result::subsumes:
                    r.status = join_status(r.status, solve_status::solved);
                    break;
                case row_subsume_result::not_subsumes:
                    r.status = solve_status::unsatisfiable;
                    r.diagnostics.push_back(
                        solver_diagnostic{"effect row leaks: ε₁ ⋢ ε₂", constraint_ref{}});
                    return r;
                case row_subsume_result::deferred:
                    r.status = join_status(r.status, solve_status::deferred);
                    break;
                }
            }
            return r;
        }

    private:
        [[nodiscard]] effect_row_ref resolve(std::uint32_t idx) const noexcept {
            if (idx == 0) return effect_row_ref{};
            const effect_row_ref candidate{idx, 1};
            return arena_->get(candidate) ? candidate : effect_row_ref{};
        }

        effect_row_arena* arena_ = nullptr;
    };

    static_assert(constraint_solver<effect_row_solver>);
} // namespace vakya::types
