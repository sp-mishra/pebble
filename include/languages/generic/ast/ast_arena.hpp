#pragma once

// generic/ast/ast_arena.hpp — Flat index-based AST node arena.
//
// C++23, header-only, no virtual, no macros. Namespace: lang
//
// ast_arena<NodeVariant> — flat vector<NodeVariant> store.
//   - Nodes addressed by ast_node_id (uint32_t).
//   - Children stored as vector<ast_node_id> inside each node type — no
//     pointer aliasing, no std::any, safe to grow (pointers not taken).
//   - push(node) → ast_node_id.  operator[] by id for read/write access.
//   - k_null_node sentinel (max uint32_t) marks "no node".
//
// Relationship to ir_module (ir/ir_module.hpp):
//   ast_arena stores heterogeneous std::variant nodes with children embedded
//   inside each variant alternative.  ir_module<KindEnum, ExtPayload> stores
//   flat ir_node records with children in a separate sidecar vector —
//   enabling egraph views, adjacency queries, hash-cons interning, and splice.
//   For frontends that need those ir_module capabilities, define a KindEnum +
//   ExtPayload and use ir_module<KE, EP> directly (see crank_ir_module in
//   languages/crank/build_ast.hpp for a typed-AST example).
//
// Language frontends define their NodeVariant (std::variant<...>) and
// optionally alias the arena:
//
//   using my_node = std::variant<fn_node, block_node, lit_node, ...>;
//   using my_arena = lang::ast_arena<my_node>;
//   using my_node_id = lang::ast_node_id;
//
// Usage:
//   my_arena arena;
//   my_node_id fid = arena.push(fn_node{"main", {}});
//   auto& fn = std::get<fn_node>(arena[fid]);

#include <cstdint>
#include <limits>
#include <vector>

namespace lang {

    // =========================================================================
    // ast_node_id — index into ast_arena; k_null_node = absent/invalid
    // =========================================================================

    using ast_node_id = std::uint32_t;
    inline constexpr ast_node_id k_null_node = std::numeric_limits<ast_node_id>::max();

    // =========================================================================
    // ast_arena<NodeVariant>
    // =========================================================================

    template <class NodeVariant>
    class ast_arena {
    public:
        using node_type = NodeVariant;

        [[nodiscard]] ast_node_id push(NodeVariant node) {
            const auto id = static_cast<ast_node_id>(nodes_.size());
            nodes_.push_back(std::move(node));
            return id;
        }

        // Reserve node storage when a frontend has a reliable capacity hint.
        // This is opt-in: callers that do not provide one retain the original
        // zero-extra-work growth behaviour.
        void reserve(const std::size_t node_capacity) {
            nodes_.reserve(node_capacity);
        }

        [[nodiscard]] const NodeVariant& operator[](ast_node_id id) const {
            return nodes_[id];
        }

        [[nodiscard]] NodeVariant& operator[](ast_node_id id) {
            return nodes_[id];
        }

        [[nodiscard]] std::size_t size()  const noexcept { return nodes_.size();  }
        [[nodiscard]] bool        empty() const noexcept { return nodes_.empty(); }

        using iterator       = typename std::vector<NodeVariant>::iterator;
        using const_iterator = typename std::vector<NodeVariant>::const_iterator;

        iterator       begin() noexcept       { return nodes_.begin(); }
        iterator       end()   noexcept       { return nodes_.end();   }
        const_iterator begin() const noexcept { return nodes_.begin(); }
        const_iterator end()   const noexcept { return nodes_.end();   }

    private:
        std::vector<NodeVariant> nodes_;
    };

} // namespace lang
