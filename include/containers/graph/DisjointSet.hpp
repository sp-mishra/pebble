#pragma once

#include <algorithm>
#include <concepts>
#include <cstdint>
#include <expected>
#include <functional>
#include <limits>
#include <ranges>
#include <unordered_map>
#include <utility>
#include <vector>

// ============================================================================
// DisjointSet — generic union-find with C++23 features
//
// Designed for advanced type-system workloads (type unification, equivalence
// class tracking, constraint propagation) but fully generic.
//
// Key capabilities
// ----------------
//  • Union-by-rank + full path compression (halving variant for cache locality)
//  • Element metadata  : arbitrary payload per element (ElemMeta)
//  • Set metadata      : arbitrary payload per equivalence class (SetMeta)
//  • Merge strategies  : pluggable policy for combining SetMeta on union
//  • Union callbacks   : observe every union event (before/after)
//  • Snapshot/rollback : undo stack for backtracking type inference
//  • Weighted union    : optional per-element weights summed on merge
//  • std::expected     : all fallible operations return expected<T, DSError>
//  • Range views       : elements_of(root), all_sets(), all_elements()
//  • Concepts          : DSMergeStrategy, DSCallback, DSElement
// ============================================================================

namespace disjointset {
    // -------------------------------------------------------------------------
    // Error type
    // -------------------------------------------------------------------------
    enum class DSError : std::uint8_t {
        ElementNotFound,
        ElementAlreadyExists,
        InvalidElement,
        SnapshotStackEmpty,
        MergeViolation,
    };

    // -------------------------------------------------------------------------
    // Concepts
    // -------------------------------------------------------------------------

    template <typename T>
    concept DSElement = std::equality_comparable<T> && std::copyable<T> && requires(T t) {
        std::hash<T>{}(t);
    };

    // A merge strategy combines two SetMeta values when two sets are unioned.
    // pick_root(meta_a, meta_b) returns the SetMeta to keep for the new root.
    template <typename S, typename Meta>
    concept DSMergeStrategy = requires(S strategy, Meta a, Meta b) {
        { strategy.pick_root(a, b) } -> std::convertible_to<Meta>;
    };

    // A callback receives union events.
    // Signature: on_union(elem_a, elem_b, new_root_elem)
    template <typename C, typename Elem>
    concept DSCallback = requires(C cb, const Elem& a, const Elem& b, const Elem& root) {
        cb.on_union(a, b, root);
    };

    // -------------------------------------------------------------------------
    // Built-in merge strategies
    // -------------------------------------------------------------------------

    // Keep the metadata of the element with higher rank (the new root).
    struct KeepRootMeta {
        template <typename Meta>
        constexpr Meta pick_root(const Meta& root_meta, const Meta&) const noexcept {
            return root_meta;
        }
    };

    // Keep the metadata of the element with lower rank (the child).
    struct KeepChildMeta {
        template <typename Meta>
        constexpr Meta pick_root(const Meta&, const Meta& child_meta) const noexcept {
            return child_meta;
        }
    };

    // Merge by calling Meta::merge(other) — useful for lattice types.
    struct LatticeJoinMeta {
        template <typename Meta>
            requires requires(Meta a, const Meta& b) { { a.merge(b) } -> std::same_as<Meta>; }
        Meta pick_root(const Meta& root_meta, const Meta& child_meta) const {
            return root_meta.merge(child_meta);
        }
    };

    // No-op placeholder for unit/empty metadata.
    struct IdentityMeta {
        template <typename Meta>
        constexpr Meta pick_root(const Meta& root_meta, const Meta&) const noexcept {
            return root_meta;
        }
    };

    // -------------------------------------------------------------------------
    // ElementId — strong index type (similar to NodeId/EdgeId in LiteGraph)
    // -------------------------------------------------------------------------
    struct ElementId {
        std::size_t value;

        constexpr ElementId() noexcept : value(std::numeric_limits<std::size_t>::max()) {}

        explicit constexpr ElementId(const std::size_t v) noexcept : value(v) {}

