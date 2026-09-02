#pragma once
// ============================================================================
// drishya/tree.hpp — retained widget tree (flat SoA arena)
// ----------------------------------------------------------------------------
// The retained tree that survives across frames. Nodes are stored in a flat
// structure-of-arrays arena addressed by a contiguous NodeId (u32); links are
// first_child / next_sibling / parent indices. This mirrors how akruti's layout
// engine addresses nodes by contiguous index (rect[i]), so layout_bridge can map
// a widget NodeId straight onto a layout node index with no hashing.
//
// Why not containers::NAryTree? That tree allocates a heap node (unique_ptr) per
// element and pulls in Highway/iostream/mutex — fine for durable document trees,
// wrong for a per-frame UI tree that must stay heap-light on the hot path. We
// keep the arena flat here and reuse containers for the two roles it fits well:
//   * slot_map<NodeId>  — stable, generation-checked *handles* the app holds
//     across rebuilds (a NodeId is a raw slot; a WidgetHandle survives churn).
//   * SparseSet<NodeId> — the O(1) dirty set of nodes needing relayout/repaint.
//
// No virtual, no macros, no RTTI. Header-only C++23.
// ============================================================================

#include "drishya/any_widget.hpp"

#include "containers/associative/SparseSet.hpp"
#include "containers/associative/slot_map.hpp"
#include "containers/handle/generational_handle.hpp"

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace pebble::drishya {
    // Contiguous node index into the arena. kInvalidNode marks "no link".
    using NodeId = std::uint32_t;
    inline constexpr NodeId kInvalidNode = 0xFFFFFFFFu;

    // Phantom tag so a WidgetHandle can't be confused with any other handle type.
    struct WidgetTag {};

    using WidgetHandle = containers::generational_handle<WidgetTag>;

    // Which arrays a dirty node needs reprocessed. Mirrors the intent of akruti's
    // DirtyFlags but scoped to the widget layer (the layout engine has its own).
    enum DirtyBits : std::uint8_t {
        kDirtyNone = 0,
        kDirtyLayout = 1u << 0, // style/measure changed → relayout subtree
        kDirtyPaint = 1u << 1, // visual-only change → repaint, geometry stable
        kDirtyTree = 1u << 2, // structural change (insert/remove/move)
    };

    // ----------------------------------------------------------------------------
    // WidgetTree<Metrics, Painter, InlineBytes>
    //   The node payload is AnyWidgetT erased against the same (Metrics, Painter)
    //   pair the app renders with.
    // ----------------------------------------------------------------------------
    template <typename Metrics, typename Painter_, std::size_t InlineBytes = 512>
        requires ITextMetrics<Metrics> && Painter<Painter_>
    class WidgetTree {
    public:
        using widget_type = AnyWidgetT<Metrics, Painter_, InlineBytes>;

        WidgetTree() = default;

        // --- construction ------------------------------------------------------
        // Set the single root, replacing any prior root subtree. Returns its NodeId.
        NodeId set_root(widget_type w) {
            clear();
            const NodeId id = alloc_node(std::move(w), kInvalidNode);
            root_ = id;
            mark_dirty(id, kDirtyTree | kDirtyLayout | kDirtyPaint);
            return id;
        }

        // Append `w` as the last child of `parent`. Returns the new child's NodeId.
        NodeId add_child(NodeId parent, widget_type w) {
            const NodeId id = alloc_node(std::move(w), parent);
            link_last_child(parent, id);
            mark_dirty(parent, kDirtyTree | kDirtyLayout);
            mark_dirty(id, kDirtyTree | kDirtyLayout | kDirtyPaint);
            return id;
        }

        // --- stable handles ----------------------------------------------------
        // Mint a stable handle for a node the app wants to reference across rebuilds.
        // The handle stays valid until the node is removed; a NodeId alone does not.
        [[nodiscard]] WidgetHandle make_handle(NodeId id) {
            const WidgetHandle h = handles_.insert(id);
            if (id != kInvalidNode && id < node_.size()) node_[id].handle = h;
            return h;
        }

        // Resolve a handle to its current NodeId (kInvalidNode if stale/removed).
        [[nodiscard]] NodeId resolve(WidgetHandle h) const noexcept {
            const NodeId* p = handles_.find(h);
            return p ? *p : kInvalidNode;
        }

        [[nodiscard]] bool alive(WidgetHandle h) const noexcept { return handles_.contains(h); }

        // --- access ------------------------------------------------------------
        [[nodiscard]] NodeId root() const noexcept { return root_; }
        [[nodiscard]] std::size_t node_count() const noexcept { return node_.size() - free_.size(); }
        [[nodiscard]] std::size_t capacity() const noexcept { return node_.size(); }

        [[nodiscard]] bool valid(NodeId id) const noexcept {
            return id < node_.size() && node_[id].live;
        }

        [[nodiscard]] widget_type& widget(NodeId id) noexcept { return node_[id].widget; }
        [[nodiscard]] const widget_type& widget(NodeId id) const noexcept { return node_[id].widget; }

        [[nodiscard]] NodeId parent(NodeId id) const noexcept { return node_[id].parent; }
        [[nodiscard]] NodeId first_child(NodeId id) const noexcept { return node_[id].first_child; }
        [[nodiscard]] NodeId next_sibling(NodeId id) const noexcept { return node_[id].next_sibling; }

        // Visit each child of `id` in order. Fn: void(NodeId).
        template <typename Fn>
        void for_each_child(NodeId id, Fn&& fn) const {
            for (NodeId c = node_[id].first_child; c != kInvalidNode; c = node_[c].next_sibling)
                fn(c);
        }

        // Pre-order depth-first walk from `id`. Fn: void(NodeId, std::size_t depth).
        template <typename Fn>
        void walk(NodeId id, Fn&& fn) const {
            walk_impl(id, 0, fn);
        }

        template <typename Fn>
        void walk(Fn&& fn) const {
            if (root_ != kInvalidNode) walk_impl(root_, 0, fn);
        }

        // --- mutation ----------------------------------------------------------
        // Remove `id` and its whole subtree, unlinking from its parent.
        void remove(NodeId id) {
            if (!valid(id)) return;
            unlink_from_parent(id);
            free_subtree(id);
            mark_dirty(root_, kDirtyTree | kDirtyLayout);
        }

        // --- dirty tracking ----------------------------------------------------
        void mark_dirty(NodeId id, std::uint8_t bits) {
            if (id == kInvalidNode || id >= node_.size()) return;
            // insert_or_update auto-grows the sparse universe and merges dirty bits.
            std::uint8_t merged = bits;
            if (auto cur = dirty_.get(id); cur.has_value())
                merged = static_cast<std::uint8_t>(cur->get() | bits);
            dirty_.insert_or_update(id, merged);
        }

        [[nodiscard]] bool any_dirty() const noexcept { return !dirty_.empty(); }

        // Consume the dirty set: fn(NodeId, u8 bits) for each, then clear. Callers
        // (the layout bridge / painter) drain this once per frame.
        template <typename Fn>
        void drain_dirty(Fn&& fn) {
            for (const auto& kv : dirty_.all_pairs()) fn(kv.first, kv.second);
            dirty_.clear();
        }

        void clear() {
            node_.clear();
            free_.clear();
            handles_ = {};
            dirty_.clear();
            root_ = kInvalidNode;
        }

    private:
        struct Node {
            widget_type widget{};
            NodeId parent = kInvalidNode;
            NodeId first_child = kInvalidNode;
            NodeId last_child = kInvalidNode;
            NodeId next_sibling = kInvalidNode;
            WidgetHandle handle{};
            bool live = false;
        };

        NodeId alloc_node(widget_type w, NodeId parent) {
            NodeId id;
            if (!free_.empty()) {
                id = free_.back();
                free_.pop_back();
                node_[id] = Node{};
            }
            else {
                id = static_cast<NodeId>(node_.size());
                node_.emplace_back();
            }
            node_[id].widget = std::move(w);
            node_[id].parent = parent;
            node_[id].live = true;
            return id;
        }

        void link_last_child(NodeId parent, NodeId child) {
            Node& p = node_[parent];
            if (p.first_child == kInvalidNode) {
                p.first_child = child;
                p.last_child = child;
            }
            else {
                node_[p.last_child].next_sibling = child;
                p.last_child = child;
            }
        }

        void unlink_from_parent(NodeId id) {
            const NodeId par = node_[id].parent;
            if (par == kInvalidNode) {
                if (root_ == id) root_ = kInvalidNode;
                return;
            }
            Node& p = node_[par];
            if (p.first_child == id) {
                p.first_child = node_[id].next_sibling;
                if (p.last_child == id) p.last_child = kInvalidNode;
                return;
            }
            NodeId prev = p.first_child;
            while (prev != kInvalidNode && node_[prev].next_sibling != id)
                prev = node_[prev].next_sibling;
            if (prev != kInvalidNode) {
                node_[prev].next_sibling = node_[id].next_sibling;
                if (p.last_child == id) p.last_child = prev;
            }
        }

        void free_subtree(NodeId id) {
            // Depth-first free without recursion depth risk: use an explicit stack.
            stack_.clear();
            stack_.push_back(id);
            while (!stack_.empty()) {
                const NodeId n = stack_.back();
                stack_.pop_back();
                for (NodeId c = node_[n].first_child; c != kInvalidNode; c = node_[c].next_sibling)
                    stack_.push_back(c);
                if (!node_[n].handle.is_null()) handles_.erase(node_[n].handle);
                if (dirty_.contains(n)) {
                    auto r = dirty_.remove(n);
                    (void)r;
                }
                node_[n] = Node{};
                free_.push_back(n);
            }
        }

        template <typename Fn>
        void walk_impl(NodeId id, std::size_t depth, Fn& fn) const {
            fn(id, depth);
            for (NodeId c = node_[id].first_child; c != kInvalidNode; c = node_[c].next_sibling)
                walk_impl(c, depth + 1, fn);
        }

        std::vector<Node> node_{};
        std::vector<NodeId> free_{};
        std::vector<NodeId> stack_{};
        containers::slot_map<NodeId, WidgetHandle> handles_{};
        sparseset::SparseSet<NodeId, std::uint8_t> dirty_{};
        NodeId root_ = kInvalidNode;
    };
} // namespace pebble::drishya
