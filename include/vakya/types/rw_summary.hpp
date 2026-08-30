#pragma once

// =============================================================================
// vakya/types/rw_summary.hpp — read/write summaries + conflict prediction
//                              (opt-in)
//
// C++23, header-only, no virtual, no macros.
// Namespace: vakya::types
//
// A read/write summary records the region set a computation reads and the set it
// writes. Two summaries CONFLICT iff one's write set overlaps the other's read or
// write set (write–write or read–write hazard). If every cross pair is provably
// disjoint (region.hpp root check), the two computations commute / can run in
// parallel / a transaction pair never aborts — a NoConflict fact.
//
// rw_summary is interned to an rw_summary_ref (tag in opt_handles.hpp) stored in
// analysis_record::rw. Region sets are sorted region_ref vectors (small; interned
// structurally like every other optimization-layer arena).
//
// predict_conflict(arena_regions, A, B) decides here where all pairs are concrete;
// any symbolic-index pair defers to the SMT band via make_noconflict_obligation.
//
// Dependencies: vakya/types/region.hpp, vakya/constraints.hpp,
//               containers/associative/slot_map.hpp
// =============================================================================

#include "vakya/types/region.hpp"
#include "vakya/constraints.hpp"
#include "containers/associative/slot_map.hpp"
#include "containers/dynamic/SmallVector.hpp"

#include <cstdint>
#include <span>
#include <unordered_map>

namespace vakya::types {
    // ============================================================================
    // rw_summary — read set + write set of region handles.
    //
    // Sets are kept sorted-unique (by handle index) so structural equality and
    // hashing are order-independent. Small inline storage; no allocation until a
    // set exceeds the inline capacity.
    // ============================================================================

    struct rw_summary {
        containers::dynamic::SmallVector<region_ref, 4> reads;
        containers::dynamic::SmallVector<region_ref, 4> writes;

        [[nodiscard]] bool operator==(const rw_summary& o) const noexcept {
            if (reads.size() != o.reads.size() || writes.size() != o.writes.size())
                return false;
            for (std::size_t i = 0; i < reads.size(); ++i)
                if (reads[i] != o.reads[i]) return false;
            for (std::size_t i = 0; i < writes.size(); ++i)
                if (writes[i] != o.writes[i]) return false;
            return true;
        }
    };

    // Insert keeping sorted-unique by (index, generation).
    inline void rw_insert(containers::dynamic::SmallVector<region_ref, 4>& set,
                          region_ref r) {
        if (r.is_null()) return;
        auto less = [](region_ref a, region_ref b) noexcept {
            return a.index < b.index ||
                (a.index == b.index && a.generation < b.generation);
        };
        std::size_t pos = 0;
        while (pos < set.size() && less(set[pos], r)) ++pos;
        if (pos < set.size() && set[pos] == r) return; // already present
        set.insert(set.begin() + static_cast<std::ptrdiff_t>(pos), r);
    }

    [[nodiscard]] inline std::uint64_t rw_summary_hash(const rw_summary& s) noexcept {
        constexpr std::uint64_t kBasis = 14695981039346656037ULL;
        constexpr std::uint64_t kPrime = 1099511628211ULL;
        std::uint64_t h = kBasis;
        auto mix = [&](region_ref r) noexcept {
            h = (h ^ r.index) * kPrime;
            h = (h ^ r.generation) * kPrime;
        };
        for (region_ref r : s.reads) mix(r);
        h = (h ^ 0x9E3779B97F4A7C15ULL) * kPrime; // separator between sets
        for (region_ref r : s.writes) mix(r);
        return h ? h : 1;
    }

    // ============================================================================
    // rw_summary_arena — interns rw_summaries to rw_summary_ref.
    // ============================================================================

    class rw_summary_arena {
    public:
        rw_summary_arena() = default;

        [[nodiscard]] rw_summary_ref intern_rw_summary(rw_summary s) {
            const std::uint64_t h = rw_summary_hash(s);
            if (const auto it = intern_.find(h); it != intern_.end()) {
                if (const rw_summary* e = store_.find(it->second); e && *e == s)
                    return it->second;
            }
            const rw_summary_ref ref = store_.insert(std::move(s));
            intern_.emplace(h, ref);
            return ref;
        }

        [[nodiscard]] const rw_summary* get(rw_summary_ref r) const noexcept {
            return store_.find(r);
        }

        [[nodiscard]] std::size_t size() const noexcept { return store_.size(); }

    private:
        containers::slot_map<rw_summary, rw_summary_ref> store_;
        std::unordered_map<std::uint64_t, rw_summary_ref> intern_;
    };

    // ============================================================================
    // conflict_result — decidable here, or deferred to SMT.
    // ============================================================================

    enum class conflict_result : std::uint8_t {
        no_conflict = 0, // every cross pair provably disjoint → commutes
        conflict = 1,    // a write overlaps a read/write (proven aliased or same)
        deferred = 2,    // a symbolic-index pair is undecidable → needs SMT
    };

    // ============================================================================
    // predict_conflict — write–write and read–write hazard check.
    //
    // For each region in A.writes vs (B.reads ∪ B.writes) and B.writes vs A.reads:
    //   - provably disjoint (regions_syntactically_disjoint / distinct roots) → skip
    //   - proven aliased or identical                                        → conflict
    //   - undecidable (symbolic index)                                       → defer
    // No conflict found and nothing deferred → no_conflict.
    // ============================================================================

