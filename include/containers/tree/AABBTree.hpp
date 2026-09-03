#pragma once
// containers/tree/AABBTree.hpp — dynamic bounding-volume hierarchy (2D/3D-agnostic via
// the AABB type parameter). Geometry-agnostic: stores an AABB + an opaque payload
// per leaf. Header-only, C++23, no virtual, no macros, no external deps.
//
// Design: binary tree over a flat node pool (union of internal + leaf). Leaves carry a
// user payload; internals carry the merged AABB of their subtree. Insertion picks the
// sibling minimizing merged surface area (SAH-lite), rebalances via AVL rotations and
// AABB refit up the parent chain. Nodes are recycled through a free list, so handles
// (node indices) are stable until removed. Fat AABBs (margin) reduce churn under small motion.
//
// The AABB type must provide: static merge(a,b), .area(), .overlaps(other),
// .fattened(margin), .contains(point). akruti::AABB and prakriti::AABB both satisfy this.

#include <cstdint>
#include <vector>
#include <limits>
#include <utility>
#include <algorithm>
#include <memory>

#include "containers/dynamic/SmallVector.hpp"

namespace containers {
    template <
        class AABB,
        class Vec = decltype(AABB{}.lo),
        typename Payload = std::uint32_t,
        typename Allocator = std::allocator<std::byte>
    >
    class AABBTree {
    public:
        using Scalar = decltype(AABB{}.area());
        using NodeIndex = std::uint32_t;
        static constexpr NodeIndex null_node = std::numeric_limits<NodeIndex>::max();
        static constexpr NodeIndex kNullNode = null_node;

        explicit AABBTree(Scalar margin = Scalar(0), const Allocator& alloc = Allocator()) noexcept
            : nodes_(alloc), margin_(margin) {}

        void reserve(std::size_t leaf_capacity) {
            nodes_.reserve(leaf_capacity * 2);
        }

        // Insert a leaf for `payload` bounded by `box`. Returns the stable node index.
        NodeIndex insert(const AABB& box, Payload payload) {
            const NodeIndex leaf = alloc_node();
            nodes_[leaf].box = box.fattened(margin_);
            nodes_[leaf].payload = std::move(payload);
            nodes_[leaf].height = 0;
            nodes_[leaf].left = nodes_[leaf].right = null_node;
            insert_leaf(leaf);
            return leaf;
        }

        void remove(NodeIndex leaf) {
            remove_leaf(leaf);
            free_node(leaf);
        }

        // Re-fit a moved leaf. If the new tight box still sits inside the stored fat box,
        // nothing to do; otherwise reinsert with a fresh fat box.
        bool update(NodeIndex leaf, const AABB& box) {
            if (contains_box(nodes_[leaf].box, box)) return false;
            remove_leaf(leaf);
            nodes_[leaf].box = box.fattened(margin_);
            insert_leaf(leaf);
            return true;
        }

        // Report every leaf payload whose fat box overlaps `query`. fn(payload).
        // Uses SmallVector stack for zero heap allocation up to 64 levels.
        template <class Fn>
        void query(const AABB& query_box, Fn&& fn) const {
            if (root_ == null_node) return;
            pebble::containers::SmallVector<NodeIndex, 64> stack;
            stack.push_back(root_);
            while (!stack.empty()) {
                const NodeIndex n = stack.back();
                stack.pop_back();
                if (n == null_node) continue;
                const Node& nd = nodes_[n];
                if (!nd.box.overlaps(query_box)) continue;
                if (nd.is_leaf()) {
                    fn(nd.payload);
                }
                else {
                    stack.push_back(nd.left);
                    stack.push_back(nd.right);
                }
            }
        }

        // Ray vs tree: slab test per node, report leaf payloads on hit branches. fn(payload).
        // Caller does the exact primitive raycast against the reported payloads.
        template <class Fn>
        void raycast(const Vec& origin, const Vec& dir, Scalar tmax, Fn&& fn) const {
            if (root_ == null_node) return;
            const Scalar dx = get_x(dir), dy = get_y(dir);
            const Vec inv{
                dx != Scalar(0) ? Scalar(1) / dx : std::numeric_limits<Scalar>::infinity(),
                dy != Scalar(0) ? Scalar(1) / dy : std::numeric_limits<Scalar>::infinity()
            };
            pebble::containers::SmallVector<NodeIndex, 64> stack;
            stack.push_back(root_);
            while (!stack.empty()) {
                const NodeIndex n = stack.back();
                stack.pop_back();
                if (n == null_node) continue;
                const Node& nd = nodes_[n];
                if (!ray_hits(nd.box, origin, inv, tmax)) continue;
                if (nd.is_leaf()) {
                    fn(nd.payload);
                }
                else {
                    stack.push_back(nd.left);
                    stack.push_back(nd.right);
                }
            }
        }

