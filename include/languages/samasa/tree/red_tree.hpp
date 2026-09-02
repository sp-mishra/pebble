#pragma once

// samasa/tree/red_tree.hpp — Parent-adding view over a green_tree.
//
// C++23, header-only, no virtual, no macros. Namespace: lang::samasa
//
// red_node — adds parent + child_index to a green_id reference.
// red_tree<SK> — built lazily on demand from a green_tree; not stored persistently.
//   Use for traversal and lowering (AST construction, binders).
//   Freed when the red_tree object is destroyed; green_tree owns the data.

#include <cstdint>
#include <limits>
#include <vector>
#include "green_tree.hpp"

namespace lang::samasa {
    using red_id = std::uint32_t;
    inline constexpr red_id k_null_red = std::numeric_limits<std::uint32_t>::max();

    template <class SyntaxKind>
    struct red_node {
        green_id green = k_null_green;
        red_id parent = k_null_red;
        std::uint32_t child_index = 0; // index among parent's children
    };

    // red_tree<SK> — flat arena of red_nodes built from a green_tree.
    // Traversal: use children(id) to get child red_ids.
    template <class SyntaxKind>
    class red_tree {
    public:
        using node_type = red_node<SyntaxKind>;

        // Build a complete red view from a green_tree (O(N)).
        static red_tree build(const green_tree<SyntaxKind>& green) {
            red_tree rt;
            if (green.empty()) return rt;
            const std::uint32_t n = green.size();
            rt.nodes_.resize(n, {k_null_green, k_null_red, 0});

            // BFS from root.
            green_id root = green.root();
            if (root == k_null_green) return rt;
            rt.nodes_[root].green = root;
            rt.nodes_[root].parent = k_null_red;

            std::vector<green_id> queue;
            queue.push_back(root);

            for (std::uint32_t qi = 0; qi < queue.size(); ++qi) {
                green_id gid = queue[qi];
                auto children = green.children(gid);
                for (std::uint32_t i = 0; i < children.size(); ++i) {
                    green_id child = children[i];
                    rt.nodes_[child].green = child;
                    rt.nodes_[child].parent = static_cast<red_id>(gid);
                    rt.nodes_[child].child_index = i;
                    queue.push_back(child);
                }
            }
            rt.root_id_ = static_cast<red_id>(root);
            return rt;
        }

        [[nodiscard]] red_id root() const noexcept { return root_id_; }
        [[nodiscard]] bool empty() const noexcept { return nodes_.empty(); }

        [[nodiscard]] const node_type& operator[](red_id id) const { return nodes_[id]; }

        // Parent traversal.
        [[nodiscard]] red_id parent_of(red_id id) const noexcept {
            return nodes_[id].parent;
        }

        // Green node for a red_id.
        [[nodiscard]] green_id green_of(red_id id) const noexcept {
            return nodes_[id].green;
        }

    private:
        std::vector<node_type> nodes_;
        red_id root_id_ = k_null_red;
    };
} // namespace lang::samasa
