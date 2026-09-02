#include "catch_amalgamated.hpp"
#include "ecs/ecs.hpp"
#include "containers/numeric/math_vector.hpp"

namespace {
    struct Pos {
        float x = 0.0f;
        float y = 0.0f;
    };

    struct Vel {
        float dx = 0.0f;
        float dy = 0.0f;
    };

    struct Hp {
        int points = 100;
    };

    struct Flag {};

    struct PhysicsSystem {
        using reads = pebble::ecs::Reads<Vel>;
        using writes = pebble::ecs::Writes<Pos>;

        int runs = 0;

        void run(pebble::ecs::World& w, float dt) {
            w.view<Pos, Vel>([&](pebble::ecs::Entity, Pos& p, const Vel& v) {
                p.x += v.dx * dt;
                p.y += v.dy * dt;
            });
            ++runs;
        }
    };

    struct RenderSystem {
        using reads = pebble::ecs::Reads<Pos>;
        using writes = pebble::ecs::Writes<>;

        int runs = 0;

        void run(pebble::ecs::World&, float) {
            ++runs;
        }
    };
} // namespace

TEST_CASE (
"ECS Improvements: Static Assertions for Zero Virtuals"
,
"[ecs][zero_virtual]"
)
 {
    STATIC_REQUIRE(!std::is_polymorphic_v<pebble::ecs::Entity>);
    STATIC_REQUIRE(!std::is_polymorphic_v<pebble::ecs::ErasedStore>);
    STATIC_REQUIRE(!std::is_polymorphic_v<pebble::ecs::ComponentStore<Pos>>);
    STATIC_REQUIRE(!std::is_polymorphic_v<pebble::ecs::CommandBuffer>);
    STATIC_REQUIRE(!std::is_polymorphic_v<pebble::ecs::LocalCommandBuffer>);
    STATIC_REQUIRE(!std::is_polymorphic_v<pebble::ecs::ObserverRegistry>);
    STATIC_REQUIRE(!std::is_polymorphic_v<pebble::ecs::RelationStore<pebble::ecs::ChildOf>>);
    STATIC_REQUIRE(!std::is_polymorphic_v<pebble::ecs::Scheduler<pebble::ecs::World>>);
    STATIC_REQUIRE(!std::is_polymorphic_v<pebble::ecs::ArchetypeStorage>);
    STATIC_REQUIRE(!std::is_polymorphic_v<pebble::ecs::PagedSparse<std::uint32_t>>);
    STATIC_REQUIRE(!std::is_polymorphic_v<pebble::ecs::World>);
}

TEST_CASE (
"ECS Improvements: Paged Sparse On-Demand Allocation"
,
"[ecs][paged_sparse]"
)
 {
    pebble::ecs::PagedSparse<std::uint32_t, 512> sparse;
    REQUIRE(sparse.allocated_pages() == 0);
    REQUIRE_FALSE(sparse.has(100));

    sparse[100] = 5;
    REQUIRE(sparse.has(100));
    REQUIRE(sparse[100] == 5);
    REQUIRE(sparse.allocated_pages() == 1);

    // Access far away index creates second page
    sparse[2000] = 42;
    REQUIRE(sparse.has(2000));
    REQUIRE(sparse[2000] == 42);
    REQUIRE(sparse.allocated_pages() == 2);

    sparse.erase(100);
    REQUIRE_FALSE(sparse.has(100));
    REQUIRE(sparse.has(2000));

    sparse.clear();
    REQUIRE_FALSE(sparse.has(2000));
}

TEST_CASE (
"ECS Improvements: Reactive Observers"
,
"[ecs][observer]"
)
 {
    pebble::ecs::World world;

    int adds = 0;
    int removes = 0;
    pebble::ecs::Entity added_entity{};

    auto add_hook = [&](pebble::ecs::OnAdd<Hp> ev) {
        ++adds;
        added_entity = ev.entity;
        REQUIRE(ev.component.points == 100);
    };

    auto remove_hook = [&](pebble::ecs::OnRemove<Hp> ev) {
        ++removes;
        REQUIRE(ev.entity == added_entity);
    };

    world.on_add<Hp>(add_hook);
    world.on_remove<Hp>(remove_hook);

    auto e1 = world.spawn();
    world.add(e1, Hp{100});

    REQUIRE(adds == 1);
    REQUIRE(added_entity == e1);
    REQUIRE(removes == 0);

    world.remove<Hp>(e1);
    REQUIRE(removes == 1);

    // Despawn should also trigger remove observer if component was attached
    world.add(e1, Hp{100});
    REQUIRE(adds == 2);
    world.despawn(e1);
    REQUIRE(removes == 2);
}

