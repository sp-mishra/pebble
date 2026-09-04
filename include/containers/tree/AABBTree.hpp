#pragma once
// containers/tree/AABBTree.hpp — dynamic bounding-volume hierarchy (2D/3D-agnostic via
// the AABB type parameter). Geometry-agnostic: stores an AABB + an opaque payload
// per leaf. Header-only, C++23, no virtual, no macros, no external deps.

#include <cstdint>
#include <vector>
#include <limits>
#include <utility>
#include <algorithm>
#include <memory>
#include <type_traits>

#include "containers/dynamic/SmallVector.hpp"

#if __has_include(<hwy/highway.h>)
#include <hwy/highway.h>
#endif

namespace pebble::containers {namespace aabb {
        using NodeIndex = std::uint32_t;
        inline constexpr NodeIndex kNullNode = std::numeric_limits<NodeIndex>::max();

        template <class V>
        static constexpr auto get_x(const V& v) noexcept {
            if constexpr (requires { v.x; }) return v.x;
            else if constexpr (requires { v.x(); }) return v.x();
            else return v[0];
        }

        template <class V>
        static constexpr auto get_y(const V& v) noexcept {
            if constexpr (requires { v.y; }) return v.y;
            else if constexpr (requires { v.y(); }) return v.y();
            else return v[1];
        }

        // -------------------------------------------------------------------------
        // Binary Branching Policy: 2-way AVL-Balanced Tree
        // -------------------------------------------------------------------------
        template <class AABB, class Vec, typename Payload, typename Allocator>
        class BinaryBranchingPolicy {
        public:
            using Scalar = decltype(AABB{}.area());

            struct Node {
                AABB box{};
                Payload payload{};
                NodeIndex parent{kNullNode};
                NodeIndex left{kNullNode};
                NodeIndex right{kNullNode};
                std::int32_t height{-1};

                [[nodiscard]] constexpr bool is_leaf() const noexcept { return left == kNullNode; }
            };

            using NodeAlloc = std::allocator_traits<Allocator>::template rebind_alloc<Node>;
            std::vector<Node, NodeAlloc> nodes;
            NodeIndex root{kNullNode};
            NodeIndex free_head{kNullNode};
            std::size_t leaf_count{0};

            explicit BinaryBranchingPolicy(const Allocator& alloc) : nodes(alloc) {}

            void reserve(const std::size_t leaf_cap) { nodes.reserve(leaf_cap * 2); }

            NodeIndex alloc_node() {
                if (free_head != kNullNode) {
                    const NodeIndex n = free_head;
                    free_head = nodes[n].left;
                    nodes[n] = Node{};
                    return n;
                }
                nodes.emplace_back();
                return static_cast<NodeIndex>(nodes.size() - 1);
            }

            void free_node(NodeIndex n) noexcept {
                nodes[n].payload = Payload{};
                nodes[n].left = free_head;
                nodes[n].height = -1;
                free_head = n;
            }

            NodeIndex insert(const AABB& fat_box, Payload payload) {
                const NodeIndex leaf = alloc_node();
                nodes[leaf].box = fat_box;
                nodes[leaf].payload = std::move(payload);
                nodes[leaf].height = 0;
                nodes[leaf].left = nodes[leaf].right = kNullNode;

                ++leaf_count;
                if (root == kNullNode) {
                    root = leaf;
                    nodes[leaf].parent = kNullNode;
                    return leaf;
                }

                NodeIndex cur = root;
                while (!nodes[cur].is_leaf()) {
                    const NodeIndex l = nodes[cur].left, r = nodes[cur].right;
                    const Scalar combined = AABB::merge(nodes[cur].box, fat_box).area();
                    const Scalar cost = Scalar(2) * combined;
                    const Scalar inherit = Scalar(2) * (combined - nodes[cur].box.area());
                    const Scalar cl = AABB::merge(nodes[l].box, fat_box).area() - (nodes[l].is_leaf()
                            ? Scalar(0)
                            : nodes[l].box.area()) + inherit;
                    const Scalar cr = AABB::merge(nodes[r].box, fat_box).area() - (nodes[r].is_leaf()
                            ? Scalar(0)
                            : nodes[r].box.area()) + inherit;
                    if (cost < cl && cost < cr) break;
                    cur = (cl < cr) ? l : r;
                }

                const NodeIndex old_parent = nodes[cur].parent;
                const NodeIndex new_parent = alloc_node();
                nodes[new_parent].parent = old_parent;
                nodes[new_parent].box = AABB::merge(fat_box, nodes[cur].box);
                nodes[new_parent].height = nodes[cur].height + 1;
                nodes[new_parent].left = cur;
                nodes[new_parent].right = leaf;
                nodes[cur].parent = new_parent;
                nodes[leaf].parent = new_parent;

                if (old_parent == kNullNode) root = new_parent;
                else {
                    if (nodes[old_parent].left == cur) nodes[old_parent].left = new_parent;
                    else nodes[old_parent].right = new_parent;
                }

                balance_and_refit(nodes[leaf].parent);
                return leaf;
            }

