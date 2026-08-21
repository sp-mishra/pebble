#pragma once
// containers/tree/AABBTree.hpp — dynamic bounding-volume hierarchy (2D/3D-agnostic via
// the AABB type parameter). Geometry-agnostic: stores an AABB + an opaque payload id
// (uint32_t) per leaf. Header-only, C++23, no virtual, no macros, no external deps.
//
// Design: binary tree over a flat node pool (union of internal + leaf). Leaves carry a
// user payload; internals carry the merged AABB of their subtree. Insertion picks the
// sibling minimizing merged surface area (SAH-lite), rebalances via AABB refit up the
// parent chain. Nodes are recycled through a free list, so handles (node indices) are
// stable until removed. Fat AABBs (margin) reduce churn under small motion.
//
// The AABB type must provide: static merge(a,b), .area(), .overlaps(other),
// .fattened(margin), .contains(point). akruti::AABB and prakriti::AABB both satisfy this.

#include <cstdint>
#include <vector>
#include <limits>
#include <utility>

namespace containers {
    template <class AABB, class Vec = decltype(AABB{}.lo)>
    class AABBTree {
    public:
        using Scalar = decltype(AABB{}.area());
        static constexpr std::uint32_t null_node = std::numeric_limits<std::uint32_t>::max();

        explicit AABBTree(Scalar margin = Scalar(0)) noexcept : margin_(margin) {}

        // Insert a leaf for `payload` bounded by `box`. Returns the stable node index.
        std::uint32_t insert(const AABB& box, std::uint32_t payload) {
            const std::uint32_t leaf = alloc_node();
            nodes_[leaf].box = box.fattened(margin_);
            nodes_[leaf].payload = payload;
            nodes_[leaf].height = 0;
            nodes_[leaf].left = nodes_[leaf].right = null_node;
            insert_leaf(leaf);
            return leaf;
        }

        void remove(std::uint32_t leaf) {
            remove_leaf(leaf);
            free_node(leaf);
        }

        // Re-fit a moved leaf. If the new tight box still sits inside the stored fat box,
        // nothing to do; otherwise reinsert with a fresh fat box.
        bool update(std::uint32_t leaf, const AABB& box) {
            if (contains_box(nodes_[leaf].box, box)) return false;
            remove_leaf(leaf);
            nodes_[leaf].box = box.fattened(margin_);
            insert_leaf(leaf);
            return true;
        }

        // Report every leaf payload whose fat box overlaps `query`. fn(payload).
        template <class Fn>
        void query(const AABB& query, Fn&& fn) const {
            if (root_ == null_node) return;
            std::uint32_t stack[64];
            int sp = 0;
            stack[sp++] = root_;
            while (sp > 0) {
                const std::uint32_t n = stack[--sp];
                if (n == null_node) continue;
                const Node& nd = nodes_[n];
                if (!nd.box.overlaps(query)) continue;
                if (nd.is_leaf()) {
                    fn(nd.payload);
                }
                else if (sp + 2 <= 64) {
                    stack[sp++] = nd.left;
                    stack[sp++] = nd.right;
                }
            }
        }