        [[nodiscard]] constexpr bool is_valid() const noexcept {
            return value != std::numeric_limits<std::size_t>::max();
        }

        constexpr auto operator<=>(const ElementId&) const noexcept = default;
    };

    inline constexpr ElementId INVALID_ELEMENT_ID{};

    // -------------------------------------------------------------------------
    // UnionEvent — passed to callbacks
    // -------------------------------------------------------------------------
    template <DSElement Elem>
    struct UnionEvent {
        Elem elem_a;
        Elem elem_b;
        Elem new_root;
        std::size_t new_set_size;
    };

    // -------------------------------------------------------------------------
    // DisjointSet
    //
    // Template parameters
    //   Elem      – the element type (must satisfy DSElement)
    //   ElemMeta  – per-element metadata; defaults to std::monostate
    //   SetMeta   – per-set metadata;     defaults to std::monostate
    //   Strategy  – merge policy for SetMeta; defaults to KeepRootMeta
    // -------------------------------------------------------------------------
    template <
        DSElement Elem,
        typename ElemMeta = std::monostate,
        typename SetMeta = std::monostate,
        DSMergeStrategy<SetMeta> Strategy = KeepRootMeta>
        requires std::copyable<ElemMeta> && std::copyable<SetMeta>
    class DisjointSet {
    public:
        // ------------------------------------------------------------------ //
        // Public type aliases
        // ------------------------------------------------------------------ //
        using element_type = Elem;
        using elem_meta_type = ElemMeta;
        using set_meta_type = SetMeta;
        using strategy_type = Strategy;
        using event_type = UnionEvent<Elem>;

        using CallbackFn = std::function<void(const event_type&)>;

        // ------------------------------------------------------------------ //
        // Construction
        // ------------------------------------------------------------------ //
        DisjointSet() = default;

        explicit DisjointSet(Strategy strategy) : strategy_(std::move(strategy)) {}

        DisjointSet(const DisjointSet&) = default;

        DisjointSet(DisjointSet&&) noexcept = default;

        DisjointSet& operator=(const DisjointSet&) = default;

        DisjointSet& operator=(DisjointSet&&) noexcept = default;

        ~DisjointSet() = default;

        // ------------------------------------------------------------------ //
        // Element registration
        // ------------------------------------------------------------------ //

        // Insert a new element. Returns its ElementId on success, or
        // DSError::ElementAlreadyExists if already present.
        std::expected<ElementId, DSError>
        insert(const Elem& elem,
               ElemMeta elem_meta = ElemMeta{},
               SetMeta set_meta = SetMeta{},
               double weight = 1.0) {
            if (elem_to_id_.contains(elem))
                return std::unexpected(DSError::ElementAlreadyExists);

            const ElementId id{nodes_.size()};
            elem_to_id_[elem] = id;

            nodes_.push_back(Node{
                .parent = id,
                .rank = 0,
                .size = 1,
                .weight_sum = weight,
                .elem = elem,
                .elem_meta = std::move(elem_meta),
                .set_meta = std::move(set_meta),
                .active = true,
            });

            return id;
        }

        // Insert or retrieve — never fails due to duplicates.
        ElementId insert_or_get(const Elem& elem,
                                ElemMeta elem_meta = ElemMeta{},
                                SetMeta set_meta = SetMeta{},
                                const double weight = 1.0) {
            if (auto it = elem_to_id_.find(elem); it != elem_to_id_.end())
                return it->second;
            return *insert(elem, std::move(elem_meta), std::move(set_meta), weight);
        }

        // ------------------------------------------------------------------ //
        // Find — returns the ElementId of the canonical root.
        // ------------------------------------------------------------------ //

        [[nodiscard]] std::expected<ElementId, DSError>
        find(const Elem& elem) const {
            auto it = elem_to_id_.find(elem);
            if (it == elem_to_id_.end())
                return std::unexpected(DSError::ElementNotFound);
            return ElementId{find_root(it->second.value)};
        }