TEST_CASE (
"ECS Improvements: Entity Relations & Cascade Despawn"
,
"[ecs][relation]"
)
 {
    pebble::ecs::World world;

    auto parent = world.spawn();
    auto child1 = world.spawn();
    auto child2 = world.spawn();
    auto grandchild = world.spawn();

    world.relate<pebble::ecs::ChildOf>(parent, child1);
    world.relate<pebble::ecs::ChildOf>(parent, child2);
    world.relate<pebble::ecs::ChildOf>(child1, grandchild);

    auto children = world.related_to<pebble::ecs::ChildOf>(parent);
    REQUIRE(children.size() == 2);

    int count = 0;
    world.for_each_child(parent, [&](pebble::ecs::Entity) {
        ++count;
    });
    REQUIRE(count == 2);

    // Cascade despawn
    world.despawn_cascade(parent);
    REQUIRE_FALSE(world.alive(parent));
    REQUIRE_FALSE(world.alive(child1));
    REQUIRE_FALSE(world.alive(child2));
    REQUIRE_FALSE(world.alive(grandchild));
}

TEST_CASE (
"ECS Improvements: Linear Arena CommandBuffer"
,
"[ecs][command_buffer]"
)
 {
    pebble::ecs::World world;

    auto e1 = world.spawn();
    world.commands().add(e1, Pos{10.0f, 20.0f});
    world.commands().add(e1, Vel{1.0f, 2.0f});

    REQUIRE_FALSE(world.has<Pos>(e1));
    world.flush_commands();

    REQUIRE(world.has<Pos>(e1));
    REQUIRE(world.has<Vel>(e1));
    REQUIRE(world.get<Pos>(e1)->x == 10.0f);

    // LocalCommandBuffer test
    {
        pebble::ecs::LocalCommandBuffer local_cmd(world.commands());
        local_cmd.add(e1, Hp{50});
        local_cmd.remove<Vel>(e1);
    } // destructor merges to world.commands()

    REQUIRE_FALSE(world.has<Hp>(e1));
    world.flush_commands();

    REQUIRE(world.has<Hp>(e1));
    REQUIRE(world.get<Hp>(e1)->points == 50);
    REQUIRE_FALSE(world.has<Vel>(e1));
}

TEST_CASE (
"ECS Improvements: Topological System Scheduler"
,
"[ecs][scheduler]"
)
 {
    pebble::ecs::World world;
    auto e = world.spawn();
    world.add(e, Pos{0.0f, 0.0f});
    world.add(e, Vel{10.0f, 5.0f});

    pebble::ecs::Scheduler<pebble::ecs::World> scheduler;
    scheduler.add_system(PhysicsSystem{});
    scheduler.add_system(RenderSystem{});
    scheduler.build();

    REQUIRE(scheduler.system_count() == 2);

    scheduler.run(world, 0.1f);
    REQUIRE(world.get<Pos>(e)->x == 1.0f);
    REQUIRE(world.get<Pos>(e)->y == 0.5f);
}

TEST_CASE (
"ECS Improvements: Change Detection Ticks"
,
"[ecs][change_detection]"
)
 {
    pebble::ecs::World world;
    auto e1 = world.spawn();

    world.add(e1, Pos{1.0f, 2.0f});
    REQUIRE(world.store<Pos>().last_mutation_tick() == 0);

    world.advance_tick();
    REQUIRE(world.current_tick() == 1);

    world.add(e1, Pos{3.0f, 4.0f});
    REQUIRE(world.store<Pos>().last_mutation_tick() == 1);
}

