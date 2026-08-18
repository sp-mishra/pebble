#pragma once

// =============================================================================
// egraph.hpp — Generic Equality Saturation Engine
//
// Namespace:  egraph
// Location:   include/containers/graph/egraph.hpp
// Depends on: SmallVector, Kosha FlatHashStorage / Cache
// Lithe dep:  NONE — domain-agnostic, standalone container
//
// Provides:
//   e_class_id                                    — dense uint32 class identity
//   e_node<OpId, Payload>                         — structural representation
//   e_class<Node, ClassData>                      — equivalence set + union-find link + user data
//   e_graph<OpId, Payload, Hash, Eq, ClassData,
//           Alloc>                                — hashcons + union-find + rebuild
//   pattern_var<N>                                — typed pattern placeholder
//   egraph_rule<R,G>                              — concept for rewrite rules
//   cost_model<C, Node>                           — concept for cost models
//   egraph_model<G>                               — concept matching e_graph interface
//   commutativity / associativity / distributivity / identity_zero — rule packs
//   saturation_limits / saturation_report
//   saturate(graph, rules, limits)                — fixpoint loop with bounds
//   node_count_cost                               — default min-node cost model
//   extract_best<CostModel>(graph, root)          — bottom-up DP extraction
//
// Design:
//   - egg-style deferred rebuild(): batch congruence repair after merges
//   - path compression + union-by-rank in union-find
//   - hashcons via Kosha FlatHashStorage (Robin-Hood, cache-line-friendly)
//   - SmallVector inline caps for common ≤4-ary nodes and worklists
//   - ClassData template param: user-defined data per e-class (zero-cost monostate)
//   - Alloc template param: PMR / smriti arena support for all internal vectors
//   - live_classes() range: filter view over root e-classes only
//   - reserve(): upfront capacity hint to amortise rehash + vector growth
//   - SIMD-accelerated children hashing when <hwy/highway.h> is available
//   - all rule packs and cost models are empty types (zero-overhead)
// =============================================================================

#include "containers/dynamic/SmallVector.hpp"
#include "containers/cache/kosha.hpp"

#include <vector>
#include <cstdint>
#include <cstddef>
#include <concepts>
#include <span>
#include <utility>
#include <functional>
#include <optional>
#include <algorithm>
#include <tuple>
#include <limits>
#include <ranges>
#include <memory>

#if __has_include(<hwy/highway.h>)
#  include <hwy/highway.h>
#  define EGRAPH_HAS_HIGHWAY 1
#endif

namespace egraph {
    // =============================================================================
    // e_class_id — dense, mutable-after-merge identifier
    // =============================================================================

    using e_class_id = std::uint32_t;
    inline constexpr e_class_id kInvalidClassId = std::numeric_limits<e_class_id>::max();

    // =============================================================================
    // e_node<OpId, Payload> — hashcons key
    //
    // OpId    — op identity (Lithe: tag_descriptor::stable_id; generic: any integral)
    // Payload — leaf value contribution (Lithe: structural_payload_hash result;
    //           0 for interior nodes)
    // =============================================================================

    template <class OpId = std::size_t, class Payload = std::size_t>
    struct e_node {
        OpId op;
        containers::dynamic::SmallVector<e_class_id, 4 * sizeof(e_class_id)> children; // inline for ≤4 children
        Payload payload{};

        [[nodiscard]] bool operator==(const e_node& o) const noexcept {
            return op == o.op
                && payload == o.payload
                && std::ranges::equal(children, o.children);
        }
    };

    // =============================================================================
    // e_class<Node, ClassData> — equivalence set + union-find bookkeeping
    //
    // ClassData — user-defined data attached to each e-class.
    //   Defaults to std::monostate (zero-cost, [[no_unique_address]]).
    // =============================================================================

    template <class Node, class ClassData = std::monostate>
    struct e_class {
        containers::dynamic::SmallVector<Node, 2 * sizeof(Node)> nodes; // equivalent forms (inline 2)
        e_class_id parent;
        std::uint32_t rank{0};
        [[no_unique_address]] ClassData data{};