        [[nodiscard]] std::size_t size() const noexcept { return leaf_count_; }
        [[nodiscard]] bool empty() const noexcept { return root_ == null_node; }

        void clear() noexcept {
            nodes_.clear();
            free_ = null_node;
            root_ = null_node;
            leaf_count_ = 0;
        }

    private:
        struct Node {
            AABB box{};
            Payload payload{};
            NodeIndex parent = null_node;
            NodeIndex left = null_node;
            NodeIndex right = null_node;
            std::int32_t height = -1; // -1 = free-list slot
            [[nodiscard]] constexpr bool is_leaf() const noexcept { return left == null_node; }
        };

        using NodeAlloc = typename std::allocator_traits<Allocator>::template rebind_alloc<Node>;

        template <class V>
        static constexpr auto get_x(const V& v) noexcept {
            if constexpr (requires { v.x; }) return v.x;
            else return v[0];
        }

        template <class V>
        static constexpr auto get_y(const V& v) noexcept {
            if constexpr (requires { v.y; }) return v.y;
            else return v[1];
        }

        static bool contains_box(const AABB& outer, const AABB& inner) noexcept {
            return get_x(outer.lo) <= get_x(inner.lo) && get_y(outer.lo) <= get_y(inner.lo) &&
                get_x(outer.hi) >= get_x(inner.hi) && get_y(outer.hi) >= get_y(inner.hi);
        }

        static bool ray_hits(const AABB& b, const Vec& o, const Vec& inv, Scalar tmax) noexcept {
            Scalar t1 = (get_x(b.lo) - get_x(o)) * get_x(inv);
            Scalar t2 = (get_x(b.hi) - get_x(o)) * get_x(inv);
            Scalar tmin = std::min(t1, t2);
            Scalar tmx = std::max(t1, t2);

            t1 = (get_y(b.lo) - get_y(o)) * get_y(inv);
            t2 = (get_y(b.hi) - get_y(o)) * get_y(inv);
            tmin = std::max(tmin, std::min(t1, t2));
            tmx = std::min(tmx, std::max(t1, t2));

            return tmx >= std::max(tmin, Scalar(0)) && tmin <= tmax;
        }

        NodeIndex alloc_node() {
            if (free_ != null_node) {
                const NodeIndex n = free_;
                free_ = nodes_[n].left; // free list threaded through .left
                nodes_[n] = Node{};
                return n;
            }
            nodes_.emplace_back();
            return static_cast<NodeIndex>(nodes_.size() - 1);
        }

        void free_node(NodeIndex n) noexcept {
            nodes_[n].left = free_;
            nodes_[n].height = -1;
            free_ = n;
        }

        void insert_leaf(NodeIndex leaf) {
            ++leaf_count_;
            if (root_ == null_node) {
                root_ = leaf;
                nodes_[leaf].parent = null_node;
                return;
            }

            // Descend to the best sibling by minimal merged area.
            const AABB lb = nodes_[leaf].box;
            NodeIndex cur = root_;
            while (!nodes_[cur].is_leaf()) {
                const NodeIndex l = nodes_[cur].left, r = nodes_[cur].right;
                const Scalar combined = AABB::merge(nodes_[cur].box, lb).area();
                const Scalar cost = Scalar(2) * combined;
                const Scalar inherit = Scalar(2) * (combined - nodes_[cur].box.area());
                const Scalar cl = AABB::merge(nodes_[l].box, lb).area()
                    - (nodes_[l].is_leaf() ? Scalar(0) : nodes_[l].box.area()) + inherit;
                const Scalar cr = AABB::merge(nodes_[r].box, lb).area()
                    - (nodes_[r].is_leaf() ? Scalar(0) : nodes_[r].box.area()) + inherit;
                if (cost < cl && cost < cr) break;
                cur = (cl < cr) ? l : r;
            }

            // New internal parent replaces `cur` under old grandparent.
            const NodeIndex old_parent = nodes_[cur].parent;
            const NodeIndex new_parent = alloc_node();
            nodes_[new_parent].parent = old_parent;
            nodes_[new_parent].box = AABB::merge(lb, nodes_[cur].box);
            nodes_[new_parent].height = nodes_[cur].height + 1;
            nodes_[new_parent].left = cur;
            nodes_[new_parent].right = leaf;
            nodes_[cur].parent = new_parent;
            nodes_[leaf].parent = new_parent;

            if (old_parent == null_node) {
                root_ = new_parent;
            }
            else {
                if (nodes_[old_parent].left == cur) nodes_[old_parent].left = new_parent;
                else nodes_[old_parent].right = new_parent;
            }
            balance_and_refit(nodes_[leaf].parent);
        }

