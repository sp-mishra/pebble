#pragma once
// akruti/shape_store.hpp — Type-Erased Shape Container with Direct Matrix Dispatch & SBO.
// Satisfies akruti::Shape concept. No virtual functions, no dynamic heap allocation.
#include "shape.hpp"
#include "primitives.hpp"
#include "narrowphase.hpp"
#include <new>
#include <type_traits>
#include <cstddef>
#include <utility>

namespace akruti {

struct ShapeStore {
    ShapeType type{ShapeType::Circle};
    alignas(64) std::byte storage[128]{};

    // Type-erased function pointers (vtable-free, static table cached)
    Scalar (*sdf_fn)(const void*, Vec) noexcept = nullptr;
    AABB<Scalar> (*aabb_fn)(const void*) noexcept = nullptr;
    Vec (*support_fn)(const void*, Vec) noexcept = nullptr;
    Vec (*centroid_fn)(const void*) noexcept = nullptr;

    constexpr ShapeStore() noexcept {
        set(Circle{.center = {0, 0}, .radius = 0.5f});
    }

    template <Shape S>
    ShapeStore(const S& shape) noexcept {
        set(shape);
    }

    template <Shape S>
    void set(const S& shape) noexcept {
        static_assert(sizeof(S) <= sizeof(storage), "Shape exceeds ShapeStore SBO size of 128 bytes");
        static_assert(alignof(S) <= alignof(std::max_align_t), "Shape alignment requirement too large");

        if constexpr (std::is_same_v<S, Circle>) {
            type = ShapeType::Circle;
        } else if constexpr (std::is_same_v<S, Box>) {
            type = ShapeType::Box;
        } else if constexpr (std::is_same_v<S, Capsule>) {
            type = ShapeType::Capsule;
        } else if constexpr (std::is_same_v<S, OrientedBox>) {
            type = ShapeType::OrientedBox;
        } else if constexpr (std::is_same_v<S, Triangle>) {
            type = ShapeType::Triangle;
        } else if constexpr (std::is_same_v<S, RoundedBox>) {
            type = ShapeType::RoundedBox;
        } else if constexpr (std::is_same_v<S, Sector>) {
            type = ShapeType::Sector;
        } else if constexpr (std::is_same_v<S, Segment>) {
            type = ShapeType::Segment;
        } else if constexpr (std::is_same_v<S, ConvexPoly<8>>) {
            type = ShapeType::ConvexPoly;
        } else if constexpr (std::is_same_v<S, RoundedPoly<8>>) {
            type = ShapeType::RoundedPoly;
        } else {
            type = ShapeType::Circle; // Fallback
        }

        ::new (static_cast<void*>(storage)) S(shape);

        sdf_fn = [](const void* ptr, Vec p) noexcept -> Scalar {
            return static_cast<const S*>(ptr)->sdf(p);
        };
        aabb_fn = [](const void* ptr) noexcept -> AABB<Scalar> {
            return static_cast<const S*>(ptr)->aabb();
        };
        support_fn = [](const void* ptr, Vec d) noexcept -> Vec {
            return static_cast<const S*>(ptr)->support(d);
        };
        centroid_fn = [](const void* ptr) noexcept -> Vec {
            return static_cast<const S*>(ptr)->centroid();
        };
    }

    [[nodiscard]] Scalar sdf(Vec p) const noexcept {
        return sdf_fn ? sdf_fn(storage, p) : Scalar(1e9);
    }

    [[nodiscard]] AABB<Scalar> aabb() const noexcept {
        return aabb_fn ? aabb_fn(storage) : AABB<Scalar>{};
    }

    [[nodiscard]] Vec support(Vec d) const noexcept {
        return support_fn ? support_fn(storage, d) : Vec{};
    }

    [[nodiscard]] Vec centroid() const noexcept {
        return centroid_fn ? centroid_fn(storage) : Vec{};
    }

    template <class S>
    [[nodiscard]] const S& as() const noexcept {
        return *reinterpret_cast<const S*>(storage);
    }

    template <class S>
    [[nodiscard]] S& as() noexcept {
        return *reinterpret_cast<S*>(storage);
    }

    [[nodiscard]] const void* data() const noexcept {
        return storage;
    }

    [[nodiscard]] void* data() noexcept {
        return storage;
    }
};

static_assert(Shape<ShapeStore>, "ShapeStore must satisfy Shape concept");

} // namespace akruti