        explicit e_class(const e_class_id self, Node n)
            : parent(self) {
            nodes.push_back(std::move(n));
        }
    };

    // =============================================================================
    // default_enode_hash — structural hash for e_node<OpId,Payload>
    //
    // When <hwy/highway.h> is available, SIMD-accelerates hashing of the children
    // array for nodes with ≥8 children (typical break-even on AArch64/x86 NEON/AVX2).
    // Falls back to scalar for smaller arities — no overhead for the common ≤4 case.
    // =============================================================================

    namespace detail {
#if defined(EGRAPH_HAS_HIGHWAY)
        // SIMD-accelerated XOR-shift mix over a span of uint32 children.
        // Uses 64-bit lanes: pairs of children are combined per lane.
        inline std::size_t hash_children_simd(const e_class_id* data, const std::size_t n) noexcept {
            namespace hn = hwy::HWY_NAMESPACE;
            const hn::ScalableTag<std::uint64_t> d;
            const std::size_t lanes = hn::Lanes(d);

            constexpr std::uint64_t kMix = 0x9e3779b97f4a7c15ULL;
            auto acc = hn::Zero(d);

            // Process pairs of uint32 children as uint64 (two u32 per lane).
            const std::size_t pairs = n / 2;
            std::size_t i = 0;
            for (; i + lanes <= pairs; i += lanes) {
                auto v = hn::Load(d, reinterpret_cast<const std::uint64_t*>(data + 2 * i));
                v = hn::Xor(v, hn::ShiftRight<17>(v));
                v = hn::Mul(v, hn::Set(d, kMix));
                acc = hn::Add(acc, v);
            }

            // Horizontal fold of SIMD accumulator.
            std::uint64_t h = hn::ReduceSum(d, acc);

            // Scalar tail for remaining children.
            for (std::size_t j = i * 2; j < n; ++j) {
                h ^= static_cast<std::uint64_t>(data[j]) * kMix + 0x9e3779b9u + (h << 6u) + (h >> 2u);
            }
            return static_cast<std::size_t>(h);
        }

        inline constexpr std::size_t kSimdChildThreshold = 8;
#endif

        inline std::size_t hash_children_scalar(const e_class_id* data, const std::size_t n,
                                                const std::size_t seed) noexcept {
            std::size_t h = seed;
            for (std::size_t i = 0; i < n; ++i)
                h ^= std::hash<e_class_id>{}(data[i]) + 0x9e3779b9u + (h << 6u) + (h >> 2u);
            return h;
        }
    } // namespace detail

    template <class OpId, class Payload>
    struct default_enode_hash {
        [[nodiscard]] std::size_t operator()(const e_node<OpId, Payload>& n) const noexcept {
            std::size_t h = std::hash<OpId>{}(n.op);
            const auto* cd = n.children.data();
            const std::size_t nc = n.children.size();

#if defined(EGRAPH_HAS_HIGHWAY)
            if (nc >= detail::kSimdChildThreshold) {
                h ^= detail::hash_children_simd(cd, nc);
            }
            else {
                h = detail::hash_children_scalar(cd, nc, h);
            }
#else
            h = detail::hash_children_scalar(cd, nc, h);
#endif
            h ^= std::hash<Payload>{}(n.payload) + 0x9e3779b9u + (h << 6u) + (h >> 2u);
            return h;
        }
    };

    template <class OpId, class Payload>
    struct default_enode_eq {
        [[nodiscard]] bool operator()(const e_node<OpId, Payload>& a,
                                      const e_node<OpId, Payload>& b) const noexcept {
            return a == b;
        }
    };

