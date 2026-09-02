#pragma once
// ============================================================================
// gati/collision.hpp — Akruti Geometry Bridge with Incremental Fat Margin Refit
// ============================================================================
// Uses containers::AABBTree broadphase with fat margin caching and Akruti GJK/EPA.
// ============================================================================

#include "math.hpp"
#include "ecs.hpp"
#include "transform.hpp"
#include "system.hpp"
#include "event.hpp"
#include "containers/cache/kosha.hpp"
#include "containers/tensor/tensor.hpp"
#include "containers/dynamic/SmallVector.hpp"
#include "mem/arena.hpp"

#if defined(GATI_ENABLE_AKRUTI) && __has_include("akruti/akruti.hpp")
#define GATI_HAS_AKRUTI 1
#include "akruti/akruti.hpp"
#include "containers/tree/AABBTree.hpp"

#include <variant>
#include <vector>
#include <memory>

namespace gati {
    using ShapeVariant = std::variant<
        akruti::Circle,
        akruti::Box,
        akruti::Capsule,
        akruti::OrientedBox,
        akruti::Triangle,
        akruti::RoundedBox,
        akruti::Sector,
        akruti::ConvexPoly < 8>
    ,
    akruti::ChainShape<16>
    ,
    akruti::GridSDF<16, 16>
    >;

    // Component: an Akruti primitive placed by the entity Transform
    struct ShapeRef {
        ShapeVariant shape;
        AABB cached_fat_aabb{};
        bool tree_inserted = false;
    };

    namespace detail {
        inline void translate(akruti::Circle& c, const Vec2& p) {
            const akruti::Vec ap(p);
            c.center = c.center + ap;
        }

        inline void translate(akruti::Box& b, const Vec2& p) {
            const akruti::Vec ap(p);
            b.center = b.center + ap;
        }

        inline void translate(akruti::Capsule& c, const Vec2& p) {
            const akruti::Vec ap(p);
            c.a = c.a + ap;
            c.b = c.b + ap;
        }

        inline void translate(akruti::OrientedBox& o, const Vec2& p) {
            const akruti::Vec ap(p);
            o.center = o.center + ap;
        }

        inline void translate(akruti::Triangle& t, const Vec2& p) {
            const akruti::Vec ap(p);
            t.a = t.a + ap;
            t.b = t.b + ap;
            t.c = t.c + ap;
        }

        inline void translate(akruti::RoundedBox& r, const Vec2& p) {
            const akruti::Vec ap(p);
            r.center = r.center + ap;
        }

        inline void translate(akruti::Sector& s, const Vec2& p) {
            const akruti::Vec ap(p);
            s.center = s.center + ap;
        }

        template <std::size_t N>
        inline void translate(akruti::ConvexPoly<N>& poly, const Vec2& p) {
            const akruti::Vec ap(p);
            for (auto& v : poly.verts) {
                v = v + ap;
            }
        }

        template <std::size_t N>
        inline void translate(akruti::ChainShape<N>& chain, const Vec2& p) {
            const akruti::Vec ap(p);
            for (auto& v : chain.verts) {
                v = v + ap;
            }
            if (chain.has_prev_ghost) chain.prev_ghost = chain.prev_ghost + ap;
            if (chain.has_next_ghost) chain.next_ghost = chain.next_ghost + ap;
        }

        template <std::size_t W, std::size_t H>
        inline void translate(akruti::GridSDF<W, H>& grid, const Vec2& p) {
            grid.bounds = AABB(grid.bounds.lo + p, grid.bounds.hi + p);
        }
    } // namespace detail