            void remove(NodeIndex leaf) {
                if (leaf >= nodes.size() || nodes[leaf].height == -1) return;
                --leaf_count;
                if (leaf == root) {
                    root = kNullNode;
                    free_node(leaf);
                    return;
                }
                const NodeIndex parent = nodes[leaf].parent;
                const NodeIndex grand = nodes[parent].parent;
                const NodeIndex sib = (nodes[parent].left == leaf) ? nodes[parent].right : nodes[parent].left;

                if (grand == kNullNode) {
                    root = sib;
                    nodes[sib].parent = kNullNode;
                }
                else {
                    if (nodes[grand].left == parent) nodes[grand].left = sib;
                    else nodes[grand].right = sib;
                    nodes[sib].parent = grand;
                    balance_and_refit(grand);
                }
                free_node(parent);
                free_node(leaf);
            }

            bool update(NodeIndex leaf, const AABB& tight_box, Scalar margin) {
                if (leaf >= nodes.size() || nodes[leaf].height == -1) return false;
                if (contains_box(nodes[leaf].box, tight_box)) return false;
                Payload p = std::move(nodes[leaf].payload);
                remove(leaf);
                (void)insert(tight_box.fattened(margin), std::move(p));
                return true;
            }

            template <class Fn>
            void query(const AABB& query_box, Fn&& fn) const {
                if (root == kNullNode) return;
                pebble::containers::SmallVector<NodeIndex, 64> stack;
                stack.push_back(root);
                while (!stack.empty()) {
                    const NodeIndex n = stack.back();
                    stack.pop_back();
                    if (n == kNullNode) continue;
                    const auto& nd = nodes[n];
                    if (!nd.box.overlaps(query_box)) continue;
                    if (nd.is_leaf()) fn(nd.payload);
                    else {
                        stack.push_back(nd.left);
                        stack.push_back(nd.right);
                    }
                }
            }

            template <class Fn>
            void raycast(const Vec& origin, const Vec& dir, Scalar tmax, Fn&& fn) const {
                if (root == kNullNode) return;
                const Scalar dx = get_x(dir), dy = get_y(dir);
                const Vec inv{
                    dx != Scalar(0) ? Scalar(1) / dx : std::numeric_limits<Scalar>::infinity(),
                    dy != Scalar(0) ? Scalar(1) / dy : std::numeric_limits<Scalar>::infinity()
                };
                pebble::containers::SmallVector<NodeIndex, 64> stack;
                stack.push_back(root);
                while (!stack.empty()) {
                    const NodeIndex n = stack.back();
                    stack.pop_back();
                    if (n == kNullNode) continue;
                    const auto& nd = nodes[n];
                    if (!ray_hits(nd.box, origin, inv, tmax)) continue;
                    if (nd.is_leaf()) fn(nd.payload);
                    else {
                        stack.push_back(nd.left);
                        stack.push_back(nd.right);
                    }
                }
            }

            void clear() noexcept {
                nodes.clear();
                free_head = kNullNode;
                root = kNullNode;
                leaf_count = 0;
            }

        private:
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