    // =============================================================================
    // e_graph<OpId, Payload, Hash, Eq, ClassData, Alloc>
    //
    // Core engine:
    //   add(node)      — hashcons insert; returns canonical e_class_id
    //   find(id)       — path-compressed root
    //   merge(a, b)    — union-by-rank; marks dirty_ worklist
    //   rebuild()      — egg-style batch congruence closure repair
    //   reserve(n)     — hint: pre-allocate for n e-classes
    //   live_classes() — range of (id, class) pairs for root classes only
    //   class_count_live() — count of root (non-merged) e-classes
    //   get_class_data / set_class_data — user data on e-classes
    //
    // Template params:
    //   ClassData — user data per e-class (zero-cost when std::monostate)
    //   Alloc     — allocator for internal vectors (default: std::allocator<char>)
    //               Use std::pmr::polymorphic_allocator or SmritiAllocator for arena.
    //
    // Invariant: after rebuild() the hashcons is consistent with the union-find.
    // =============================================================================

    template <
        class OpId = std::size_t,
        class Payload = std::size_t,
        class Hash = default_enode_hash<OpId, Payload>,
        class Eq = default_enode_eq<OpId, Payload>,
        class ClassData = std::monostate,
        class Alloc = std::allocator<char>>
    class e_graph {
    public:
        using node_t = e_node<OpId, Payload>;
        using class_t = e_class<node_t, ClassData>;

        using alloc_traits = std::allocator_traits<Alloc>;
        template <class T>
        using rebound_alloc = alloc_traits::template rebind_alloc<T>;

        e_graph() = default;

        explicit e_graph(Alloc alloc)
            : classes_(rebound_alloc<class_t>(alloc))
              , dirty_(rebound_alloc<e_class_id>(alloc))
              , parent_map_(rebound_alloc<containers::dynamic::SmallVector<e_class_id, 4 * sizeof(e_class_id)>>(alloc))
              , alloc_(alloc) {}

        // -------------------------------------------------------------------------
        // reserve — pre-allocate storage for n_classes e-classes to amortise
        // vector growth and hashcons rehash across bulk insertions.
        // -------------------------------------------------------------------------
        void reserve(std::size_t n_classes) {
            classes_.reserve(n_classes);
            parent_map_.reserve(n_classes);
        }

        // -------------------------------------------------------------------------
        // add — insert a node; returns the canonical e_class_id (path-compressed root)
        // If an equivalent node already exists, returns its class id without insert.
        // -------------------------------------------------------------------------
        [[nodiscard]] e_class_id add(node_t n) {
            for (auto& c : n.children) c = find(c);

            if (const e_class_id* existing = hashcons_.find(n))
                return find(*existing);

            const auto id = static_cast<e_class_id>(classes_.size());
            parent_map_.emplace_back();
            hashcons_.insert(n, id);
            classes_.emplace_back(id, n);

            for (auto c : n.children) {
                const e_class_id cr = find(c);
                if (cr < parent_map_.size())
                    parent_map_[cr].push_back(id);
            }
            return id;
        }

        // -------------------------------------------------------------------------
        // find — path-splitting union-find root (const, via mutable storage)
        // -------------------------------------------------------------------------
        [[nodiscard]] e_class_id find(e_class_id id) const {
            while (classes_[id].parent != id) {
                e_class_id parent = classes_[id].parent;
                if (classes_[parent].parent != parent)
                    classes_[id].parent = classes_[parent].parent;
                id = classes_[id].parent;
            }
            return id;
        }

        // -------------------------------------------------------------------------
        // is_root — true iff id is its own union-find root (live class)
        // -------------------------------------------------------------------------
        [[nodiscard]] bool is_root(e_class_id id) const noexcept {
            return id < classes_.size() && classes_[id].parent == id;
        }

