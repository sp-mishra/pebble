#include "catch_amalgamated.hpp"
#include "containers/spatial/AABBTree.hpp"
#include "containers/associative/SparseSet.hpp"
#include "containers/associative/slot_map.hpp"
#include "containers/tree/NAryTree.hpp"
#include <algorithm>
#include <cstdint>

namespace {
    struct Box2D {
        struct { float x, y; } lo, hi;
        auto area() const noexcept { return (hi.x - lo.x) * (hi.y - lo.y); }
        Box2D fattened(float m) const noexcept { return {{lo.x - m, lo.y - m}, {hi.x + m, hi.y + m}}; }
        static Box2D merge(const Box2D& a, const Box2D& b) noexcept {
            return {{std::min(a.lo.x, b.lo.x), std::min(a.lo.y, b.lo.y)},
                    {std::max(a.hi.x, b.hi.x), std::max(a.hi.y, b.hi.y)}};
        }
        bool overlaps(const Box2D& o) const noexcept {
            return !(hi.x < o.lo.x || lo.x > o.lo.x || hi.y < o.lo.y || lo.y > o.hi.y);
        }
    };
}

TEST_CASE("Unified AABBTree: Binary and Quad Policy", "[aabb][unified]") {
    pebble::containers::AABBTree<Box2D> tree;
    for (uint32_t i = 0; i < 10'000; ++i) {
        float f = static_cast<float>(i);
        tree.insert(Box2D{{f, f}, {f + 1.0f, f + 1.0f}}, i);
    }
    REQUIRE(tree.size() == 10'000);

    size_t hits = 0;
    tree.query(Box2D{{50.0f, 50.0f}, {60.0f, 60.0f}}, [&](uint32_t) { ++hits; });
    REQUIRE(hits > 0);

    pebble::containers::AABBTree<Box2D, decltype(Box2D{}.lo), std::uint32_t, std::allocator<std::byte>, pebble::containers::aabb::QuadBranchingPolicy<Box2D, decltype(Box2D{}.lo), std::uint32_t, std::allocator<std::byte>>> quad_tree;
    for (uint32_t i = 0; i < 1'000; ++i) {
        float f = static_cast<float>(i);
        quad_tree.insert(Box2D{{f, f}, {f + 1.0f, f + 1.0f}}, i);
    }
    REQUIRE(quad_tree.size() == 1'000);
}

TEST_CASE("Unified SparseSet: Flat and Paged Policy Equivalence", "[sparse][unified]") {
    pebble::containers::FlatSparseSet<uint32_t, uint64_t> flat_set;
    pebble::containers::PagedSparseSet<uint32_t, uint64_t> paged_set;

    flat_set.insert_or_update(100, 1000ULL);
    paged_set.insert_or_update(100, 1000ULL);

    REQUIRE(flat_set.contains(100));
    REQUIRE(paged_set.contains(100));
    REQUIRE(flat_set.get(100)->get() == 1000ULL);
    REQUIRE(paged_set.get(100)->get() == 1000ULL);
}

TEST_CASE("Unified slot_map: Automatic Dense Optimization", "[slotmap][unified]") {
    pebble::containers::slot_map<uint64_t> sm;
    auto h1 = sm.insert(10ULL);
    auto h2 = sm.insert(20ULL);

    REQUIRE(sm.contains(h1));
    REQUIRE(*sm.find(h1) == 10ULL);

    REQUIRE(sm.erase(h1));
    REQUIRE(!sm.contains(h1));
    REQUIRE(*sm.find(h2) == 20ULL);
}

#include "containers/descriptor_registry.hpp"

TEST_CASE("descriptor_registry: Handle immutability on duplicate stable_id overwrite", "[descriptor][regression]") {
    struct MockDesc {
        std::uint32_t stable_id;
        std::uint64_t name_hash;
        std::uint32_t category;
        std::string name;
    };

    containers::descriptor_registry<MockDesc> reg;
    auto h1 = reg.register_desc(MockDesc{1, 100, 0, "alpha"});
    auto h2 = reg.register_desc(MockDesc{1, 200, 0, "beta"});

    REQUIRE(h1 == h2);
    REQUIRE(reg.size() == 1);
    REQUIRE(reg.find(1)->name == "beta");
}

TEST_CASE("AABBTree: 4-Way Raycast Path Precision", "[aabb][raycast][simd]") {
    using QuadTree = pebble::containers::AABBTree<
        Box2D, decltype(Box2D{}.lo), std::uint32_t, std::allocator<std::byte>,
        pebble::containers::aabb::QuadBranchingPolicy<Box2D, decltype(Box2D{}.lo), std::uint32_t, std::allocator<std::byte>>
    >;
    QuadTree tree;
    tree.insert(Box2D{{40.0f, 40.0f}, {60.0f, 60.0f}}, 1);
    tree.insert(Box2D{{140.0f, 140.0f}, {160.0f, 160.0f}}, 2);

    size_t hits = 0;
    tree.raycast(decltype(Box2D{}.lo){0.0f, 0.0f}, decltype(Box2D{}.lo){1.0f, 1.0f}, 1000.0f, [&](uint32_t) {
        ++hits;
    });
    REQUIRE(hits >= 1);
}

TEST_CASE("AABBTree: SIMD QuadBranching vs BinaryBranching raycast parity", "[aabb][simd][parity]") {
    struct Box {
        struct { float x, y; } lo, hi;
        auto area() const noexcept { return (hi.x - lo.x) * (hi.y - lo.y); }
        Box fattened(float m) const noexcept { return {{lo.x - m, lo.y - m}, {hi.x + m, hi.y + m}}; }
        static Box merge(const Box& a, const Box& b) noexcept {
            return {{std::min(a.lo.x, b.lo.x), std::min(a.lo.y, b.lo.y)},
                    {std::max(a.hi.x, b.hi.x), std::max(a.hi.y, b.hi.y)}};
        }
        bool overlaps(const Box& o) const noexcept {
            return !(hi.x < o.lo.x || lo.x > o.lo.x || hi.y < o.lo.y || lo.y > o.hi.y);
        }
    };

    pebble::containers::AABBTree<Box, decltype(Box{}.lo), uint32_t, std::allocator<std::byte>,
        pebble::containers::aabb::BinaryBranchingPolicy<Box, decltype(Box{}.lo), uint32_t, std::allocator<std::byte>>> binary_tree;
    pebble::containers::AABBTree<Box, decltype(Box{}.lo), uint32_t, std::allocator<std::byte>,
        pebble::containers::aabb::QuadBranchingPolicy<Box, decltype(Box{}.lo), uint32_t, std::allocator<std::byte>>> quad_tree;

    for (uint32_t i = 0; i < 10'000; ++i) {
        float f = static_cast<float>(i * 2);
        Box b{{f, f}, {f + 1.0f, f + 1.0f}};
        binary_tree.insert(b, i);
        quad_tree.insert(b, i);
    }

    std::vector<uint32_t> bin_hits, quad_hits;
    binary_tree.raycast({0.0f, 0.0f}, {1.0f, 1.0f}, 500.0f, [&](uint32_t p) { bin_hits.push_back(p); });
    quad_tree.raycast({0.0f, 0.0f}, {1.0f, 1.0f}, 500.0f, [&](uint32_t p) { quad_hits.push_back(p); });

    std::sort(bin_hits.begin(), bin_hits.end());
    std::sort(quad_hits.begin(), quad_hits.end());
    REQUIRE(bin_hits == quad_hits);
}