            NodeIndex balance(NodeIndex iA) {
                Node* A = &nodes[iA];
                if (A->is_leaf() || A->height < 2) return iA;
                const NodeIndex iB = A->left, iC = A->right;
                Node* B = &nodes[iB];
                Node* C = &nodes[iC];
                const int balance_factor = C->height - B->height;

                if (balance_factor > 1) {
                    const NodeIndex iF = C->left, iG = C->right;
                    Node* F = &nodes[iF];
                    Node* G = &nodes[iG];
                    C->left = iA;
                    C->parent = A->parent;
                    A->parent = iC;
                    if (C->parent != kNullNode) {
                        if (nodes[C->parent].left == iA) nodes[C->parent].left = iC;
                        else nodes[C->parent].right = iC;
                    }
                    else root = iC;

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
                    const NodeIndex iD = B->left, iE = B->right;
                    Node* D = &nodes[iD];
                    Node* E = &nodes[iE];
                    B->left = iA;
                    B->parent = A->parent;
                    A->parent = iB;
                    if (B->parent != kNullNode) {
                        if (nodes[B->parent].left == iA) nodes[B->parent].left = iB;
                        else nodes[B->parent].right = iB;
                    }
                    else root = iB;

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
                while (n != kNullNode) {
                    n = balance(n);
                    const NodeIndex l = nodes[n].left, r = nodes[n].right;
                    nodes[n].box = AABB::merge(nodes[l].box, nodes[r].box);
                    nodes[n].height = 1 + std::max(nodes[l].height, nodes[r].height);
                    n = nodes[n].parent;
                }
            }
        };

        // -------------------------------------------------------------------------
        // Quad Branching Policy: 4-Way SIMD Highway Vectorized BVH
        // -------------------------------------------------------------------------
        template <class AABB, class Vec, typename Payload, typename Allocator>
        class QuadBranchingPolicy {
        public:
            using Scalar = decltype(AABB{}.area());

            struct alignas(64) QuadNode {
                Scalar min_x[4]{}, min_y[4]{}, max_x[4]{}, max_y[4]{};
                NodeIndex children[4]{kNullNode, kNullNode, kNullNode, kNullNode};
                Payload payloads[4]{};
                std::uint8_t is_leaf_mask{0};
                std::uint8_t count{0};
                NodeIndex parent{kNullNode};

                [[nodiscard]] constexpr bool is_child_leaf(const std::size_t idx) const noexcept {
                    return (is_leaf_mask & (1u << idx)) != 0;
                }

                constexpr void set_child_leaf(const std::size_t idx, const bool leaf) noexcept {
                    if (leaf) is_leaf_mask |= static_cast<std::uint8_t>(1u << idx);
                    else is_leaf_mask &= static_cast<std::uint8_t>(~(1u << idx));
                }
            };

            using NodeAlloc = std::allocator_traits<Allocator>::template rebind_alloc<QuadNode>;
            std::vector<QuadNode, NodeAlloc> nodes;
            NodeIndex root{kNullNode};
            NodeIndex free_head{kNullNode};
            std::size_t leaf_count{0};

            explicit QuadBranchingPolicy(const Allocator& alloc) : nodes(alloc) {}

            void reserve(const std::size_t leaf_cap) { nodes.reserve((leaf_cap + 3) / 2); }

            NodeIndex alloc_node() {
                if (free_head != kNullNode) {
                    const NodeIndex n = free_head;
                    free_head = nodes[n].children[0];
                    nodes[n] = QuadNode{};
                    return n;
                }
                nodes.emplace_back();
                return static_cast<NodeIndex>(nodes.size() - 1);
            }

            void free_node(NodeIndex n) noexcept {
                nodes[n].children[0] = free_head;
                free_head = n;
            }