        // -------------------------------------------------------------------------
        // merge — union by rank; returns true if anything changed
        // -------------------------------------------------------------------------
        [[nodiscard]] bool merge(e_class_id a, e_class_id b) {
            a = find(a);
            b = find(b);
            if (a == b) return false;

            if (classes_[a].rank < classes_[b].rank) std::swap(a, b);
            classes_[b].parent = a;
            if (classes_[a].rank == classes_[b].rank) ++classes_[a].rank;

            auto& loser_nodes = classes_[b].nodes;
            for (auto& n : loser_nodes)
                classes_[a].nodes.push_back(std::move(n));
            loser_nodes.clear();

            if (b < parent_map_.size() && a < parent_map_.size()) {
                for (auto p : parent_map_[b])
                    parent_map_[a].push_back(p);
                parent_map_[b].clear();
            }

            dirty_.push_back(a);
            return true;
        }

        // -------------------------------------------------------------------------
        // rebuild — egg-style batch congruence closure
        //
        // After merges, parent e-nodes may now reference classes that have been
        // collapsed.  rebuild() re-canonicalizes every e-node in dirty classes and
        // re-inserts into hashcons, merging any newly congruent classes.
        // Must be called before the next round of rule applications.
        //
        // Parent list iteration uses a size snapshot to avoid copies:
        // any new entries appended during the inner merge won't be visited
        // until the next dirty flush, which is correct (egg-style).
        // -------------------------------------------------------------------------
        void rebuild() {
            while (!dirty_.empty()) {
                containers::dynamic::SmallVector<e_class_id, 32 * sizeof(e_class_id)> todo;
                for (auto id : dirty_) todo.push_back(find(id));
                dirty_.clear();

                for (auto id : todo) {
                    if (id >= parent_map_.size()) continue;
                    // Snapshot size: new parents appended during inner merges are
                    // intentionally deferred to the next round (correct egg semantics).
                    const std::size_t nparents = parent_map_[id].size();
                    for (std::size_t pi = 0; pi < nparents; ++pi) {
                        const e_class_id par_id = parent_map_[id][pi];
                        const e_class_id par_root = find(par_id);
                        for (auto& n : classes_[par_root].nodes) {
                            hashcons_.erase(n);
                            for (auto& c : n.children) c = find(c);
                            if (const e_class_id* existing_ptr = hashcons_.find(n)) {
                                if (const e_class_id existing = find(*existing_ptr); existing != par_root)
                                    (void)merge(existing, par_root);
                            }
                            else {
                                hashcons_.insert(n, par_root);
                            }
                        }
                    }
                }
            }
        }

        // -------------------------------------------------------------------------
        // Accessors
        // -------------------------------------------------------------------------
        [[nodiscard]] std::size_t class_count() const noexcept { return classes_.size(); }

        [[nodiscard]] std::size_t class_count_live() const noexcept {
            std::size_t n = 0;
            for (e_class_id id = 0; id < static_cast<e_class_id>(classes_.size()); ++id)
                if (classes_[id].parent == id) ++n;
            return n;
        }

        [[nodiscard]] std::size_t enode_count() const noexcept {
            std::size_t total = 0;
            for (auto& cls : classes_) total += cls.nodes.size();
            return total;
        }

        [[nodiscard]] const class_t& get_class(const e_class_id id) const { return classes_[find(id)]; }
        [[nodiscard]] class_t& get_class(const e_class_id id) { return classes_[find(id)]; }

        // Read-only view of all classes (for extraction and rule iteration).
        [[nodiscard]] const std::vector<class_t, rebound_alloc<class_t>>& classes() const noexcept {
            return classes_;
        }

        // -------------------------------------------------------------------------
        // live_classes() — lazy range of (e_class_id, const class_t&) for roots.
        // Filters out merged-away classes; avoids the repeated parent!=id guard
        // that every rule pack currently repeats.
        //
        // Returns a range of e_class_id values (roots only).
        // -------------------------------------------------------------------------
        [[nodiscard]] auto live_class_ids() const noexcept {
            return std::views::iota(e_class_id{0}, static_cast<e_class_id>(classes_.size()))
                | std::views::filter([this](e_class_id id) noexcept {
                    return classes_[id].parent == id;
                });
        }

