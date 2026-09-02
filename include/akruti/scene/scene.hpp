#pragma once
// akruti/scene/scene.hpp — unified facade over per-type SoA batches with an AABBTree broadphase.
// Extended to support collision layer filtering and new game primitives (OrientedBox, Triangle, RoundedBox).
#include "../math.hpp"
#include "../primitives.hpp"
#include "batch.hpp"
#include "parallel.hpp"
#include "containers/tree/AABBTree.hpp"

#include <cstdint>
#include <tuple>
#include <utility>

namespace akruti::scene {
    // ── Collision Layer Filtering Mask ────────────────────────────────────────────────
    using LayerMask = std::uint32_t;
    inline constexpr LayerMask kAllLayers = ~0u;

    // The extended primitive set.
    using BatchTuple = std::tuple<ShapeBatch<Circle>,
                                  ShapeBatch<Box>,
                                  ShapeBatch<OrientedBox>,
                                  ShapeBatch<Triangle>,
                                  ShapeBatch<RoundedBox>,
                                  ShapeBatch<Segment>,
                                  ShapeBatch<Capsule>,
                                  ShapeBatch<HalfPlane>,
                                  ShapeBatch<ConvexPoly<8>>>;

    inline constexpr std::size_t kBatchCount = std::tuple_size_v<BatchTuple>;

    // ── Payload packing: type in high 4 bits (16 types), index in low 28 bits (~268M per type). ──
    inline constexpr std::uint32_t kTypeBits = 4;
    inline constexpr std::uint32_t kIndexBits = 32 - kTypeBits;
    inline constexpr std::uint32_t kIndexMask = (1u << kIndexBits) - 1u;
    static_assert(kBatchCount <= (1u << kTypeBits), "too many primitive types for the payload type field");

    [[nodiscard]] inline constexpr std::uint32_t pack(std::uint32_t type, std::uint32_t index) noexcept {
        return (type << kIndexBits) | (index & kIndexMask);
    }

    [[nodiscard]] inline constexpr std::uint32_t unpack_type(std::uint32_t payload) noexcept {
        return payload >> kIndexBits;
    }

    [[nodiscard]] inline constexpr std::uint32_t unpack_index(std::uint32_t payload) noexcept {
        return payload & kIndexMask;
    }

    template <class Prim, std::size_t I = 0>
    [[nodiscard]] consteval std::size_t type_index() {
        if constexpr (std::is_same_v < std::tuple_element_t < I, BatchTuple >, ShapeBatch<Prim> >) return I;
        else return type_index<Prim, I + 1>();
    }

    struct Handle {
        std::uint32_t payload{};
    };

    class Scene {
    public:
        using Tree = containers::AABBTree<Box2, Vec>;

        explicit Scene(Scalar margin = Scalar(0), unsigned threads = 0)
            : tree_(margin), exec_(threads) {}

        // Add a shape with an optional collision layer mask and static/dynamic flag
        template <class Prim>
        Handle add(const Prim& p, LayerMask layer = 1u, LayerMask mask = kAllLayers) {
            auto& b = batch<Prim>();
            const std::uint32_t idx = b.add(p);
            const std::uint32_t pl = pack(static_cast<std::uint32_t>(type_index<Prim>()), idx);
            const std::uint32_t leaf = tree_.insert(b.box(idx), pl);
            b.leaves[idx] = leaf;

            if (pl >= layers_.size()) {
                layers_.resize(pl + 1, 1u);
                masks_.resize(pl + 1, kAllLayers);
            }
            layers_[pl] = layer;
            masks_[pl] = mask;

            return Handle{pl};
        }

        [[nodiscard]] bool can_collide(std::uint32_t payload_a, std::uint32_t payload_b) const noexcept {
            if (payload_a >= layers_.size() || payload_b >= layers_.size()) return true;
            return (layers_[payload_a] & masks_[payload_b]) && (layers_[payload_b] & masks_[payload_a]);
        }

        template <class Prim>
        [[nodiscard]] ShapeBatch<Prim>& batch() noexcept { return std::get<ShapeBatch<Prim>>(batches_); }

        template <class Prim>
        [[nodiscard]] const ShapeBatch<Prim>& batch() const noexcept { return std::get<ShapeBatch<Prim>>(batches_); }

        [[nodiscard]] const Tree& tree() const noexcept { return tree_; }
        [[nodiscard]] Tree& tree() noexcept { return tree_; }
        [[nodiscard]] ParallelExecutor& executor() noexcept { return exec_; }

        [[nodiscard]] std::size_t count() const noexcept { return tree_.size(); }

        template <class Prim>
        [[nodiscard]] std::size_t count() const noexcept { return batch<Prim>().size(); }

        template <class Fn>
        void for_each_leaf(Fn&& fn) const {
            for_each_batch([&](const auto& b, std::size_t type) {
                const std::size_t n = b.size();
                for (std::size_t i = 0; i < n; ++i)
                    fn(pack(static_cast<std::uint32_t>(type), static_cast<std::uint32_t>(i)));
            });
        }

        template <class Visitor>
        void dispatch(std::uint32_t payload, Visitor&& v) const {
            const std::uint32_t type = unpack_type(payload);
            const std::uint32_t idx = unpack_index(payload);
            dispatch_impl(type, idx, std::forward<Visitor>(v),
                          std::make_index_sequence < kBatchCount >
            {});
        }

        void refit_all() {
            for_each_batch_mut([&](auto& b, std::size_t type) {
                b.refit();
                const std::size_t n = b.size();
                for (std::size_t i = 0; i < n; ++i) {
                    (void)tree_.update(b.leaves[i], b.box(static_cast<std::uint32_t>(i)));
                    (void)type;
                }
            });
        }

    private:
        template <class Fn, std::size_t... I>
        void for_each_batch_impl(Fn&& fn, std::index_sequence<I...>) const {
            (fn(std::get < I > (batches_), I), ...);
        }

        template <class Fn>
        void for_each_batch(Fn&& fn) const {
            for_each_batch_impl(std::forward<Fn>(fn), std::make_index_sequence < kBatchCount >
            {});
        }

        template <class Fn, std::size_t... I>
        void for_each_batch_mut_impl(Fn&& fn, std::index_sequence<I...>) {
            (fn(std::get < I > (batches_), I), ...);
        }

        template <class Fn>
        void for_each_batch_mut(Fn&& fn) {
            for_each_batch_mut_impl(std::forward<Fn>(fn), std::make_index_sequence < kBatchCount >
            {});
        }

        template <class Visitor, std::size_t... I>
        void dispatch_impl(std::uint32_t type, std::uint32_t idx, Visitor&& v,
                           std::index_sequence<I...>) const {
            ((type == I ? (void)v(std::get < I > (batches_), idx) : (void)0), ...);
        }

        BatchTuple batches_;
        Tree tree_;
        ParallelExecutor exec_;
        std::vector<LayerMask> layers_;
        std::vector<LayerMask> masks_;
    };
} // namespace akruti::scene