        [[nodiscard]] std::expected<ElementId, DSError>
        find_by_id(const ElementId id) const {
            if (!valid_id(id))
                return std::unexpected(DSError::InvalidElement);
            return ElementId{find_root(id.value)};
        }

        // Returns the canonical element (not just the id).
        [[nodiscard]] std::expected<Elem, DSError>
        representative(const Elem& elem) const {
            auto r = find(elem);
            if (!r) return std::unexpected(r.error());
            return nodes_[r->value].elem;
        }

        // ------------------------------------------------------------------ //
        // Union
        // ------------------------------------------------------------------ //

        // Unite the sets containing elem_a and elem_b.
        // Returns the root ElementId of the merged set, or an error.
        std::expected<ElementId, DSError>
        unite(const Elem& elem_a, const Elem& elem_b) {
            auto ra = find(elem_a);
            if (!ra) return std::unexpected(ra.error());
            auto rb = find(elem_b);
            if (!rb) return std::unexpected(rb.error());

            return unite_by_id(*ra, *rb);
        }

        std::expected<ElementId, DSError>
        unite_by_id(const ElementId id_a, const ElementId id_b) {
            if (!valid_id(id_a) || !valid_id(id_b))
                return std::unexpected(DSError::InvalidElement);

            const std::size_t ra = find_root(id_a.value);
            const std::size_t rb = find_root(id_b.value);

            if (ra == rb) return ElementId{ra}; // already same set

            // Push undo frame before modifying.
            if (!undo_stack_.empty())
                undo_stack_.back().push_back(make_undo_frame(ra, rb));

            const std::size_t new_root = link(ra, rb);

            // Fire callbacks.
            if (!callbacks_.empty()) {
                event_type ev{
                    nodes_[ra].elem,
                    nodes_[rb].elem,
                    nodes_[new_root].elem,
                    nodes_[new_root].size,
                };
                for (auto& cb : callbacks_)
                    cb(ev);
            }

            return ElementId{new_root};
        }

        // ------------------------------------------------------------------ //
        // Queries
        // ------------------------------------------------------------------ //

        [[nodiscard]] bool connected(const Elem& a, const Elem& b) const {
            auto ra = find(a);
            auto rb = find(b);
            if (!ra || !rb) return false;
            return ra->value == rb->value;
        }

        [[nodiscard]] bool connected_by_id(const ElementId a, const ElementId b) const {
            if (!valid_id(a) || !valid_id(b)) return false;
            return find_root(a.value) == find_root(b.value);
        }

        [[nodiscard]] std::size_t element_count() const noexcept {
            return elem_to_id_.size();
        }

        // Number of distinct equivalence classes.
        [[nodiscard]] std::size_t set_count() const noexcept {
            std::size_t count = 0;
            for (std::size_t i = 0; i < nodes_.size(); ++i)
                if (nodes_[i].active && find_root(i) == i)
                    ++count;
            return count;
        }

        // Size of the set containing elem.
        [[nodiscard]] std::expected<std::size_t, DSError>
        set_size(const Elem& elem) const {
            auto r = find(elem);
            if (!r) return std::unexpected(r.error());
            return nodes_[r->value].size;
        }

        // Total weight sum of the set containing elem.
        [[nodiscard]] std::expected<double, DSError>
        set_weight(const Elem& elem) const {
            auto r = find(elem);
            if (!r) return std::unexpected(r.error());
            return nodes_[r->value].weight_sum;
        }

        [[nodiscard]] bool contains(const Elem& elem) const noexcept {
            return elem_to_id_.contains(elem);
        }

        [[nodiscard]] bool valid_id(ElementId id) const noexcept {
            return id.is_valid() && id.value < nodes_.size() && nodes_[id.value].active;
        }

        // ------------------------------------------------------------------ //
        // Metadata accessors
        // ------------------------------------------------------------------ //

        [[nodiscard]] std::expected<std::reference_wrapper<ElemMeta>, DSError>
        elem_meta(const Elem& elem) {
            auto it = elem_to_id_.find(elem);
            if (it == elem_to_id_.end())
                return std::unexpected(DSError::ElementNotFound);
            return std::ref(nodes_[it->second.value].elem_meta);
        }