        // Ray vs tree: slab test per node, report leaf payloads on hit branches. fn(payload).
        // Caller does the exact primitive raycast against the reported payloads.
        template <class Fn>
        void raycast(const Vec& origin, const Vec& dir, Scalar tmax, Fn&& fn) const {
            if (root_ == null_node) return;
            const Vec inv{
                Scalar(1) / (dir.x != Scalar(0) ? dir.x : Scalar(1e-30)),
                Scalar(1) / (dir.y != Scalar(0) ? dir.y : Scalar(1e-30))
            };
            std::uint32_t stack[64];
            int sp = 0;
            stack[sp++] = root_;
            while (sp > 0) {
                const std::uint32_t n = stack[--sp];
                if (n == null_node) continue;
                const Node& nd = nodes_[n];
                if (!ray_hits(nd.box, origin, inv, tmax)) continue;
                if (nd.is_leaf()) {
                    fn(nd.payload);
                }
                else if (sp + 2 <= 64) {
                    stack[sp++] = nd.left;
                    stack[sp++] = nd.right;
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
            std::uint32_t payload = 0;
            std::uint32_t parent = null_node;
            std::uint32_t left = null_node;
            std::uint32_t right = null_node;
            std::int32_t height = -1; // -1 = free-list slot
            [[nodiscard]] bool is_leaf() const noexcept { return left == null_node; }
        };

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
            Scalar t1 = (get_x(b.lo) - get_x(o)) * get_x(inv), t2 = (get_x(b.hi) - get_x(o)) * get_x(inv);
            Scalar tmin = t1 < t2 ? t1 : t2, tmx = t1 < t2 ? t2 : t1;
            t1 = (get_y(b.lo) - get_y(o)) * get_y(inv);
            t2 = (get_y(b.hi) - get_y(o)) * get_y(inv);
            tmin = (t1 < t2 ? t1 : t2) > tmin ? (t1 < t2 ? t1 : t2) : tmin;
            tmx = (t1 < t2 ? t2 : t1) < tmx ? (t1 < t2 ? t2 : t1) : tmx;
            return tmx >= tmin && tmx >= Scalar(0) && tmin <= tmax;
        }


        std::uint32_t alloc_node() {
            if (free_ != null_node) {
                const std::uint32_t n = free_;
                free_ = nodes_[n].left; // free list threaded through .left
                nodes_[n] = Node{};
                return n;
            }
            nodes_.push_back(Node{});
            return static_cast<std::uint32_t>(nodes_.size() - 1);
        }

        void free_node(std::uint32_t n) noexcept {
            nodes_[n].left = free_;
            nodes_[n].height = -1;
            free_ = n;
        }

        void insert_leaf(std::uint32_t leaf) {
            ++leaf_count_;
            if (root_ == null_node) {
                root_ = leaf;
                nodes_[leaf].parent = null_node;
                return;
            }

            // Descend to the best sibling by minimal merged area.
            const AABB lb = nodes_[leaf].box;
            std::uint32_t cur = root_;
            while (!nodes_[cur].is_leaf()) {
                const std::uint32_t l = nodes_[cur].left, r = nodes_[cur].right;
                const Scalar combined = AABB::merge(nodes_[cur].box, lb).area();
                const Scalar cost = Scalar(2) * combined;
                const Scalar inherit = Scalar(2) * (combined - nodes_[cur].box.area());
                const Scalar cl = AABB::merge(nodes_[l].box, lb).area()
                    - (nodes_[l].is_leaf() ? Scalar(0) : nodes_[l].box.area()) + inherit;
                const Scalar cr = AABB::merge(nodes_[r].box, lb).area()
                    - (nodes_[r].is_leaf() ? Scalar(0) : nodes_[r].box.area()) + inherit;
                if (cost < cl && cost < cr) break;
                cur = cl < cr ? l : r;
            }

            // New internal parent replaces `cur` under old grandparent.
            const std::uint32_t old_parent = nodes_[cur].parent;
            const std::uint32_t new_parent = alloc_node();
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
            refit(nodes_[leaf].parent);
        }

        void remove_leaf(std::uint32_t leaf) {
            --leaf_count_;
            if (leaf == root_) {
                root_ = null_node;
                return;
            }
            const std::uint32_t parent = nodes_[leaf].parent;
            const std::uint32_t grand = nodes_[parent].parent;
            const std::uint32_t sib = (nodes_[parent].left == leaf)
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
                refit(grand);
            }
            free_node(parent);
        }

        void refit(std::uint32_t n) {
            while (n != null_node) {
                const std::uint32_t l = nodes_[n].left, r = nodes_[n].right;
                nodes_[n].box = AABB::merge(nodes_[l].box, nodes_[r].box);
                nodes_[n].height = 1 + (nodes_[l].height > nodes_[r].height
                                            ? nodes_[l].height
                                            : nodes_[r].height);
                n = nodes_[n].parent;
            }
        }

        std::vector<Node> nodes_;
        std::uint32_t free_ = null_node;
        std::uint32_t root_ = null_node;
        std::size_t leaf_count_ = 0;
        Scalar margin_;
    };
} // namespace containers