            NodeIndex insert(const AABB& fat_box, Payload payload) {
                ++leaf_count;
                if (root == kNullNode) {
                    root = alloc_node();
                    auto& r = nodes[root];
                    r.min_x[0] = get_x(fat_box.lo);
                    r.min_y[0] = get_y(fat_box.lo);
                    r.max_x[0] = get_x(fat_box.hi);
                    r.max_y[0] = get_y(fat_box.hi);
                    r.payloads[0] = std::move(payload);
                    r.set_child_leaf(0, true);
                    r.count = 1;
                    return root;
                }

                NodeIndex cur = root;
                while (true) {
                    auto& nd = nodes[cur];
                    if (nd.count < 4) {
                        const std::size_t slot = nd.count++;
                        nd.min_x[slot] = get_x(fat_box.lo);
                        nd.min_y[slot] = get_y(fat_box.lo);
                        nd.max_x[slot] = get_x(fat_box.hi);
                        nd.max_y[slot] = get_y(fat_box.hi);
                        nd.payloads[slot] = std::move(payload);
                        nd.set_child_leaf(slot, true);
                        refit_up(cur);
                        return cur;
                    }

                    std::size_t best_idx = 0;
                    Scalar best_cost = std::numeric_limits<Scalar>::infinity();
                    for (std::size_t i = 0; i < 4; ++i) {
                        const AABB cb{Vec{nd.min_x[i], nd.min_y[i]}, Vec{nd.max_x[i], nd.max_y[i]}};
                        const Scalar cost = AABB::merge(cb, fat_box).area() - cb.area();
                        if (cost < best_cost) {
                            best_cost = cost;
                            best_idx = i;
                        }
                    }

                    if (nd.is_child_leaf(best_idx)) {
                        const Scalar saved_min_x = nd.min_x[best_idx];
                        const Scalar saved_min_y = nd.min_y[best_idx];
                        const Scalar saved_max_x = nd.max_x[best_idx];
                        const Scalar saved_max_y = nd.max_y[best_idx];
                        Payload saved_payload = std::move(nd.payloads[best_idx]);

                        const NodeIndex new_internal = alloc_node();
                        auto& ni = nodes[new_internal];
                        ni.parent = cur;
                        ni.min_x[0] = saved_min_x;
                        ni.min_y[0] = saved_min_y;
                        ni.max_x[0] = saved_max_x;
                        ni.max_y[0] = saved_max_y;
                        ni.payloads[0] = std::move(saved_payload);
                        ni.set_child_leaf(0, true);

                        ni.min_x[1] = get_x(fat_box.lo);
                        ni.min_y[1] = get_y(fat_box.lo);
                        ni.max_x[1] = get_x(fat_box.hi);
                        ni.max_y[1] = get_y(fat_box.hi);
                        ni.payloads[1] = std::move(payload);
                        ni.set_child_leaf(1, true);
                        ni.count = 2;

                        auto& cur_nd = nodes[cur];
                        cur_nd.children[best_idx] = new_internal;
                        cur_nd.min_x[best_idx] = std::min(ni.min_x[0], ni.min_x[1]);
                        cur_nd.min_y[best_idx] = std::min(ni.min_y[0], ni.min_y[1]);
                        cur_nd.max_x[best_idx] = std::max(ni.max_x[0], ni.max_x[1]);
                        cur_nd.max_y[best_idx] = std::max(ni.max_y[0], ni.max_y[1]);
                        cur_nd.set_child_leaf(best_idx, false);
                        refit_up(cur);
                        return new_internal;
                    }
                    cur = nd.children[best_idx];
                }
            }

            void remove(NodeIndex /*leaf*/) noexcept {
                if (leaf_count > 0) --leaf_count;
            }

            bool update(NodeIndex /*leaf*/, const AABB& /*tight_box*/, Scalar /*margin*/) noexcept {
                return false;
            }

            template <class Fn>
            void query(const AABB& query_box, Fn&& fn) const {
                if (root == kNullNode) return;
                pebble::containers::SmallVector<NodeIndex, 64> stack;
                stack.push_back(root);

                const Scalar q_min_x = get_x(query_box.lo), q_min_y = get_y(query_box.lo);
                const Scalar q_max_x = get_x(query_box.hi), q_max_y = get_y(query_box.hi);

                while (!stack.empty()) {
                    const NodeIndex curr_idx = stack.back();
                    stack.pop_back();
                    if (curr_idx == kNullNode) continue;
                    const auto& nd = nodes[curr_idx];

#if defined(PEBBLE_HAS_HIGHWAY)
                    namespace hn = hwy::HWY_NAMESPACE;
                    constexpr hn::FixedTag<float, 4> d;
                    const auto v_min_x = hn::LoadU(d, nd.min_x), v_min_y = hn::LoadU(d, nd.min_y);
                    const auto v_max_x = hn::LoadU(d, nd.max_x), v_max_y = hn::LoadU(d, nd.max_y);
                    const auto v_qmin_x = hn::Set(d, q_min_x), v_qmin_y = hn::Set(d, q_min_y);
                    const auto v_qmax_x = hn::Set(d, q_max_x), v_qmax_y = hn::Set(d, q_max_y);

                    const auto no_x = hn::Or(hn::Lt(v_max_x, v_qmin_x), hn::Gt(v_min_x, v_qmax_x));
                    const auto no_y = hn::Or(hn::Lt(v_max_y, v_qmin_y), hn::Gt(v_min_y, v_qmax_y));
                    const auto overlap_mask = hn::Not(hn::Or(no_x, no_y));

                    const std::uint32_t mask = hn::BitsFromMask(d, overlap_mask) & 0xFu;
                    for (std::size_t i = 0; i < nd.count; ++i) {
                        if (mask & (1u << i)) {
                            if (nd.is_child_leaf(i)) fn(nd.payloads[i]);
                            else stack.push_back(nd.children[i]);
                        }
                    }
#else
                    for (std::size_t i = 0; i < nd.count; ++i) {
                        if (nd.max_x[i] >= q_min_x && nd.min_x[i] <= q_max_x &&
                            nd.max_y[i] >= q_min_y && nd.min_y[i] <= q_max_y) {
                            if (nd.is_child_leaf(i)) fn(nd.payloads[i]);
                            else stack.push_back(nd.children[i]);
                        }
                    }
#endif
                }
            }

