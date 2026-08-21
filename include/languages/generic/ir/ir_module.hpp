#pragma once

// languages/generic/ir/ir_module.hpp — Generic IR module: flat node storage.
//
// C++23, header-only, no virtual, no macros. Namespace: lang
//
// ir_module<KindEnum, ExtPayload, Store> — owns flat ir_node vector + child_ids.
//   Layout == green_arena<KE> when ExtPayload=monostate → Stage 3 adoption is zero-copy.
//
// Store policy:
//   default_store (std::vector, raw ir_node_id)      — runtime heap, default.
//   handle_store<Tag> (slot_map + generational_handle<Tag,uint32_t>) — stable
//       generational ids; node_handle = generational_handle<Tag,uint32_t>.
//       vakya's type_arena is the reference implementation (type_tag → type_ref).
//
// as_egraph_view() — adjacency view compatible with egraph.hpp.
// as_adjacency()   — feeds DominatorTree / LiteGraph for CFG/dominance.
// reset()          — clear nodes + child_ids, keep vector capacity.

#include <cstdint>
#include <span>
#include <vector>
#include "node.hpp"
#include "containers/associative/slot_map.hpp"
#include "containers/handle/generational_handle.hpp"

namespace lang {

    // ---- Store policy tags -------------------------------------------------

    struct default_store {};   // std::vector backing (default)

    // handle_store<Tag>: slot_map + generational_handle<Tag,uint32_t>.
    // Tag is a phantom type (e.g. vakya::types::type_tag) so handles are
    // type-safe and distinct from other generational_handle users.
    template <class Tag>
    struct handle_store {};

    // ---- egraph adjacency view (default_store) -----------------------------
    // Lightweight range-of-ranges: each element is the child id list for node i.
    // Compatible with egraph.hpp adjacency consumer (no new egraph created here).

    template <class KindEnum, class ExtPayload>
    struct ir_adjacency_view {
        const std::vector<ir_node<KindEnum, ExtPayload>>* nodes    = nullptr;
        const std::vector<ir_node_id>*                    children = nullptr;

        [[nodiscard]] std::span<const ir_node_id> adj(ir_node_id id) const noexcept {
            if (!nodes || !children) return {};
            const auto& n = (*nodes)[id];
            if (n.child_count == 0 || n.first_child == k_null_ir) return {};
            return std::span<const ir_node_id>(children->data() + n.first_child, n.child_count);
        }

        [[nodiscard]] std::size_t node_count() const noexcept {
            return nodes ? nodes->size() : 0;
        }
    };

    // ---- ir_module (default_store) -----------------------------------------

    template <class KindEnum,
              class ExtPayload = std::monostate,
              class Store      = default_store>
    class ir_module {
    public:
        using node_type   = ir_node<KindEnum, ExtPayload>;
        using node_handle = ir_node_id;   // raw uint32_t under default_store

        // ---- Append ---------------------------------------------------------

        [[nodiscard]] node_handle push(node_type n) {
            const node_handle id = static_cast<node_handle>(nodes_.size());
            nodes_.push_back(std::move(n));
            return id;
        }

        // Append child ids for the node at `node_id`, returning first_child offset.
        node_handle append_children(node_handle node_id,
                                    std::span<const ir_node_id> kids)
        {
            const ir_node_id offset = static_cast<ir_node_id>(child_ids_.size());
            for (auto cid : kids) child_ids_.push_back(cid);
            nodes_[node_id].first_child  = offset;
            nodes_[node_id].child_count  = static_cast<std::uint32_t>(kids.size());
            return offset;
        }

        // ---- Access ---------------------------------------------------------

        [[nodiscard]] node_type&       operator[](node_handle id)       { return nodes_[id]; }
        [[nodiscard]] const node_type& operator[](node_handle id) const { return nodes_[id]; }

        [[nodiscard]] std::span<const ir_node_id> children(node_handle id) const noexcept {
            const auto& n = nodes_[id];
            if (n.child_count == 0 || n.first_child == k_null_ir) return {};
            return std::span<const ir_node_id>(child_ids_.data() + n.first_child, n.child_count);
        }

        [[nodiscard]] node_handle     root()  const noexcept { return root_id_; }
        [[nodiscard]] std::uint32_t   size()  const noexcept {
            return static_cast<std::uint32_t>(nodes_.size());
        }
        [[nodiscard]] bool            empty() const noexcept { return nodes_.empty(); }

        void set_root(node_handle id) noexcept { root_id_ = id; }

        // reset: clear nodes + child_ids, preserve vector capacity.
        void reset() noexcept {
            nodes_.clear();
            child_ids_.clear();
            root_id_ = k_null_ir;
        }

        // ---- Graph views ----------------------------------------------------