        // -------------------------------------------------------------------------
        // ClassData accessors — zero overhead when ClassData == std::monostate
        // -------------------------------------------------------------------------
        [[nodiscard]] const ClassData& get_class_data(const e_class_id id) const {
            return classes_[find(id)].data;
        }

        ClassData& get_class_data(const e_class_id id) {
            return classes_[find(id)].data;
        }

        void set_class_data(const e_class_id id, ClassData d) {
            classes_[find(id)].data = std::move(d);
        }

    private:
        // mutable for path-splitting in const find()
        mutable std::vector<class_t, rebound_alloc<class_t>> classes_{rebound_alloc<class_t>(alloc_)};

        // hashcons: e_node → e_class_id (canonical at time of insert)
        kosha::core::FlatHashStorage<node_t, e_class_id, Hash, Eq> hashcons_{64};

        // Dirty worklist: class ids merged since last rebuild()
        std::vector<e_class_id, rebound_alloc<e_class_id>> dirty_{rebound_alloc<e_class_id>(alloc_)};

        // parent_map_[child_root] = list of class ids that have a node using child_root
        std::vector<
            containers::dynamic::SmallVector<e_class_id, 4 * sizeof(e_class_id)>,
            rebound_alloc<containers::dynamic::SmallVector<e_class_id, 4 * sizeof(e_class_id)>>
        > parent_map_{
            rebound_alloc<containers::dynamic::SmallVector<e_class_id, 4 * sizeof(e_class_id)>>(alloc_)
        };

        [[no_unique_address]] Alloc alloc_{};
    };

    // =============================================================================
    // egraph_model concept — structural concept for e_graph-like types.
    //
    // Satisfied by e_graph<...> and any adaptor wrapping it.
    // =============================================================================

    template <class G>
    concept egraph_model = requires(G g, const G cg, typename G::node_t n, e_class_id id) {
        typename G::node_t;
        typename G::class_t;
        { g.add(n) } -> std::same_as<e_class_id>;
        { cg.find(id) } -> std::same_as<e_class_id>;
        { g.merge(id, id) } -> std::same_as<bool>;
        { g.rebuild() } -> std::same_as<void>;
        { cg.class_count() } -> std::same_as<std::size_t>;
        { cg.enode_count() } -> std::same_as<std::size_t>;
        { cg.classes() };
    };

    // =============================================================================
    // pattern_var<N> — typed pattern placeholder (N = slot index)
    // =============================================================================

    template <int N>
    struct pattern_var {};

    // =============================================================================
    // egraph_rule concept
    // =============================================================================

    template <class R, class G>
    concept egraph_rule = requires(R r, G& g) {
        r.apply(g);
    };

    // =============================================================================
    // cost_model concept
    // =============================================================================

    template <class C, class Node>
    concept cost_model = requires(C c, const Node& n) {
        typename C::cost_t;
        {
            c.cost(n, std::span<const typename C::cost_t>{})
        }
        -> std::same_as<typename C::cost_t>;
    };

    // =============================================================================
    // saturation_limits / saturation_report
    // =============================================================================

    struct saturation_limits {
        std::size_t max_iters = 30;
        std::size_t max_enodes = 100'000;
        std::size_t max_eclasses = 50'000;
    };

    struct saturation_report {
        std::size_t iters;
        std::size_t enodes;
        std::size_t eclasses;
        std::size_t merges_fired; // total merge() calls returning true across all iters
        bool hit_limit;
        bool saturated; // true iff fixpoint reached without hitting a limit
    };

    // =============================================================================
    // saturate — egg-style fixpoint loop
    //
    // Template params:
    //   G       — e_graph specialization (satisfies egraph_model)
    //   Rules   — tuple of rule types satisfying egraph_rule<R,G>
    //
    // Loop: for each rule in tuple → apply → merge → rebuild, until fixpoint or limit.
    // Returns a saturation_report for observability.
    //
    // merges_fired: counts actual merge() calls that returned true (not rules×iters).
    // =============================================================================