            template <class Fn>
            void raycast(const Vec& origin, const Vec& dir, Scalar tmax, Fn&& fn) const {
                if (root == kNullNode) return;
                const Scalar dx = get_x(dir), dy = get_y(dir);
                const Scalar inv_x = dx != Scalar(0) ? Scalar(1) / dx : std::numeric_limits<Scalar>::infinity();
                const Scalar inv_y = dy != Scalar(0) ? Scalar(1) / dy : std::numeric_limits<Scalar>::infinity();
                const Scalar ox = get_x(origin), oy = get_y(origin);

                pebble::containers::SmallVector<NodeIndex, 64> stack;
                stack.push_back(root);

                while (!stack.empty()) {
                    const NodeIndex curr_idx = stack.back();
                    stack.pop_back();
                    if (curr_idx == kNullNode) continue;
                    const auto& nd = nodes[curr_idx];

#if defined(PEBBLE_HAS_HIGHWAY)
                    namespace hn = hwy::HWY_NAMESPACE;
                    constexpr hn::FixedTag<float, 4> d;
                    const auto v_min_x = hn::LoadU(d, nd.min_x), v_min_y = hn::LoadU(d, nd.min_y);
                    const auto v_max_x = hn::LoadU(d, nd.max_x), v_max_y = hn::LoadU(d, nd.max_y);
                    const auto v_ox = hn::Set(d, ox), v_oy = hn::Set(d, oy);
                    const auto v_inv_x = hn::Set(d, inv_x), v_inv_y = hn::Set(d, inv_y);
                    const auto v_zero = hn::Zero(d), v_tmax = hn::Set(d, tmax);

                    const auto t1x = hn::Mul(hn::Sub(v_min_x, v_ox), v_inv_x);
                    const auto t2x = hn::Mul(hn::Sub(v_max_x, v_ox), v_inv_x);
                    const auto tmin_x = hn::Min(t1x, t2x);
                    const auto tmax_x = hn::Max(t1x, t2x);

                    const auto t1y = hn::Mul(hn::Sub(v_min_y, v_oy), v_inv_y);
                    const auto t2y = hn::Mul(hn::Sub(v_max_y, v_oy), v_inv_y);
                    const auto tmin_y = hn::Min(t1y, t2y);
                    const auto tmax_y = hn::Max(t1y, t2y);

                    const auto tmin = hn::Max(tmin_x, tmin_y);
                    const auto tmax_box = hn::Min(tmax_x, tmax_y);

                    const auto hit_mask = hn::And(
                        hn::Ge(tmax_box, hn::Max(tmin, v_zero)),
                        hn::Le(tmin, v_tmax)
                    );

                    const std::uint32_t mask = hn::BitsFromMask(d, hit_mask) & 0xFu;
                    for (std::size_t i = 0; i < nd.count; ++i) {
                        if (mask & (1u << i)) {
                            if (nd.is_child_leaf(i)) fn(nd.payloads[i]);
                            else stack.push_back(nd.children[i]);
                        }
                    }
#else
                    for (std::size_t i = 0; i < nd.count; ++i) {
                        Scalar t1x = (nd.min_x[i] - ox) * inv_x;
                        Scalar t2x = (nd.max_x[i] - ox) * inv_x;
                        Scalar tmin_x = std::min(t1x, t2x);
                        Scalar tmax_x = std::max(t1x, t2x);

                        Scalar t1y = (nd.min_y[i] - oy) * inv_y;
                        Scalar t2y = (nd.max_y[i] - oy) * inv_y;
                        Scalar tmin_y = std::min(t1y, t2y);
                        Scalar tmax_y = std::max(t1y, t2y);

                        Scalar tmin = std::max(tmin_x, tmin_y);
                        Scalar tmax_box = std::min(tmax_x, tmax_y);

                        if (tmax_box >= std::max(tmin, Scalar(0)) && tmin <= tmax) {
                            if (nd.is_child_leaf(i)) fn(nd.payloads[i]);
                            else stack.push_back(nd.children[i]);
                        }
                    }
#endif
                }
            }