        // as_egraph_view() — adjacency view compatible with egraph.hpp.
        [[nodiscard]] ir_adjacency_view<KindEnum, ExtPayload> as_egraph_view() const noexcept {
            return {&nodes_, &child_ids_};
        }

        // as_adjacency() — feeds DominatorTree / LiteGraph.
        [[nodiscard]] ir_adjacency_view<KindEnum, ExtPayload> as_adjacency() const noexcept {
            return {&nodes_, &child_ids_};
        }

    private:
        std::vector<node_type>  nodes_;
        std::vector<ir_node_id> child_ids_;
        node_handle             root_id_ = k_null_ir;
    };

    // ---- ir_module (handle_store<Tag>) specialization ----------------------
    //
    // Backs node storage with slot_map<ir_node<KE,EP>, generational_handle<Tag,uint32_t>>.
    // node_handle = generational_handle<Tag,uint32_t> — stable across erases; stale
    // handles are detected at access time (find returns nullptr).
    //
    // Child sidecar uses raw ir_node_id offsets (uint32_t) — same as default_store.
    //
    // as_egraph_view() returns a handle-store adjacency view where adj() takes a
    // node_handle and queries via slot_map::find.

    template <class KindEnum, class ExtPayload, class Tag>
    struct ir_adjacency_view_hs {
        using node_handle = containers::generational_handle<Tag, std::uint32_t>;
        using node_type   = ir_node<KindEnum, ExtPayload>;

        const containers::slot_map<node_type, node_handle>* store    = nullptr;
        const std::vector<ir_node_id>*                      children = nullptr;

        [[nodiscard]] std::span<const ir_node_id> adj(node_handle h) const noexcept {
            if (!store || !children) return {};
            const node_type* n = store->find(h);
            if (!n || n->child_count == 0 || n->first_child == k_null_ir) return {};
            return std::span<const ir_node_id>(children->data() + n->first_child, n->child_count);
        }

        [[nodiscard]] std::size_t node_count() const noexcept {
            return store ? store->size() : 0;
        }
    };

    template <class KindEnum, class ExtPayload, class Tag>
    class ir_module<KindEnum, ExtPayload, handle_store<Tag>> {
    public:
        using node_type   = ir_node<KindEnum, ExtPayload>;
        using node_handle = containers::generational_handle<Tag, std::uint32_t>;

        // ---- Append ---------------------------------------------------------

        [[nodiscard]] node_handle push(node_type n) {
            return store_.insert(std::move(n));
        }

        // Append child ids for the node at `h`, returning first_child offset.
        ir_node_id append_children(node_handle h,
                                   std::span<const ir_node_id> kids)
        {
            const ir_node_id offset = static_cast<ir_node_id>(child_ids_.size());
            for (auto cid : kids) child_ids_.push_back(cid);
            node_type* n = store_.find(h);
            if (n) {
                n->first_child  = offset;
                n->child_count  = static_cast<std::uint32_t>(kids.size());
            }
            return offset;
        }

        // ---- Access ---------------------------------------------------------

        // Returns nullptr when handle is stale.
        [[nodiscard]] node_type*       find(node_handle h)       noexcept { return store_.find(h); }
        [[nodiscard]] const node_type* find(node_handle h) const noexcept { return store_.find(h); }

        [[nodiscard]] std::span<const ir_node_id> children(node_handle h) const noexcept {
            const node_type* n = store_.find(h);
            if (!n || n->child_count == 0 || n->first_child == k_null_ir) return {};
            return std::span<const ir_node_id>(child_ids_.data() + n->first_child, n->child_count);
        }

        [[nodiscard]] node_handle   root()  const noexcept { return root_id_; }
        [[nodiscard]] std::uint32_t size()  const noexcept { return store_.size(); }
        [[nodiscard]] bool          empty() const noexcept { return store_.empty(); }

        void set_root(node_handle h) noexcept { root_id_ = h; }

        void erase(node_handle h) { store_.erase(h); }

        // reset: clear store + child_ids; root becomes null.
        void reset() noexcept {
            store_     = {};
            child_ids_ = {};
            root_id_   = {};
        }

        // ---- Graph views ----------------------------------------------------

        [[nodiscard]] ir_adjacency_view_hs<KindEnum, ExtPayload, Tag>
        as_egraph_view() const noexcept { return {&store_, &child_ids_}; }

        [[nodiscard]] ir_adjacency_view_hs<KindEnum, ExtPayload, Tag>
        as_adjacency() const noexcept { return {&store_, &child_ids_}; }

        // ---- Iteration (live nodes only) ------------------------------------

        auto begin()       { return store_.begin(); }
        auto end()         { return store_.end(); }
        auto begin() const { return store_.begin(); }
        auto end()   const { return store_.end(); }

    private:
        containers::slot_map<node_type, node_handle> store_;
        std::vector<ir_node_id>                      child_ids_;
        node_handle                                  root_id_{};
    };

} // namespace lang