    // World AABB of a shape placed at p
    inline AABB shape_aabb(const ShapeVariant& v, const Vec2& p) {
        return std::visit([&](auto s) -> AABB {
            detail::translate(s, p);
            auto akruti_box = s.aabb();
            return AABB(akruti_box.lo, akruti_box.hi);
        }, v);
    }

// Platform-adaptive default policy detection
#if defined(__APPLE__) && defined(__aarch64__) && defined(PEBBLE_ENABLE_MLX)
#include "containers/tensor/mlx_storage_policy.hpp"
#include "containers/tensor/mlx_computation_policy.hpp"
namespace detail {
    using DefaultGatiStoragePolicy = ts::mlx_storage_policy;
    using DefaultGatiCompPolicy = ts::mlx_computation_policy;
}
#elif defined(PEBBLE_ENABLE_HIGHWAY)
#include "containers/tensor/highway_computation_policy.hpp"
namespace detail {
    using DefaultGatiStoragePolicy = ts::DefaultStoragePolicy;
    using DefaultGatiCompPolicy = ts::highway_computation_policy;
}
#else
namespace detail {
    using DefaultGatiStoragePolicy = ts::DefaultStoragePolicy;
    using DefaultGatiCompPolicy = ts::DefaultComputationPolicy;
}
#endif

// Tensor-accelerated broadphase state for large entity counts (zero-allocation bump/arena friendly)
template <typename StoragePolicy = detail::DefaultGatiStoragePolicy,
          typename CompPolicy = detail::DefaultGatiCompPolicy>
struct BasicTensorBroadphase {
    using TensorType = ts::DynamicTensor<float, StoragePolicy, CompPolicy>;
    TensorType state; // [N, 4] -> [x, y, vx, vy]
    TensorType radii; // [N]    -> radius
    containers::dynamic::SmallVector<std::uint32_t, 1024> entity_indices;

    static constexpr float kCellSize = 18.0f;
    containers::dynamic::SmallVector<int, 4096> head;
    containers::dynamic::SmallVector<int, 1024> next;

    void update(World& w) {
        std::size_t count = 0;
        w.view<ShapeRef, Transform>([&](Entity, ShapeRef&, Transform&) {
            ++count;
        });

        if (count == 0) return;
        if (state.shape().empty() || state.shape()[0] != count) {
            state = TensorType({count, 4});
            radii = TensorType({count});
            entity_indices.resize(count);
        }

        float* s_ptr = state.data();
        float* r_ptr = radii.data();
        std::size_t idx = 0;

        w.view<ShapeRef, Transform>([&](Entity e, ShapeRef& s, Transform& tr) {
            s_ptr[idx * 4 + 0] = tr.position[0];
            s_ptr[idx * 4 + 1] = tr.position[1];
            s_ptr[idx * 4 + 2] = 0.0f;
            s_ptr[idx * 4 + 3] = 0.0f;
            auto box = shape_aabb(s.shape, tr.position);
            r_ptr[idx] = std::max(box.hi[0] - box.lo[0], box.hi[1] - box.lo[1]) * 0.5f;
            entity_indices[idx] = e.index;
            ++idx;
        });
    }
};

// Broadphase over transformed AABBs / Tensor spatial bins + GJK/EPA narrowphase; emits ContactEvents.
template <typename StoragePolicy = detail::DefaultGatiStoragePolicy,
          typename CompPolicy = detail::DefaultGatiCompPolicy>
struct BasicCollisionSystem {
    using TensorBroadphaseType = BasicTensorBroadphase<StoragePolicy, CompPolicy>;

    containers::AABBTree<AABB> tree{Scalar(0.1)}; // fat margin reduces churn
    Scalar fat_margin = Scalar(0.2);
    kosha::LRUCache<std::uint64_t, akruti::SimplexCache> simplex_caches_{8192};
    TensorBroadphaseType tensor_broadphase_{};