    namespace detail {
        // Counting merge wrapper — wraps G and intercepts merge() to tally firings.
        template <class G>
        struct merge_counting_wrapper {
            G& graph;
            std::size_t fired{0};

            // Forward all reads directly.
            [[nodiscard]] e_class_id find(e_class_id id) const { return graph.find(id); }
            [[nodiscard]] e_class_id add(G::node_t n) { return graph.add(std::move(n)); }

            [[nodiscard]] bool merge(e_class_id a, e_class_id b) {
                const bool changed = graph.merge(a, b);
                if (changed) ++fired;
                return changed;
            }

            void rebuild() { graph.rebuild(); }

            [[nodiscard]] std::size_t class_count() const { return graph.class_count(); }
            [[nodiscard]] std::size_t enode_count() const { return graph.enode_count(); }
            [[nodiscard]] const auto& classes() const { return graph.classes(); }
            [[nodiscard]] bool is_root(e_class_id id) const { return graph.is_root(id); }
            [[nodiscard]] auto live_class_ids() const { return graph.live_class_ids(); }

            using node_t = G::node_t;
            using class_t = G::class_t;
        };

        template <class G, class... Rs>
        void apply_rules(G& g, std::tuple<Rs...>& rules) {
            std::apply([&](auto&... r) { (r.apply(g), ...); }, rules);
        }
    } // namespace detail

    template <class G, class... Rules>
    [[nodiscard]] saturation_report saturate(
        G& graph,
        std::tuple<Rules...> rules,
        const saturation_limits limits = {}) {
        std::size_t iters = 0;
        std::size_t total_merges = 0;
        bool hit = false;

        for (; iters < limits.max_iters; ++iters) {
            const std::size_t nodes_before = graph.enode_count();
            const std::size_t classes_before = graph.class_count();

            // Wrap graph to count actual merges fired this iteration.
            detail::merge_counting_wrapper<G> wrapper{graph};
            detail::apply_rules(wrapper, rules);
            graph.rebuild();
            total_merges += wrapper.fired;

            const std::size_t nodes_after = graph.enode_count();
            const std::size_t classes_after = graph.class_count();

            if (nodes_after > limits.max_enodes || classes_after > limits.max_eclasses) {
                hit = true;
                ++iters;
                break;
            }

            if (nodes_after == nodes_before && classes_after == classes_before) {
                ++iters;
                break;
            }
        }

        return saturation_report{
            .iters = iters,
            .enodes = graph.enode_count(),
            .eclasses = graph.class_count(),
            .merges_fired = total_merges,
            .hit_limit = hit,
            .saturated = !hit && iters < limits.max_iters,
        };
    }

    // =============================================================================
    // node_count_cost — default cost model: minimize number of e-nodes in the tree
    // =============================================================================

    struct node_count_cost {
        using cost_t = std::size_t;

        template <class Node>
        [[nodiscard]] cost_t cost(const Node& /*n*/,
                                  const std::span<const cost_t> child_costs) const noexcept {
            cost_t total = 1;
            for (const auto c : child_costs) total += c;
            return total;
        }
    };

    static_assert(cost_model<node_count_cost, e_node<>>);

    // =============================================================================
    // extract_best<CostModel, G> — bottom-up DP extraction
    //
    // Returns extraction_result<G,CM> with parallel vectors:
    //   best_nodes[class_id]  — the chosen e_node for each class
    //   best_costs[class_id]  — its cost
    // =============================================================================

    namespace detail {
        template <class G, class CM>
        struct extraction_result {
            using cost_t = CM::cost_t;
            using node_t = G::node_t;

            std::vector<std::optional<node_t>> best_nodes;
            std::vector<cost_t> best_costs;
        };