        [[nodiscard]] std::expected<std::reference_wrapper<const ElemMeta>, DSError>
        elem_meta(const Elem& elem) const {
            auto it = elem_to_id_.find(elem);
            if (it == elem_to_id_.end())
                return std::unexpected(DSError::ElementNotFound);
            return std::cref(nodes_[it->second.value].elem_meta);
        }

        // Set metadata of the representative of elem's class.
        [[nodiscard]] std::expected<std::reference_wrapper<SetMeta>, DSError>
        set_meta(const Elem& elem) {
            auto r = find(elem);
            if (!r) return std::unexpected(r.error());
            return std::ref(nodes_[r->value].set_meta);
        }

        [[nodiscard]] std::expected<std::reference_wrapper<const SetMeta>, DSError>
        set_meta(const Elem& elem) const {
            auto r = find(elem);
            if (!r) return std::unexpected(r.error());
            return std::cref(nodes_[r->value].set_meta);
        }

        [[nodiscard]] std::expected<std::reference_wrapper<SetMeta>, DSError>
        set_meta_by_id(const ElementId id) {
            auto r = find_by_id(id);
            if (!r) return std::unexpected(r.error());
            return std::ref(nodes_[r->value].set_meta);
        }

        // ------------------------------------------------------------------ //
        // Range views
        // ------------------------------------------------------------------ //

        // Lazy range of all active element values.
        [[nodiscard]] auto all_elements() const {
            return std::views::iota(std::size_t{0}, nodes_.size())
                | std::views::filter([this](std::size_t i) { return nodes_[i].active; })
                | std::views::transform([this](std::size_t i) -> const Elem& {
                    return nodes_[i].elem;
                });
        }

        // Lazy range of representative elements (one per equivalence class).
        [[nodiscard]] auto all_sets() const {
            return std::views::iota(std::size_t{0}, nodes_.size())
                | std::views::filter([this](std::size_t i) {
                    return nodes_[i].active && find_root(i) == i;
                })
                | std::views::transform([this](std::size_t i) -> const Elem& {
                    return nodes_[i].elem;
                });
        }

        // Lazy range of all elements that belong to the same set as elem.
        [[nodiscard]] auto elements_of(const Elem& elem) const {
            const auto root_opt = find(elem);
            const std::size_t root = root_opt
                                         ? root_opt->value
                                         : std::numeric_limits<std::size_t>::max();
            return std::views::iota(std::size_t{0}, nodes_.size())
                | std::views::filter([this, root](std::size_t i) {
                    return nodes_[i].active && find_root(i) == root;
                })
                | std::views::transform([this](std::size_t i) -> const Elem& {
                    return nodes_[i].elem;
                });
        }

        // Returns the ElementIds of all current roots (one per set).
        [[nodiscard]] auto root_ids() const {
            return std::views::iota(std::size_t{0}, nodes_.size())
                | std::views::filter([this](std::size_t i) {
                    return nodes_[i].active && find_root(i) == i;
                })
                | std::views::transform([](const std::size_t i) { return ElementId{i}; });
        }

        // ------------------------------------------------------------------ //
        // Snapshot / rollback (for backtracking type inference)
        // ------------------------------------------------------------------ //

        // Push a new undo frame. All unite() calls made after this point
        // are recorded and can be undone atomically by rollback().
        void push_snapshot() {
            undo_stack_.emplace_back();
        }

        // Undo all unions recorded since the last push_snapshot().
        std::expected<void, DSError> rollback() {
            if (undo_stack_.empty())
                return std::unexpected(DSError::SnapshotStackEmpty);

            for (auto it = undo_stack_.back().rbegin();
                 it != undo_stack_.back().rend(); ++it) {
                apply_undo(*it);
            }
            undo_stack_.pop_back();
            return {};
        }