        void remove_leaf(NodeIndex leaf) {
            --leaf_count_;
            if (leaf == root_) {
                root_ = null_node;
                return;
            }
            const NodeIndex parent = nodes_[leaf].parent;
            const NodeIndex grand = nodes_[parent].parent;
            const NodeIndex sib = (nodes_[parent].left == leaf)
                                      ? nodes_[parent].right
                                      : nodes_[parent].left;
            if (grand == null_node) {
                root_ = sib;
                nodes_[sib].parent = null_node;
            }
            else {
                if (nodes_[grand].left == parent) nodes_[grand].left = sib;
                else nodes_[grand].right = sib;
                nodes_[sib].parent = grand;
                balance_and_refit(grand);
            }
            free_node(parent);
        }

        NodeIndex balance(NodeIndex iA) {
            Node* A = &nodes_[iA];
            if (A->is_leaf() || A->height < 2) return iA;

            const NodeIndex iB = A->left;
            const NodeIndex iC = A->right;
            Node* B = &nodes_[iB];
            Node* C = &nodes_[iC];

            const int balance_factor = C->height - B->height;

            if (balance_factor > 1) {
                const NodeIndex iF = C->left;
                const NodeIndex iG = C->right;
                Node* F = &nodes_[iF];
                Node* G = &nodes_[iG];

                C->left = iA;
                C->parent = A->parent;
                A->parent = iC;

                if (C->parent != null_node) {
                    if (nodes_[C->parent].left == iA) nodes_[C->parent].left = iC;
                    else nodes_[C->parent].right = iC;
                }
                else {
                    root_ = iC;
                }

                if (F->height > G->height) {
                    C->right = iF;
                    A->right = iG;
                    G->parent = iA;
                    A->box = AABB::merge(B->box, G->box);
                    C->box = AABB::merge(A->box, F->box);
                    A->height = 1 + std::max(B->height, G->height);
                    C->height = 1 + std::max(A->height, F->height);
                }
                else {
                    C->right = iG;
                    A->right = iF;
                    F->parent = iA;
                    A->box = AABB::merge(B->box, F->box);
                    C->box = AABB::merge(A->box, G->box);
                    A->height = 1 + std::max(B->height, F->height);
                    C->height = 1 + std::max(A->height, G->height);
                }
                return iC;
            }

            if (balance_factor < -1) {
                const NodeIndex iD = B->left;
                const NodeIndex iE = B->right;
                Node* D = &nodes_[iD];
                Node* E = &nodes_[iE];

                B->left = iA;
                B->parent = A->parent;
                A->parent = iB;

                if (B->parent != null_node) {
                    if (nodes_[B->parent].left == iA) nodes_[B->parent].left = iB;
                    else nodes_[B->parent].right = iB;
                }
                else {
                    root_ = iB;
                }

                if (D->height > E->height) {
                    B->right = iD;
                    A->left = iE;
                    E->parent = iA;
                    A->box = AABB::merge(C->box, E->box);
                    B->box = AABB::merge(A->box, D->box);
                    A->height = 1 + std::max(C->height, E->height);
                    B->height = 1 + std::max(A->height, D->height);
                }
                else {
                    B->right = iE;
                    A->left = iD;
                    D->parent = iA;
                    A->box = AABB::merge(C->box, D->box);
                    B->box = AABB::merge(A->box, E->box);
                    A->height = 1 + std::max(C->height, D->height);
                    B->height = 1 + std::max(A->height, E->height);
                }
                return iB;
            }

            return iA;
        }

        void balance_and_refit(NodeIndex n) {
            while (n != null_node) {
                n = balance(n);
                const NodeIndex l = nodes_[n].left, r = nodes_[n].right;
                nodes_[n].box = AABB::merge(nodes_[l].box, nodes_[r].box);
                nodes_[n].height = 1 + std::max(nodes_[l].height, nodes_[r].height);
                n = nodes_[n].parent;
            }
        }

        std::vector<Node, NodeAlloc> nodes_;
        NodeIndex free_ = null_node;
        NodeIndex root_ = null_node;
        std::size_t leaf_count_ = 0;
        Scalar margin_;
    };
} // namespace containers

namespace pebble::containers {
    using ::containers::AABBTree;

    template <
        class AABB,
        class Vec = decltype(AABB{}.lo),
        typename Payload = std::uint32_t,
        typename Allocator = std::allocator<std::byte>
    >
    using ScalableAABBTree = ::containers::AABBTree<AABB, Vec, Payload, Allocator>;
} // namespace pebble::containers