        template <class G, class CM>
        void extract_class(const G& g, e_class_id id, CM& model,
                           extraction_result<G, CM>& result,
                           std::vector<bool>& visiting) {
            using cost_t = CM::cost_t;
            const e_class_id root = g.find(id);

            if (result.best_nodes[root].has_value()) return;
            if (visiting[root]) return;
            visiting[root] = true;

            const auto& cls = g.get_class(root);
            bool found = false;
            cost_t best = std::numeric_limits<cost_t>::max();

            for (const auto& n : cls.nodes) {
                containers::dynamic::SmallVector<cost_t, 4 * sizeof(cost_t)> child_costs;
                bool ok = true;
                for (auto ch : n.children) {
                    extract_class(g, ch, model, result, visiting);
                    const e_class_id cr = g.find(ch);
                    if (!result.best_nodes[cr].has_value()) {
                        ok = false;
                        break;
                    }
                    child_costs.push_back(result.best_costs[cr]);
                }
                if (!ok) continue;

                cost_t c = model.cost(n, std::span<const cost_t>(child_costs.data(),
                                                                 child_costs.size()));
                if (!found || c < best) {
                    best = c;
                    result.best_nodes[root] = n;
                    result.best_costs[root] = c;
                    found = true;
                }
            }
            visiting[root] = false;
        }
    } // namespace detail

    template <class CM = node_count_cost, class G>
    [[nodiscard]] detail::extraction_result<G, CM>
    extract_best(const G& graph, e_class_id root, CM model = {}) {
        const std::size_t n = graph.class_count();
        detail::extraction_result<G, CM> result;
        result.best_nodes.resize(n);
        result.best_costs.resize(n, typename CM::cost_t{});

        std::vector<bool> visiting(n, false);
        detail::extract_class(graph, root, model, result, visiting);
        return result;
    }

    // =============================================================================
    // Built-in generic rule packs (empty types — zero overhead)
    //
    // Each pack is parametrised on an OpTraits type that provides:
    //   static constexpr OpId commutative_op — for commutativity
    //   static constexpr OpId associative_op — for associativity
    //   static constexpr OpId add_op         — for distributivity / identity_zero
    //   static constexpr OpId mul_op         — for distributivity / identity_zero
    //   static constexpr OpId zero_op        — for identity_zero
    //   static constexpr OpId one_op         — for identity_zero (mul identity)
    //   static constexpr Payload zero_payload
    //   static constexpr Payload one_payload
    //
    // All rule packs use live_class_ids() when G provides it (via if constexpr /
    // requires), else fall back to the full class scan with parent==id guard.
    // =============================================================================

    namespace detail {
        // Provides a range over live class ids, working with both the real e_graph
        // and the merge_counting_wrapper (which forwards live_class_ids).
        template <class G>
        auto live_ids(const G& g) {
            if constexpr (requires { g.live_class_ids(); }) {
                return g.live_class_ids();
            }
            else {
                return std::views::iota(e_class_id{0}, static_cast<e_class_id>(g.class_count()))
                    | std::views::filter([&g](e_class_id id) {
                        return g.classes()[id].parent == id;
                    });
            }
        }
    } // namespace detail

    template <class OpTraits, class G = void>
    struct commutativity {
        template <class AnyG>
        void apply(AnyG& g) const {
            const std::size_t snapshot = g.class_count();
            for (e_class_id id = 0; id < static_cast<e_class_id>(snapshot); ++id) {
                if (!g.is_root(id)) continue;
                for (const auto& node : g.classes()[id].nodes) {
                    if (node.op == OpTraits::commutative_op && node.children.size() == 2) {
                        auto swapped = node;
                        swapped.children[0] = node.children[1];
                        swapped.children[1] = node.children[0];
                        const e_class_id sid = g.add(swapped);
                        (void)g.merge(id, sid);
                    }
                }
            }
        }
    };