        // Commit the current snapshot (discard undo records, merge into parent).
        std::expected<void, DSError> commit() {
            if (undo_stack_.empty())
                return std::unexpected(DSError::SnapshotStackEmpty);

            auto frames = std::move(undo_stack_.back());
            undo_stack_.pop_back();
            // Merge into the parent snapshot if one exists.
            if (!undo_stack_.empty()) {
                auto& parent = undo_stack_.back();
                parent.insert(parent.end(),
                              std::make_move_iterator(frames.begin()),
                              std::make_move_iterator(frames.end()));
            }
            return {};
        }

        [[nodiscard]] std::size_t snapshot_depth() const noexcept {
            return undo_stack_.size();
        }

        // ------------------------------------------------------------------ //
        // Callbacks
        // ------------------------------------------------------------------ //

        // Register a callback invoked after every successful union.
        // Returns a handle that can be used with remove_callback().
        std::size_t add_callback(CallbackFn fn) {
            const std::size_t id = next_callback_id_++;
            callbacks_with_ids_.emplace_back(id, std::move(fn));
            rebuild_callback_list();
            return id;
        }

        void remove_callback(std::size_t callback_id) {
            std::erase_if(callbacks_with_ids_,
                          [callback_id](const auto& pair) {
                              return pair.first == callback_id;
                          });
            rebuild_callback_list();
        }

        // ------------------------------------------------------------------ //
        // Bulk operations
        // ------------------------------------------------------------------ //

        // Union a range of elements into a single equivalence class.
        // Returns the final root, or the first error encountered.
        template <std::ranges::input_range R>
            requires std::same_as < std::ranges::range_value_t < R >



        ,
        Elem
        >
        [[nodiscard]] std::expected<ElementId, DSError> unite_all(R&& range) {
            auto it = std::ranges::begin(range);
            auto end = std::ranges::end(range);
            if (it == end)
                return std::unexpected(DSError::InvalidElement);

            auto root = find(*it);
            if (!root) return std::unexpected(root.error());
            ++it;

            for (; it != end; ++it) {
                auto result = unite(nodes_[root->value].elem, *it);
                if (!result) return std::unexpected(result.error());
                root = result;
            }
            return root;
        }

        // Insert a range of elements without connecting them.
        template <std::ranges::input_range R>
            requires std::same_as < std::ranges::range_value_t < R >



        ,
        Elem
        >
        void insert_all(R&& range) {
            for (const auto& elem : range)
                insert_or_get(elem);
        }

        // ------------------------------------------------------------------ //
        // Conversion helpers (useful for type-system consumers)
        // ------------------------------------------------------------------ //

        // Returns a map: representative element → vector of all elements in its class.
        [[nodiscard]] std::unordered_map<Elem, std::vector<Elem>>
        partition() const {
            std::unordered_map<Elem, std::vector<Elem>> result;
            for (std::size_t i = 0; i < nodes_.size(); ++i) {
                if (!nodes_[i].active) continue;
                const Elem& rep = nodes_[find_root(i)].elem;
                result[rep].push_back(nodes_[i].elem);
            }
            return result;
        }

        // Collect all members of the equivalence class of elem into a vector.
        [[nodiscard]] std::expected<std::vector<Elem>, DSError>
        members(const Elem& elem) const {
            auto r = find(elem);
            if (!r) return std::unexpected(r.error());
            const std::size_t root = r->value;
            std::vector<Elem> result;
            for (std::size_t i = 0; i < nodes_.size(); ++i)
                if (nodes_[i].active && find_root(i) == root)
                    result.push_back(nodes_[i].elem);
            return result;
        }

        // ------------------------------------------------------------------ //
        // Utility
        // ------------------------------------------------------------------ //

        void clear() {
            nodes_.clear();
            elem_to_id_.clear();
            undo_stack_.clear();
        }

        void reserve(std::size_t capacity) {
            nodes_.reserve(capacity);
            elem_to_id_.reserve(capacity);
        }