    void run(World& w, StepContext ctx) {
        // Automatic platform & workload threshold: for > 200 entities, leverage TensorBroadphase
        std::size_t entity_count = 0;
        w.view<ShapeRef, Transform>([&](Entity, ShapeRef&, Transform&) { ++entity_count; });

        if (entity_count > 200) {
            tensor_broadphase_.update(w);
            const float* s_ptr = tensor_broadphase_.state.data();
            const float* r_ptr = tensor_broadphase_.radii.data();
            const auto& indices = tensor_broadphase_.entity_indices;
            const std::size_t N = indices.size();

            // Dynamic grid dimensions
            constexpr float cs = TensorBroadphaseType::kCellSize;
            int g_cols = 80, g_rows = 60;
            int total_cells = g_cols * g_rows;
            if (tensor_broadphase_.head.size() != static_cast<std::size_t>(total_cells)) {
                tensor_broadphase_.head.resize(total_cells, -1);
            }
            std::fill(tensor_broadphase_.head.begin(), tensor_broadphase_.head.end(), -1);
            tensor_broadphase_.next.resize(N, -1);

            for (std::size_t i = 0; i < N; ++i) {
                int cx = std::clamp(static_cast<int>(s_ptr[i * 4 + 0] / cs), 0, g_cols - 1);
                int cy = std::clamp(static_cast<int>(s_ptr[i * 4 + 1] / cs), 0, g_rows - 1);
                int cell = cy * g_cols + cx;
                tensor_broadphase_.next[i] = tensor_broadphase_.head[cell];
                tensor_broadphase_.head[cell] = static_cast<int>(i);
            }

            // 4-Color Checkerboard Domain Decomposition for lock-free Pravaha task parallelism
            // Color formula: (cx % 2) + 2 * (cy % 2) in [0, 3]
            // Cells of the same color never share boundaries or neighbor links!
            containers::dynamic::SmallVector<std::uint32_t, 1024> color_buckets[4];
            for (std::size_t i = 0; i < N; ++i) {
                int cx = std::clamp(static_cast<int>(s_ptr[i * 4 + 0] / cs), 0, g_cols - 1);
                int cy = std::clamp(static_cast<int>(s_ptr[i * 4 + 1] / cs), 0, g_rows - 1);
                int color = (cx & 1) | ((cy & 1) << 1);
                color_buckets[color].push_back(static_cast<std::uint32_t>(i));
            }

            // Execute 4 disjoint non-interfering parallel passes across all CPU cores
            for (int color = 0; color < 4; ++color) {
                const auto& bucket = color_buckets[color];
                if (bucket.empty()) continue;

                ctx.executor.for_range(bucket.size(), [&](std::size_t item_idx) {
                    std::size_t i = bucket[item_idx];
                    const float ra = r_ptr[i];
                    const float ax = s_ptr[i * 4 + 0];
                    const float ay = s_ptr[i * 4 + 1];
                    std::uint32_t ea_idx = indices[i];
                    ShapeRef* sa = w.get_by_index<ShapeRef>(ea_idx);
                    Transform* ta = w.get_by_index<Transform>(ea_idx);
                    if (!sa || !ta) return;

                    int cx = std::clamp(static_cast<int>(ax / cs), 0, g_cols - 1);
                    int cy = std::clamp(static_cast<int>(ay / cs), 0, g_rows - 1);

                    for (int dy = -1; dy <= 1; ++dy) {
                        int ny = cy + dy;
                        if (ny < 0 || ny >= g_rows) continue;
                        for (int dx = -1; dx <= 1; ++dx) {
                            int nx = cx + dx;
                            if (nx < 0 || nx >= g_cols) continue;

                            int cell = ny * g_cols + nx;
                            for (int j = tensor_broadphase_.head[cell]; j != -1; j = tensor_broadphase_.next[j]) {
                                if (static_cast<std::size_t>(j) <= i) continue;

                                const float rb = r_ptr[j];
                                const float bx = s_ptr[j * 4 + 0];
                                const float by = s_ptr[j * 4 + 1];
                                float dist_sq = (bx - ax) * (bx - ax) + (by - ay) * (by - ay);
                                float min_d = ra + rb;
                                if (dist_sq >= min_d * min_d) continue;

                                std::uint32_t eb_idx = indices[j];
                                ShapeRef* sb = w.get_by_index<ShapeRef>(eb_idx);
                                Transform* tb = w.get_by_index<Transform>(eb_idx);
                                if (!sb || !tb) continue;

                                const std::uint64_t key = (static_cast<std::uint64_t>(ea_idx) << 32) | static_cast<
                                    std::uint64_t>(eb_idx);
                                akruti::SimplexCache* cache = simplex_caches_.get_ref(key);
                                if (!cache) {
                                    (void)simplex_caches_.put(key, akruti::SimplexCache{});
                                    cache = simplex_caches_.get_ref(key);
                                }
                                narrow(ctx, ea_idx, eb_idx, sa->shape, ta->position, sb->shape, tb->position,
                                       cache ? *cache : dummy_cache);
                            }
                        }
                    }
                }, 64);
            }
            return;
        }

        // Standard AABBTree broadphase for low/medium entity counts
        tree = containers::AABBTree<AABB>{fat_margin};

        w.view<ShapeRef, Transform>([&](Entity e, ShapeRef& s, Transform& tr) {
            auto current_box = shape_aabb(s.shape, tr.position);
            s.cached_fat_aabb = current_box.fattened(fat_margin);
            tree.insert(s.cached_fat_aabb, e.index);
        });

        w.view<ShapeRef, Transform>([&](Entity ea, ShapeRef& sa, Transform& ta) {
            const auto qbox = shape_aabb(sa.shape, ta.position);
            tree.query(qbox, [&](std::uint32_t other) {
                if (other <= ea.index) return; // a < b dedup + skip self
                ShapeRef* sb = w.get_by_index<ShapeRef>(other);
                Transform* tb = w.get_by_index<Transform>(other);
                if (!sb || !tb) return;

                const std::uint64_t key = (static_cast<std::uint64_t>(ea.index) << 32) | static_cast<std::uint64_t>(
                    other);
                akruti::SimplexCache* cache = simplex_caches_.get_ref(key);
                if (!cache) {
                    (void)simplex_caches_.put(key, akruti::SimplexCache{});
                    cache = simplex_caches_.get_ref(key);
                }
                narrow(ctx, ea.index, other, sa.shape, ta.position, sb->shape, tb->position,
                       cache ? *cache : dummy_cache);
            });
        });
    }

private:
    akruti::SimplexCache dummy_cache{};

private:
    static void narrow(StepContext& ctx, std::uint32_t a, std::uint32_t b,
                       const ShapeVariant& va, const Vec2& pa,
                       const ShapeVariant& vb, const Vec2& pb,
                       akruti::SimplexCache& cache) {
        std::visit([&](auto sa) {
            std::visit([&](auto sb) {
                detail::translate(sa, pa);
                detail::translate(sb, pb);
                akruti::Manifold m = akruti::collide_gjk_warm_started(sa, sb, &cache);
                if (m.hit) {
                    const Vec2 cp = m.points.empty()
                                        ? Vec2{0.0f, 0.0f}
                                        : Vec2{m.points[0].point.x, m.points[0].point.y};
                    ctx.events.publish(ContactEvent{a, b, {m.normal.x, m.normal.y}, m.depth, cp});
                }
            }, vb);
        }, va);
    }
};

using CollisionSystem = BasicCollisionSystem<>;

// Broadphase-accelerated raycast: nearest entity hit + Akruti RayHit
struct RaycastResult {
    Entity entity = null_entity;
    akruti::RayHit hit;
};

[[nodiscard]] inline RaycastResult raycast(World& w, CollisionSystem& cs,
                                           const Vec2& origin, const Vec2& dir,
                                           Scalar max_t = Scalar(1e4)) {
    RaycastResult best;
    best.hit.t = max_t;
    cs.tree.raycast(origin, dir, max_t, [&](std::uint32_t idx) {
        ShapeRef* s = w.get_by_index<ShapeRef>(idx);
        Transform* t = w.get_by_index<Transform>(idx);
        if (!s || !t) return;
        std::visit([&](auto prim) {
            detail::translate(prim, t->position);
            const akruti::RayHit h = akruti::raycast(prim, origin, dir, max_t);
            if (h.hit && h.t < best.hit.t) {
                best.hit = h;
                best.entity = Entity{idx, w.generation_of(idx)};
            }
        }, s->shape);
    });
    return best;
}

// Continuous Collision Detection (CCD) Sweep Query between moving entity and static world
struct SweepResult {
    Entity entity = null_entity;
    akruti::TOIResult toi{};
};

[[nodiscard]] inline SweepResult sweep_test(World& w, CollisionSystem& cs,
                                            const ShapeVariant& moving_shape,
                                            const Vec2& start_pos, const Vec2& delta_pos) {
    SweepResult best;
    best.toi.t = Scalar(1.0f);

    // Compute broadphase swept AABB
    AABB box_start = shape_aabb(moving_shape, start_pos);
    AABB box_end = shape_aabb(moving_shape, start_pos + delta_pos);
    AABB swept_box{
        {std::min(box_start.lo[0], box_end.lo[0]), std::min(box_start.lo[1], box_end.lo[1])},
        {std::max(box_start.hi[0], box_end.hi[0]), std::max(box_start.hi[1], box_end.hi[1])}
    };

    cs.tree.query(swept_box, [&](std::uint32_t other_idx) {
        ShapeRef* other_s = w.get_by_index<ShapeRef>(other_idx);
        Transform* other_t = w.get_by_index<Transform>(other_idx);
        if (!other_s || !other_t) return;

        std::visit([&](auto static_prim) {
            std::visit([&](auto mover_prim) {
                detail::translate(static_prim, other_t->position);
                detail::translate(mover_prim, start_pos);

                auto res = akruti::time_of_impact(static_prim, mover_prim, delta_pos);
                if (res.hit && res.t < best.toi.t) {
                    best.toi = res;
                    best.entity = Entity{other_idx, w.generation_of(other_idx)};
                }
            }, moving_shape);
        }, other_s->shape);
    });

    return best;
}

} // namespace gati
#endif // GATI_HAS_AKRUTI