    [[nodiscard]] inline conflict_result
    predict_conflict(region_arena& regions, const rw_summary& a, const rw_summary& b) {
        bool any_deferred = false;

        // A single write-vs-set hazard scan. `w` writes; each `x` is read-or-written
        // by the other party. Disjoint → safe; aliased/equal → conflict; else defer.
        auto scan = [&](std::span<const region_ref> w,
                        std::span<const region_ref> other) -> conflict_result {
            for (region_ref wr : w) {
                for (region_ref xr : other) {
                    if (wr == xr) return conflict_result::conflict;
                    if (regions_syntactically_disjoint(regions, wr, xr)) continue;
                    if (regions.aliases(wr, xr)) return conflict_result::conflict;
                    any_deferred = true; // can't decide this pair here
                }
            }
            return conflict_result::no_conflict;
        };

        // A.writes hazards against everything B touches.
        if (scan(std::span<const region_ref>{a.writes.data(), a.writes.size()},
                 std::span<const region_ref>{b.reads.data(), b.reads.size()}) ==
            conflict_result::conflict)
            return conflict_result::conflict;
        if (scan(std::span<const region_ref>{a.writes.data(), a.writes.size()},
                 std::span<const region_ref>{b.writes.data(), b.writes.size()}) ==
            conflict_result::conflict)
            return conflict_result::conflict;
        // B.writes hazards against A.reads (write–write already covered above).
        if (scan(std::span<const region_ref>{b.writes.data(), b.writes.size()},
                 std::span<const region_ref>{a.reads.data(), a.reads.size()}) ==
            conflict_result::conflict)
            return conflict_result::conflict;

        return any_deferred ? conflict_result::deferred : conflict_result::no_conflict;
    }

    [[nodiscard]] inline conflict_result
    predict_conflict(region_arena& regions, const rw_summary_arena& arena,
                     rw_summary_ref a, rw_summary_ref b) {
        const rw_summary* sa = arena.get(a);
        const rw_summary* sb = arena.get(b);
        if (!sa || !sb) return conflict_result::deferred;
        return predict_conflict(regions, *sa, *sb);
    }

    // ============================================================================
    // kNoConflictKind — ext-band constraint "summaries A and B do not conflict"
    // (routes to graph class — the region root/disjoint fast path; symbolic residual
    // falls to SMT band). extension band +24.
    // ============================================================================

    inline constexpr constraint_kind kNoConflictKind =
        static_cast<constraint_kind>(kConstraintKindExtensionBase + 24);

    // Payload packs the two rw_summary_ref indices: low32 = A, high32 = B.
    [[nodiscard]] inline constraint
    make_noconflict_obligation(rw_summary_ref a, rw_summary_ref b) noexcept {
        constraint c;
        c.kind = kNoConflictKind;
        c.payload = (static_cast<std::uint64_t>(a.index) & 0xFFFFFFFFULL) |
            (static_cast<std::uint64_t>(b.index) << 32);
        return c;
    }

    // ============================================================================
    // no_conflict_solver — constraint_solver for kNoConflictKind.
    //
    // Holds non-owning pointers to both arenas (region + rw_summary). solved when
    // predict_conflict proves no_conflict; unsatisfiable on a proven conflict;
    // deferred when any pair is symbolic (routes onward to the SMT band).
    // ============================================================================

    class no_conflict_solver {
    public:
        no_conflict_solver() = default;
        no_conflict_solver(region_arena& regions, rw_summary_arena& summaries) noexcept
            : regions_(&regions), summaries_(&summaries) {}

        [[nodiscard]] bool handles(constraint_kind k) const noexcept {
            return k == kNoConflictKind;
        }

        [[nodiscard]] solve_result solve(std::span<const constraint> batch,
                                         solve_context /*ctx*/) {
            solve_result r;
            if (!regions_ || !summaries_) { r.status = solve_status::deferred; return r; }

            for (const constraint& c : batch) {
                if (!handles(c.kind)) continue;
                const auto a = resolve(static_cast<std::uint32_t>(c.payload & 0xFFFFFFFFULL));
                const auto b = resolve(static_cast<std::uint32_t>(c.payload >> 32));
                if (a.is_null() || b.is_null()) {
                    r.status = join_status(r.status, solve_status::deferred);
                    continue;
                }
                switch (predict_conflict(*regions_, *summaries_, a, b)) {
                case conflict_result::no_conflict:
                    r.status = join_status(r.status, solve_status::solved);
                    break;
                case conflict_result::conflict:
                    r.status = solve_status::unsatisfiable;
                    r.diagnostics.push_back(
                        solver_diagnostic{"read/write summaries conflict", constraint_ref{}});
                    return r;
                case conflict_result::deferred:
                    r.status = join_status(r.status, solve_status::deferred);
                    break;
                }
            }
            return r;
        }

    private:
        [[nodiscard]] rw_summary_ref resolve(std::uint32_t idx) const noexcept {
            if (idx == 0) return rw_summary_ref{};
            const rw_summary_ref candidate{idx, 1};
            return summaries_->get(candidate) ? candidate : rw_summary_ref{};
        }

        region_arena* regions_ = nullptr;
        rw_summary_arena* summaries_ = nullptr;
    };

    static_assert(constraint_solver<no_conflict_solver>);
} // namespace vakya::types