        // Expose the merge strategy (useful for testing / introspection).
        [[nodiscard]] const Strategy& strategy() const noexcept { return strategy_; }
        [[nodiscard]] Strategy& strategy() noexcept { return strategy_; }

    private:
        // ------------------------------------------------------------------ //
        // Internal node storage
        // ------------------------------------------------------------------ //
        struct Node {
            ElementId parent;
            std::size_t rank = 0;
            std::size_t size = 1;
            double weight_sum = 1.0;
            Elem elem;
            ElemMeta elem_meta{};
            SetMeta set_meta{};
            bool active = true;
        };

        // ------------------------------------------------------------------ //
        // Undo frame for snapshot/rollback
        // ------------------------------------------------------------------ //
        struct UndoFrame {
            std::size_t root; // index of who became root
            std::size_t child; // index of who became child
            std::size_t old_root_parent;
            std::size_t old_child_parent;
            std::size_t old_root_rank;
            std::size_t old_child_rank;
            std::size_t old_root_size;
            std::size_t old_child_size;
            double old_root_weight;
            double old_child_weight;
            SetMeta old_root_set_meta;
            SetMeta old_child_set_meta;
        };

        // ------------------------------------------------------------------ //
        // Path-compressed find (path halving, amortized O(α(n)))
        // Const version uses const_cast internally — safe because parent
        // updates are logically non-observable state (compression invariant).
        // ------------------------------------------------------------------ //
        [[nodiscard]] std::size_t find_root(std::size_t i) const noexcept {
            auto& nodes = const_cast<std::vector<Node>&>(nodes_);
            while (nodes[i].parent.value != i) {
                // Path halving: point to grandparent.
                const std::size_t gp = nodes[nodes[i].parent.value].parent.value;
                nodes[i].parent = ElementId{gp};
                i = gp;
            }
            return i;
        }

        // ------------------------------------------------------------------ //
        // Union by rank
        // ------------------------------------------------------------------ //
        std::size_t link(std::size_t ra, std::size_t rb) {
            // ra and rb are already roots.
            std::size_t new_root, child;
            if (nodes_[ra].rank < nodes_[rb].rank) {
                new_root = rb;
                child = ra;
            }
            else if (nodes_[ra].rank > nodes_[rb].rank) {
                new_root = ra;
                child = rb;
            }
            else {
                new_root = ra;
                child = rb;
                ++nodes_[ra].rank;
            }

            nodes_[child].parent = ElementId{new_root};
            nodes_[new_root].size += nodes_[child].size;
            nodes_[new_root].weight_sum += nodes_[child].weight_sum;
            nodes_[new_root].set_meta =
                strategy_.pick_root(nodes_[new_root].set_meta, nodes_[child].set_meta);

            return new_root;
        }

        // ------------------------------------------------------------------ //
        // Snapshot helpers
        // ------------------------------------------------------------------ //

        // Capture a complete pre-union snapshot of both roots ra and rb.
        // 'ra' and 'rb' are already canonical roots (find_root has been called).
        // We record both indices directly so apply_undo can restore exactly.
        UndoFrame make_undo_frame(std::size_t ra, std::size_t rb) const {
            return UndoFrame{
                .root = ra,
                .child = rb,
                .old_root_parent = nodes_[ra].parent.value, // == ra (it's a root)
                .old_child_parent = nodes_[rb].parent.value, // == rb (it's a root)
                .old_root_rank = nodes_[ra].rank,
                .old_child_rank = nodes_[rb].rank,
                .old_root_size = nodes_[ra].size,
                .old_child_size = nodes_[rb].size,
                .old_root_weight = nodes_[ra].weight_sum,
                .old_child_weight = nodes_[rb].weight_sum,
                .old_root_set_meta = nodes_[ra].set_meta,
                .old_child_set_meta = nodes_[rb].set_meta,
            };
        }

