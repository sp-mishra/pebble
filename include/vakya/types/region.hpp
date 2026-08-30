#pragma once

// =============================================================================
// vakya/types/region.hpp — ownership / aliasing region algebra (opt-in)
//
// C++23, header-only, no virtual, no macros.
// Namespace: vakya::types
//
// A region names an abstract memory location. Regions form a projection tree:
//   root(id)                    — a fresh allocation / parameter / binding
//   field(parent, selector)     — parent.selector
//   index(parent, offset)       — parent[offset]   (offset kInvalidIndex = symbolic)
//
// Two regions may_alias() iff neither is a disjoint projection of the other and
// they share (or could share) a root. Concrete disjointness (distinct roots, or
// distinct constant field/index off a common parent) is decided in O(α(n)) via a
// union_find over region roots — the same primitive unification uses. Symbolic
// index disjointness is deferred to SMT by vakya/alias.hpp.
//
// region_ref is minted here (tag declared in opt_handles.hpp) and stored in
// analysis_record::region. The arena interns structurally-equal regions to one
// handle, mirroring type_arena's intern discipline.
//
// Reuse: containers::slot_map, containers::union_find, generational_handle.
// No new generic mechanism — this is a thin domain arena over existing pieces.
//
// Dependencies: vakya/types/opt_handles.hpp, containers/associative/slot_map.hpp,
//               containers/union_find.hpp, containers/handle/generational_handle.hpp
// =============================================================================

#include "vakya/types/opt_handles.hpp"
#include "containers/associative/slot_map.hpp"
#include "containers/union_find.hpp"
#include "containers/handle/generational_handle.hpp"

#include <cstdint>
#include <limits>
#include <unordered_map>

namespace vakya::types {
    // ============================================================================
    // region_kind
    // ============================================================================

    enum class region_kind : std::uint8_t {
        root = 0,  // fresh allocation / parameter / binding
        field = 1, // parent.selector
        index = 2, // parent[offset]
    };

    inline constexpr std::uint64_t kSymbolicIndex = std::numeric_limits<std::uint64_t>::max();

    // ============================================================================
    // region_node — flat interned node (payloads are hashes / small integers)
    // ============================================================================

    struct region_node {
        region_kind kind = region_kind::root;
        region_ref parent{};        // null for root
        std::uint64_t selector = 0; // field name hash (field) / index offset (index)
        std::uint64_t root_id = 0;  // unique id for root regions (0 for projections)

        [[nodiscard]] bool operator==(const region_node& o) const noexcept {
            return kind == o.kind && parent == o.parent &&
                selector == o.selector && root_id == o.root_id;
        }
    };

    [[nodiscard]] inline std::uint64_t region_hash(const region_node& n) noexcept {
        constexpr std::uint64_t kBasis = 14695981039346656037ULL;
        constexpr std::uint64_t kPrime = 1099511628211ULL;
        auto mix = [&](std::uint64_t h, std::uint64_t v) noexcept {
            for (int b = 0; b < 8; ++b) { h ^= (v & 0xFFu); h *= kPrime; v >>= 8; }
            return h;
        };
        std::uint64_t h = kBasis;
        h = mix(h, static_cast<std::uint64_t>(n.kind));
        h = mix(h, static_cast<std::uint64_t>(n.parent.index));
        h = mix(h, static_cast<std::uint64_t>(n.parent.generation));
        h = mix(h, n.selector);
        h = mix(h, n.root_id);
        return h ? h : 1;
    }

    // ============================================================================
    // region_arena — interns region nodes; owns the alias-class union_find.
    //
    // Alias classes: each interned region maps to one union_find id. unite_alias()
    // merges two regions' classes (used when an assignment / borrow proves they may
    // share storage). aliases() answers "same class?" in O(α(n)).
    // ============================================================================

    class region_arena {
    public:
        region_arena() = default;

        // Mint a fresh root region with a unique root_id. Distinct roots are
        // disjoint by construction (own union_find class each).
        [[nodiscard]] region_ref fresh_region() {
            region_node n;
            n.kind = region_kind::root;
            n.root_id = ++root_counter_;
            return intern(n);
        }

        // parent.selector  (selector = field name hash).
        [[nodiscard]] region_ref project_field(region_ref parent, std::uint64_t name_hash) {
            region_node n;
            n.kind = region_kind::field;
            n.parent = parent;
            n.selector = name_hash;
            return intern(n);
        }