            void clear() noexcept {
                nodes.clear();
                free_head = kNullNode;
                root = kNullNode;
                leaf_count = 0;
            }

        private:
            void refit_up(NodeIndex idx) {
                while (idx != kNullNode) {
                    const NodeIndex p = nodes[idx].parent;
                    if (p != kNullNode) {
                        auto& nd = nodes[idx];
                        Scalar mx = nd.min_x[0], my = nd.min_y[0], xx = nd.max_x[0], xy = nd.max_y[0];
                        for (std::size_t j = 1; j < nd.count; ++j) {
                            mx = std::min(mx, nd.min_x[j]);
                            my = std::min(my, nd.min_y[j]);
                            xx = std::max(xx, nd.max_x[j]);
                            xy = std::max(xy, nd.max_y[j]);
                        }
                        auto& p_nd = nodes[p];
                        for (std::size_t i = 0; i < p_nd.count; ++i) {
                            if (!p_nd.is_child_leaf(i) && p_nd.children[i] == idx) {
                                p_nd.min_x[i] = mx;
                                p_nd.min_y[i] = my;
                                p_nd.max_x[i] = xx;
                                p_nd.max_y[i] = xy;
                                break;
                            }
                        }
                    }
                    idx = p;
                }
            }
        };

        template <class AABB, class Vec, typename Payload, typename Allocator>
        struct DefaultBranchingPolicy {
            using type = BinaryBranchingPolicy<AABB, Vec, Payload, Allocator>;
        };
    } // namespace aabb

    // =============================================================================
    // Unified AABBTree Container
    // =============================================================================
    template <
        class AABB,
        class Vec = decltype(AABB{}.lo),
        typename Payload = std::uint32_t,
        typename Allocator = std::allocator<std::byte>,
        class Policy = aabb::DefaultBranchingPolicy<AABB, Vec, Payload, Allocator>::type>
    class AABBTree {
    public:
        using Scalar = decltype(AABB{}.area());
        using NodeIndex = aabb::NodeIndex;
        static constexpr NodeIndex null_node = aabb::kNullNode;
        static constexpr NodeIndex kNullNode = null_node;
        using BranchingPolicy = Policy;

        explicit AABBTree(Scalar margin = Scalar(0), const Allocator& alloc = Allocator()) noexcept
            : policy_(alloc), margin_(margin) {}

        void reserve(std::size_t capacity) { policy_.reserve(capacity); }
        [[nodiscard]] std::size_t size() const noexcept { return policy_.leaf_count; }
        [[nodiscard]] bool empty() const noexcept { return policy_.leaf_count == 0; }

        NodeIndex insert(const AABB& box, Payload payload) {
            return policy_.insert(box.fattened(margin_), std::move(payload));
        }

        void remove(NodeIndex leaf) {
            policy_.remove(leaf);
        }

        bool update(NodeIndex leaf, const AABB& box) {
            return policy_.update(leaf, box, margin_);
        }

        template <class Fn>
        void query(const AABB& query_box, Fn&& fn) const {
            policy_.query(query_box, std::forward<Fn>(fn));
        }

        template <class Fn>
        void raycast(const Vec& origin, const Vec& dir, Scalar tmax, Fn&& fn) const {
            policy_.raycast(origin, dir, tmax, std::forward<Fn>(fn));
        }

        void clear() noexcept {
            policy_.clear();
        }

    private:
        BranchingPolicy policy_;
        Scalar margin_;
    };

    template <
        class AABB,
        class Vec = decltype(AABB{}.lo),
        typename Payload = std::uint32_t,
        typename Allocator = std::allocator<std::byte>>
    using ScalableAABBTree = AABBTree<AABB, Vec, Payload, Allocator>;
} // namespace pebble::containers

namespace containers {
    using pebble::containers::AABBTree;
    using pebble::containers::ScalableAABBTree;

    namespace aabb {
        using pebble::containers::aabb::BinaryBranchingPolicy;
        using pebble::containers::aabb::QuadBranchingPolicy;
        using pebble::containers::aabb::DefaultBranchingPolicy;
    }} // namespace containers
