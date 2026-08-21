// src/tests/containers/test_akruti_khanda.cpp — advanced fracture pipeline: Voronoi shatter, holes,
// mass properties, convex decomposition, impact-biased Poisson sites, recursive re-fracture.
#include "Catch2-3.9.0/extras/catch_amalgamated.hpp"
#include "akruti/akruti.hpp"
#include <cmath>
#include <span>
#include <vector>

using namespace akruti;
using namespace akruti::khanda;

namespace {
Poly unit_square() {
    Poly p;
    p.push_back({0, 0}); p.push_back({1, 0}); p.push_back({1, 1}); p.push_back({0, 1});
    return p;
}
} // namespace

TEST_CASE("akruti: khanda Voronoi shatter conserves area", "[akruti][khanda]") {
    Poly outer = unit_square();
    std::vector<Vec> sites = {{0.25f, 0.25f}, {0.75f, 0.3f}, {0.4f, 0.8f}, {0.8f, 0.75f}, {0.5f, 0.5f}};
    auto shards = fracture_voronoi(outer, std::span<const Vec>(sites));

    double sum = 0;
    for (const auto& s : shards) sum += s.area;
    REQUIRE(sum == Catch::Approx(1.0).margin(1e-3));
    REQUIRE(!shards.empty());
    REQUIRE(shards.size() <= sites.size());
}

TEST_CASE("akruti: khanda shards are convex-decomposed", "[akruti][khanda]") {
    Poly outer = unit_square();
    std::vector<Vec> sites = {{0.25f, 0.25f}, {0.75f, 0.3f}, {0.4f, 0.8f}, {0.8f, 0.75f}};
    auto shards = fracture_voronoi(outer, std::span<const Vec>(sites));
    for (const auto& s : shards)
        for (const auto& pc : s.convex)
            REQUIRE(is_convex_ccw(pc, 1e-4f));
}

TEST_CASE("akruti: khanda triangulation area matches shard", "[akruti][khanda]") {
    Poly outer = unit_square();
    std::vector<Vec> sites = {{0.3f, 0.3f}, {0.7f, 0.7f}, {0.5f, 0.2f}};
    auto shards = fracture_voronoi(outer, std::span<const Vec>(sites));
    for (const auto& s : shards) {
        double ta = 0;
        for (std::size_t i = 0; i + 2 < s.mesh.indices.size(); i += 3) {
            Vec a = s.mesh.vertices[s.mesh.indices[i]];
            Vec b = s.mesh.vertices[s.mesh.indices[i + 1]];
            Vec c = s.mesh.vertices[s.mesh.indices[i + 2]];
            ta += 0.5 * double(khanda::detail::orient(a, b, c));
        }
        REQUIRE(ta == Catch::Approx(s.area).margin(1e-3));
    }
}

TEST_CASE("akruti: khanda respects holes", "[akruti][khanda]") {
    Poly outer = unit_square();
    Poly hole; // CW inner square [0.4,0.6]^2, area 0.04
    hole.push_back({0.4f, 0.4f}); hole.push_back({0.4f, 0.6f});
    hole.push_back({0.6f, 0.6f}); hole.push_back({0.6f, 0.4f});
    std::vector<Poly> holes = {hole};
    std::vector<Vec> sites = {{0.2f, 0.2f}, {0.8f, 0.2f}, {0.2f, 0.8f}, {0.8f, 0.8f}, {0.5f, 0.5f}};
    auto shards = fracture_voronoi(outer, std::span<const Poly>(holes), std::span<const Vec>(sites));

    double sum = 0;
    for (const auto& s : shards) sum += s.area;
    REQUIRE(sum == Catch::Approx(1.0 - 0.04).margin(3e-3));
}

TEST_CASE("akruti: khanda polar moment of inertia", "[akruti][khanda]") {
    // Unit square centered at origin: analytic polar J = 1/6 (unit density).
    Poly sq;
    sq.push_back({-0.5f, -0.5f}); sq.push_back({0.5f, -0.5f});
    sq.push_back({0.5f, 0.5f}); sq.push_back({-0.5f, 0.5f});
    EarClipTriangulator tri;
    Triangulation t = tri(sq, std::span<const Poly>{});
    MassProps mp = shard_mass_props(t);
    REQUIRE(mp.area == Catch::Approx(1.0).margin(1e-4));
    REQUIRE(mp.inertia == Catch::Approx(1.0 / 6.0).margin(1e-3));
}

TEST_CASE("akruti: khanda impact-biased Poisson densifies near impact", "[akruti][khanda]") {
    AABB<Scalar> b{{0, 0}, {4, 4}};
    Vec center{2, 2};
    Scalar r = 0.8f;
    PoissonConfig pc; pc.min_dist = 0.25f; pc.seed = 42;
    auto uniform = poisson_disk_sites(b, pc, nullptr);
    ImpactField field{center, 4.0f, 0.8f};
    auto biased = poisson_disk_sites(b, pc, &field);
    auto near = [&](const std::vector<Vec>& v) {
        int n = 0; for (auto p : v) if ((p - center).len2() <= r * r) ++n; return n;
    };
    REQUIRE(near(biased) > near(uniform));
}

TEST_CASE("akruti: khanda recursive re-fracture conserves area", "[akruti][khanda]") {
    Poly outer = unit_square();
    std::vector<Vec> sites = {{0.3f, 0.3f}, {0.7f, 0.7f}, {0.3f, 0.7f}};
    auto shards = fracture_voronoi(outer, std::span<const Vec>(sites));
    REQUIRE(!shards.empty());

    const Shard& parent = shards.front();
    AABB<Scalar> pb = khanda::detail::bounds_of(parent.outline);
    Vec c = pb.center();
    std::vector<Vec> subs = {{c.x - 0.05f, c.y}, {c.x + 0.05f, c.y}, {c.x, c.y + 0.05f}};
    auto subshards = refracture(parent, std::span<const Vec>(subs));
    double sum = 0;
    for (const auto& s : subshards) sum += s.area;
    REQUIRE(sum == Catch::Approx(parent.area).margin(1e-3));
}