        // parent[offset]  (offset kSymbolicIndex => symbolic, disjointness → SMT).
        [[nodiscard]] region_ref project_index(region_ref parent, std::uint64_t offset) {
            region_node n;
            n.kind = region_kind::index;
            n.parent = parent;
            n.selector = offset;
            return intern(n);
        }

        [[nodiscard]] const region_node* get(region_ref r) const noexcept {
            return store_.find(r);
        }

        // Root region reachable by walking parents; null stays null.
        [[nodiscard]] region_ref root_of(region_ref r) const noexcept {
            region_ref cur = r;
            for (const region_node* n = get(cur); n && n->kind != region_kind::root;
                 n = get(cur)) {
                cur = n->parent;
            }
            return cur;
        }

        // Merge alias classes of a and b (an assignment / borrow may alias them).
        void unite_alias(region_ref a, region_ref b) {
            const std::uint32_t ia = uf_id(a), ib = uf_id(b);
            if (ia == containers::union_find<>::kInvalidId ||
                ib == containers::union_find<>::kInvalidId)
                return;
            (void)uf_.unite(ia, ib);
        }

        // Same alias class? (proven aliasing). Distinct classes => not proven-aliased,
        // but see may_alias() in alias.hpp for the disjointness decision.
        [[nodiscard]] bool aliases(region_ref a, region_ref b) noexcept {
            const std::uint32_t ia = uf_id(a), ib = uf_id(b);
            if (ia == containers::union_find<>::kInvalidId ||
                ib == containers::union_find<>::kInvalidId)
                return false;
            return uf_.connected(ia, ib);
        }

        [[nodiscard]] std::size_t size() const noexcept { return store_.size(); }

    private:
        [[nodiscard]] region_ref intern(const region_node& n) {
            const std::uint64_t h = region_hash(n);
            if (const auto it = intern_.find(h); it != intern_.end()) {
                if (const region_node* e = store_.find(it->second); e && *e == n)
                    return it->second;
            }
            const region_ref ref = store_.insert(n);
            intern_.emplace(h, ref);
            // Assign a fresh alias-class id for this region.
            const std::uint32_t id = static_cast<std::uint32_t>(uf_.make_set());
            handle_to_uf_.emplace(ref, id);
            return ref;
        }

        // union_find id for a region handle (kInvalidId if unknown).
        [[nodiscard]] std::uint32_t uf_id(region_ref r) const noexcept {
            const auto it = handle_to_uf_.find(r);
            return it == handle_to_uf_.end()
                       ? containers::union_find<>::kInvalidId
                       : it->second;
        }

        containers::slot_map<region_node, region_ref> store_;
        std::unordered_map<std::uint64_t, region_ref> intern_;
        std::unordered_map<region_ref, std::uint32_t> handle_to_uf_;
        containers::union_find<> uf_;
        std::uint64_t root_counter_ = 0;
    };

    // ============================================================================
    // regions_syntactically_disjoint — pure structural fast path (no arena state
    // mutation). Decides disjointness for two CONCRETE regions:
    //   - distinct roots            → disjoint
    //   - a.field(x) vs a.field(y), x != y (both concrete) → disjoint
    //   - a[i] vs a[j], i != j (both concrete, non-symbolic) → disjoint
    //   - a vs a.field / a[i]       → NOT disjoint (nested — may overlap)
    // Returns false when it cannot prove disjointness (caller falls back to SMT).
    // ============================================================================

    [[nodiscard]] inline bool
    regions_syntactically_disjoint(const region_arena& arena,
                                   region_ref a, region_ref b) noexcept {
        if (a.is_null() || b.is_null()) return false;
        if (a == b) return false;

        const region_ref ra = arena.root_of(a);
        const region_ref rb = arena.root_of(b);
        if (ra.is_null() || rb.is_null()) return false;

        const region_node* rna = arena.get(ra);
        const region_node* rnb = arena.get(rb);
        if (!rna || !rnb) return false;

        // Distinct roots are disjoint by construction.
        if (rna->root_id != rnb->root_id) return true;

        // Same root: disjoint only if a common ancestor splits into distinct
        // concrete selectors of the same kind, with neither a prefix of the other.
        const region_node* na = arena.get(a);
        const region_node* nb = arena.get(b);
        if (!na || !nb) return false;
        if (na->kind == region_kind::root || nb->kind == region_kind::root) return false;
        if (na->parent == nb->parent && na->kind == nb->kind) {
            if (na->kind == region_kind::index &&
                (na->selector == kSymbolicIndex || nb->selector == kSymbolicIndex))
                return false; // symbolic index — defer to SMT
            return na->selector != nb->selector;
        }
        return false;
    }
} // namespace vakya::types