    template <class OpTraits, class G = void>
    struct associativity {
        template <class AnyG>
        void apply(AnyG& g) const {
            const std::size_t snapshot = g.class_count();
            for (e_class_id id = 0; id < static_cast<e_class_id>(snapshot); ++id) {
                if (!g.is_root(id)) continue;
                for (const auto& outer : g.classes()[id].nodes) {
                    if (outer.op != OpTraits::associative_op || outer.children.size() != 2)
                        continue;
                    const e_class_id left = g.find(outer.children[0]);
                    for (const auto& inner : g.classes()[left].nodes) {
                        if (inner.op != OpTraits::associative_op || inner.children.size() != 2)
                            continue;
                        typename AnyG::node_t right_node;
                        right_node.op = OpTraits::associative_op;
                        right_node.children.push_back(inner.children[1]);
                        right_node.children.push_back(outer.children[1]);
                        const e_class_id right_id = g.add(right_node);

                        typename AnyG::node_t new_outer;
                        new_outer.op = OpTraits::associative_op;
                        new_outer.children.push_back(inner.children[0]);
                        new_outer.children.push_back(right_id);
                        const e_class_id new_id = g.add(new_outer);
                        (void)g.merge(id, new_id);
                    }
                }
            }
        }
    };

    template <class OpTraits, class G = void>
    struct distributivity {
        template <class AnyG>
        void apply(AnyG& g) const {
            const std::size_t snapshot = g.class_count();
            for (e_class_id id = 0; id < static_cast<e_class_id>(snapshot); ++id) {
                if (!g.is_root(id)) continue;
                for (const auto& outer : g.classes()[id].nodes) {
                    if (outer.op != OpTraits::mul_op || outer.children.size() != 2)
                        continue;
                    const e_class_id right = g.find(outer.children[1]);
                    for (const auto& inner : g.classes()[right].nodes) {
                        if (inner.op != OpTraits::add_op || inner.children.size() != 2)
                            continue;
                        typename AnyG::node_t left_mul, right_mul, new_add;
                        left_mul.op = OpTraits::mul_op;
                        left_mul.children.push_back(outer.children[0]);
                        left_mul.children.push_back(inner.children[0]);
                        right_mul.op = OpTraits::mul_op;
                        right_mul.children.push_back(outer.children[0]);
                        right_mul.children.push_back(inner.children[1]);

                        new_add.op = OpTraits::add_op;
                        new_add.children.push_back(g.add(left_mul));
                        new_add.children.push_back(g.add(right_mul));

                        (void)g.merge(id, g.add(new_add));
                    }
                }
            }
        }
    };

    template <class OpTraits, class G = void>
    struct identity_zero {
        template <class AnyG>
        void apply(AnyG& g) const {
            const std::size_t snapshot = g.class_count();
            for (e_class_id id = 0; id < static_cast<e_class_id>(snapshot); ++id) {
                if (!g.is_root(id)) continue;
                for (const auto& node : g.classes()[id].nodes) {
                    if (node.children.size() != 2) continue;
                    if constexpr (requires { OpTraits::add_op; OpTraits::zero_payload; }) {
                        if (node.op == OpTraits::add_op) {
                            for (int slot = 0; slot < 2; ++slot) {
                                const e_class_id cid = g.find(node.children[slot]);
                                for (const auto& cn : g.classes()[cid].nodes) {
                                    if (cn.op == OpTraits::zero_op &&
                                        cn.children.empty() &&
                                        cn.payload == OpTraits::zero_payload) {
                                        (void)g.merge(id, g.find(node.children[1 - slot]));
                                    }
                                }
                            }
                        }
                    }
                    if constexpr (requires { OpTraits::mul_op; OpTraits::one_payload; }) {
                        if (node.op == OpTraits::mul_op) {
                            for (int slot = 0; slot < 2; ++slot) {
                                const e_class_id cid = g.find(node.children[slot]);
                                for (const auto& cn : g.classes()[cid].nodes) {
                                    if (cn.op == OpTraits::one_op &&
                                        cn.children.empty() &&
                                        cn.payload == OpTraits::one_payload) {
                                        (void)g.merge(id, g.find(node.children[1 - slot]));
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    };
} // namespace egraph