TEST_CASE (
"ECS Improvements: Archetype Columnar Storage"
,
"[ecs][archetype]"
)
 {
    pebble::ecs::ArchetypeStorage storage;
    REQUIRE(storage.archetype_count() == 0);

    auto& arch1 = storage.get_or_create(0b01);
    REQUIRE(storage.archetype_count() == 1);
    REQUIRE(arch1.signature == 0b01);

    auto& arch2 = storage.get_or_create(0b11);
    REQUIRE(storage.archetype_count() == 2);

    auto& arch1_ref = storage.get_or_create(0b01);
    REQUIRE(&arch1_ref == &arch1);
    REQUIRE(storage.archetype_count() == 2);
}

TEST_CASE (
"ECS Improvements: Auto-Lead-Store Dynamic Join View"
,
"[ecs][view][lead_store]"
)
 {
    pebble::ecs::World world;

    // Create 100 entities with Pos
    for (int i = 0; i < 100; ++i) {
        auto e = world.spawn();
        world.add(e, Pos{static_cast<float>(i), static_cast<float>(i * 2)});
    }

    // Attach Vel to only 5 of them (Vel is the smaller lead store)
    for (std::uint32_t i = 1; i <= 5; ++i) {
        pebble::ecs::Entity e{i, 1};
        world.add(e, Vel{10.0f, 20.0f});
    }

    REQUIRE(world.store<Pos>().size() == 100);
    REQUIRE(world.store<Vel>().size() == 5);

    int visited_count = 0;
    // View queried as <Pos, Vel> should automatically evaluate Vel as the smaller lead store and visit exactly 5 matching entities
    world.view<Pos, Vel>([&](pebble::ecs::Entity, Pos& p, const Vel& v) {
        REQUIRE(p.y == p.x * 2.0f);
        REQUIRE(v.dx == 10.0f);
        REQUIRE(v.dy == 20.0f);
        p.x += v.dx;
        ++visited_count;
    });

    REQUIRE(visited_count == 5);
}

TEST_CASE (
"ECS Improvements: ErasedStore Type-Erased Ops (Zero Virtual)"
,
"[ecs][erased_store]"
)
 {
    pebble::ecs::ComponentStore<Pos> pos_store(1024);
    pos_store.set(1, Pos{42.0f, 84.0f});
    pos_store.set(2, Pos{10.0f, 20.0f});

    pebble::ecs::ErasedStore erased{
        .data = &pos_store,
        .destroy = nullptr,
        .erase = [](void* p, std::uint32_t idx) noexcept {
            static_cast<pebble::ecs::ComponentStore<Pos>*>(p)->erase_by_index(idx);
        },
        .set_raw = nullptr,
        .set_tick = nullptr,
        .size = [](const void* p) noexcept -> std::size_t {
            return static_cast<const pebble::ecs::ComponentStore<Pos>*>(p)->size();
        },
        .has = [](const void* p, std::uint32_t idx) noexcept -> bool {
            return static_cast<const pebble::ecs::ComponentStore<Pos>*>(p)->has(idx);
        },
        .type_id = 1
    };

    REQUIRE(erased.get_size() == 2);
    REQUIRE(erased.contains(1));
    REQUIRE(erased.contains(2));
    REQUIRE_FALSE(erased.contains(3));

    erased.erase_by_index(1);
    REQUIRE(erased.get_size() == 1);
    REQUIRE_FALSE(erased.contains(1));
    REQUIRE(erased.contains(2));
}

TEST_CASE (
"ECS Improvements: PagedSparse On-Demand Page Allocation"
,
"[ecs][paged_sparse]"
)
 {
    pebble::ecs::PagedSparse<std::uint32_t, 1024> paged_sparse;

    REQUIRE(paged_sparse.allocated_pages() == 0);
    REQUIRE_FALSE(paged_sparse.has(100));

    paged_sparse[100] = 42;
    REQUIRE(paged_sparse.allocated_pages() == 1);
    REQUIRE(paged_sparse.has(100));
    REQUIRE(paged_sparse.get(100) == 42);

    // Access high entity ID spanning to another page on-demand
    paged_sparse[3500] = 999;
    REQUIRE(paged_sparse.allocated_pages() == 2);
    REQUIRE(paged_sparse.has(3500));
    REQUIRE(paged_sparse.get(3500) == 999);

    paged_sparse.erase(100);
    REQUIRE_FALSE(paged_sparse.has(100));
    REQUIRE(paged_sparse.has(3500));
}


