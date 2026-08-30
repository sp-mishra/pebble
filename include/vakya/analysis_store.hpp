#pragma once

// =============================================================================
// vakya/analysis_store.hpp — schema'd semantic analysis sidecar (opt-in)
//
// C++23, header-only, no virtual, no macros.
// Namespace: vakya::types
//
// Replaces ad-hoc property_store entries with a structured schema so
// downstream (query engine, LSP) reads a known record shape, not loose keys.
//
// analysis_record = {
//   type_ref       type;           // inferred / checked type
//   type_ref       shape;          // inferred shape (shape_type_tag, or null)
//   effect_mask    effects;        // aggregated effect obligations
//   capability_mask caps;          // required capability obligations
//   proof_status   proofs;         // result from verify.hpp
//   uint64_t       trait_set;      // bitmask of satisfied trait ids (fast path)
//   uint64_t       features;       // lithe-style feature vector (observability)
// }
//
// Keyed by structural_hash (uint64_t). Backing follows property_store's
// shared_mutex + update_for discipline exactly — no new concurrency machinery.
// Unpopulated fields cost nothing (zero values).
//
// Dependencies: vakya/types.hpp, vakya/types/capability.hpp, vakya/types/effect.hpp
// =============================================================================

#include "vakya/types/capability.hpp"
#include "vakya/types/effect.hpp"
#include "vakya/types/opt_handles.hpp"

#include <cstdint>
#include <shared_mutex>
#include <type_traits>
#include <unordered_map>

namespace vakya::types {
    // ============================================================================
    // proof_status — result of formal verification for this node
    // ============================================================================

    enum class proof_status : std::uint8_t {
        unknown = 0, // not yet verified
        proven = 1, // all obligations proven by Tarka
        refuted = 2, // at least one obligation falsified (counter-model exists)
        deferred = 3, // SMT not available; obligations deferred
    };

    // ============================================================================
    // analysis_record — fixed-schema value stored per AST node
    // ============================================================================

    struct analysis_record {
        type_ref type{}; // inferred/checked type handle (null if unknown)
        type_ref shape{}; // shape type handle (null if none)
        effect_mask effects = 0; // aggregate effect obligations
        capability_mask caps = 0; // required capability obligations
        proof_status proofs = proof_status::unknown;
        std::uint64_t trait_set = 0; // bitmask of satisfied trait stable_ids
        std::uint64_t features = 0; // observability feature vector

        // Counter-model payload from Tarka (populated when proofs == refuted).
        // Stored as an opaque uint64_t to avoid pulling in Tarka headers here.
        std::uint64_t refutation_payload = 0;

        // Owning domain id (0 == sutra::domain::kScalarDomain by construction).
        // Bare uint32 so Vakya stays ignorant of sutra::domain (downward-only layering).
        std::uint32_t domain = 0;

        // ------------------------------------------------------------------------
        // Semantic-optimization block (opt-in; all default null/unknown/0).
        // Populated by the optimization-layer headers; payloads live in per-phase
        // side-arenas keyed by these handles. Leaving them at defaults costs nothing.
        // ------------------------------------------------------------------------
        region_ref region{};            // aliasing region of this node's value
        effect_row_ref effect_row{};    // polymorphic effect row (concrete+tail)
        rw_summary_ref rw{};            // read/write region summary
        typestate_id state = kNoTypestate;   // affine typestate protocol state
        std::uint16_t simd_width = 0;   // synthesized SIMD lane count (0 = none)
        std::uint16_t tile_hint = 0;    // synthesized loop-tile size (0 = none)
        execution_affinity affinity = execution_affinity::unknown; // scheduling hint
        cost_class cost = cost_class::unknown; // compile-time cost lattice band
        std::uint32_t cert_id = 0;      // rewrite_certificate index (0 = none)

        [[nodiscard]] bool has_type() const noexcept { return !type.is_null(); }
        [[nodiscard]] bool has_shape() const noexcept { return !shape.is_null(); }
        [[nodiscard]] bool is_proven() const noexcept { return proofs == proof_status::proven; }
    };

    // analysis_record must stay a trivially-copyable POD: the store copies records
    // by value under lock, and downstream sidecars memcpy them. Every optimization
    // field is a handle / enum / integer, so this holds.
    static_assert(std::is_trivially_copyable_v<analysis_record>);

    // ============================================================================
    // analysis_store — thread-safe map from structural_hash → analysis_record
    //
    // API mirrors property_store:
    //   find(hash)          → const analysis_record* (shared lock, nullptr if absent)
    //   ensure(hash)        → analysis_record& (exclusive lock; valid until next ensure/clear)
    //   update(hash, fn)    → void (exclusive lock held for entire fn — safe concurrent update)
    //   update_for(expr,fn) → void (hashes expr then calls update)
    //   clear()             → void
    //   size()              → std::size_t
    // ============================================================================

    class analysis_store {
    public:
        analysis_store() = default;

        // -------------------------------------------------------------------------
        // find — shared lock, returns nullptr if absent
        // -------------------------------------------------------------------------
        [[nodiscard]] const analysis_record* find(std::uint64_t hash) const noexcept {
            std::shared_lock lock{mutex_};
            auto it = map_.find(hash);
            return (it != map_.end()) ? &it->second : nullptr;
        }

        // -------------------------------------------------------------------------
        // ensure — exclusive lock; creates default record if absent.
        // WARNING: reference valid only until next ensure() or clear() (rehash hazard).
        // Prefer update() for concurrent mutation.
        // -------------------------------------------------------------------------
        analysis_record& ensure(std::uint64_t hash) {
            std::unique_lock lock{mutex_};
            return map_[hash];
        }

        // -------------------------------------------------------------------------
        // update — holds exclusive lock for entire fn; safe for concurrent mutation
        // -------------------------------------------------------------------------
        template <class Fn>
        void update(std::uint64_t hash, Fn&& fn) {
            std::unique_lock lock{mutex_};
            fn(map_[hash]);
        }

        // -------------------------------------------------------------------------
        // update_for — expression-keyed update
        // -------------------------------------------------------------------------
        template <class Expr, class Fn>
        void update_for(const Expr& expr, Fn&& fn) {
            update(vakya::structural_hash(expr), std::forward<Fn>(fn));
        }

        // -------------------------------------------------------------------------
        // find_for — expression-keyed find
        // -------------------------------------------------------------------------
        template <class Expr>
        [[nodiscard]] const analysis_record* find_for(const Expr& expr) const noexcept {
            return find(vakya::structural_hash(expr));
        }

        // -------------------------------------------------------------------------
        // contains
        // -------------------------------------------------------------------------
        [[nodiscard]] bool contains(std::uint64_t hash) const noexcept {
            std::shared_lock lock{mutex_};
            return map_.count(hash) > 0;
        }

        void clear() {
            std::unique_lock lock{mutex_};
            map_.clear();
        }

        [[nodiscard]] std::size_t size() const noexcept {
            std::shared_lock lock{mutex_};
            return map_.size();
        }

        // Iterate all (hash, record) pairs under a shared lock.
        // fn: (uint64_t hash, const analysis_record& rec) -> void
        template <class Fn>
        void discover_impl(Fn&& fn) const {
            std::shared_lock lock{mutex_};
            for (const auto& [hash, rec] : map_) {
                fn(hash, rec);
            }
        }

    private:
        mutable std::shared_mutex mutex_;
        std::unordered_map<std::uint64_t, analysis_record> map_;
    };
} // namespace vakya::types