        void apply_undo(const UndoFrame& f) {
            // Restore both nodes (indexed by f.root / f.child) to pre-union state.
            // We use f.root and f.child (the actual node indices) rather than the
            // parent values, because the parent values are only equal to the index
            // for roots and are not reliable after partial path compression.
            nodes_[f.root].parent = ElementId{f.old_root_parent};
            nodes_[f.root].rank = f.old_root_rank;
            nodes_[f.root].size = f.old_root_size;
            nodes_[f.root].weight_sum = f.old_root_weight;
            nodes_[f.root].set_meta = f.old_root_set_meta;

            nodes_[f.child].parent = ElementId{f.old_child_parent};
            nodes_[f.child].rank = f.old_child_rank;
            nodes_[f.child].size = f.old_child_size;
            nodes_[f.child].weight_sum = f.old_child_weight;
            nodes_[f.child].set_meta = f.old_child_set_meta;
        }

        void rebuild_callback_list() {
            callbacks_.clear();
            callbacks_.reserve(callbacks_with_ids_.size());
            for (auto& [id, fn] : callbacks_with_ids_)
                callbacks_.push_back(fn);
        }

        // ------------------------------------------------------------------ //
        // Data members
        // ------------------------------------------------------------------ //
        std::vector<Node> nodes_;
        std::unordered_map<Elem, ElementId> elem_to_id_;
        std::vector<std::vector<UndoFrame>> undo_stack_;
        std::vector<std::pair<std::size_t, CallbackFn>> callbacks_with_ids_;
        std::vector<CallbackFn> callbacks_; // fast path cache
        Strategy strategy_{};
        std::size_t next_callback_id_ = 0;
    };

    // -------------------------------------------------------------------------
    // Deduction guide — infer Elem from initializer_list
    // -------------------------------------------------------------------------
    template <DSElement Elem>
    DisjointSet(std::initializer_list<Elem>) -> DisjointSet<Elem>;

    // -------------------------------------------------------------------------
    // Free-function helpers
    // -------------------------------------------------------------------------

    // Build a DisjointSet from a range, inserting every element independently.
    template <std::ranges::input_range R,
              typename ElemMeta = std::monostate,
              typename SetMeta = std::monostate,
              typename Strategy = KeepRootMeta>
        requires DSElement<std::ranges::range_value_t<R>>
    [[nodiscard]] auto make_disjoint_set(R&& range,
                                         Strategy strategy = Strategy{}) {
        using Elem = std::ranges::range_value_t<R>;
        DisjointSet<Elem, ElemMeta, SetMeta, Strategy> ds{std::move(strategy)};
        for (const auto& e : range)
            ds.insert_or_get(e);
        return ds;
    }

    // Check whether two DisjointSets over the same element type have identical
    // partition structure (same elements in same classes, regardless of root).
    template <DSElement Elem, typename EM, typename SM, typename Str>
    [[nodiscard]] bool same_partition(
        const DisjointSet<Elem, EM, SM, Str>& a,
        const DisjointSet<Elem, EM, SM, Str>& b) {
        auto pa = a.partition();
        auto pb = b.partition();
        if (pa.size() != pb.size()) return false;

        // Normalise each class to a sorted set and compare.
        auto normalise = [](std::unordered_map<Elem, std::vector<Elem>>& p) {
            std::unordered_map<std::size_t, std::vector<Elem>> by_class;
            std::size_t class_id = 0;
            for (auto& [rep, members] : p) {
                std::ranges::sort(members);
                by_class[class_id++] = std::move(members);
            }
            return by_class;
        };

        auto na = normalise(pa);
        auto nb = normalise(pb);

        std::vector<std::vector<Elem>> va, vb;
        for (auto& [k, v] : na) va.push_back(v);
        for (auto& [k, v] : nb) vb.push_back(v);
        std::ranges::sort(va);
        std::ranges::sort(vb);
        return va == vb;
    }
} // namespace disjointset

// -------------------------------------------------------------------------
// std::hash specialisation for ElementId
// -------------------------------------------------------------------------
template <>
struct std::hash<disjointset::ElementId> {
    constexpr std::size_t operator()(const disjointset::ElementId& id) const noexcept {
        return std::hash<std::size_t>{}(id.value);
    }
};

