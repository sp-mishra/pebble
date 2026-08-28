#include <catch_amalgamated.hpp>
#include "akruti/layout.hpp"

using namespace akruti::layout;

TEST_CASE("akruti::layout: Basic flexbox column and row solve", "[akruti][layout]") {
    LayoutTree tree;
    
    // Root container (Column, 200x400)
    LayoutNode root_node;
    root_node.style.axis = Axis::Column;
    root_node.style.width = SizeSpec::Px(200.0f);
    root_node.style.height = SizeSpec::Px(400.0f);
    auto root_id = tree.insert(nullptr, root_node);

    // Child 1 (50px height, flex_grow 0)
    LayoutNode child1;
    child1.style.width = SizeSpec::Px(100.0f);
    child1.style.height = SizeSpec::Px(50.0f);
    tree.insert(root_id, child1);

    // Child 2 (flex_grow 1.0)
    LayoutNode child2;
    child2.style.width = SizeSpec::Px(100.0f);
    child2.style.flex_grow = 1.0f;
    tree.insert(root_id, child2);

    Engine engine;
    engine.bake(tree);

    Bounds2D viewport{{0.0f, 0.0f}, {200.0f, 400.0f}};
    engine.solve(viewport);

    REQUIRE(engine.size() == 3);

    // Root rect
    CHECK(engine.rect[0].x == 0.0f);
    CHECK(engine.rect[0].y == 0.0f);
    CHECK(engine.rect[0].w == 200.0f);
    CHECK(engine.rect[0].h == 400.0f);

    // Child 1 rect
    CHECK(engine.rect[1].x == 0.0f);
    CHECK(engine.rect[1].y == 0.0f);
    CHECK(engine.rect[1].w == 100.0f);
    CHECK(engine.rect[1].h == 50.0f);

    // Child 2 rect (gets remaining 350px height)
    CHECK(engine.rect[2].x == 0.0f);
    CHECK(engine.rect[2].y == 50.0f);
    CHECK(engine.rect[2].w == 100.0f);
    CHECK(engine.rect[2].h == 350.0f);
}

TEST_CASE("akruti::layout: Min/Max specifications and aspect ratio lock", "[akruti][layout]") {
    LayoutTree tree;

    LayoutNode node;
    node.style.width = SizeSpec::Px(100.0f);
    node.style.min_width = SizeSpec::Px(150.0f); // Min width forces 150px
    node.style.height = SizeSpec::Px(200.0f);
    node.style.max_height = SizeSpec::Px(120.0f); // Max height clamps to 120px
    tree.insert(nullptr, node);

    Engine engine;
    engine.bake(tree);

    Bounds2D viewport{{0.0f, 0.0f}, {500.0f, 500.0f}};
    engine.solve(viewport);

    REQUIRE(engine.size() == 1);
    CHECK(engine.measured[0].w == 150.0f);
    CHECK(engine.measured[0].h == 120.0f);
}

TEST_CASE("akruti::layout: Padding, margin, and alignment positioning", "[akruti][layout]") {
    LayoutTree tree;

    LayoutNode root;
    root.style.axis = Axis::Row;
    root.style.width = SizeSpec::Px(300.0f);
    root.style.height = SizeSpec::Px(100.0f);
    root.style.padding = Edges{.l = 10.0f, .t = 10.0f, .r = 10.0f, .b = 10.0f};
    root.style.align_items = Align::Center;
    auto r_id = tree.insert(nullptr, root);

    LayoutNode item;
    item.style.width = SizeSpec::Px(50.0f);
    item.style.height = SizeSpec::Px(40.0f);
    item.style.margin = Edges{.l = 5.0f, .t = 0.0f, .r = 5.0f, .b = 0.0f};
    tree.insert(r_id, item);

    Engine engine;
    engine.bake(tree);

    Bounds2D viewport{{0.0f, 0.0f}, {300.0f, 100.0f}};
    engine.solve(viewport);

    REQUIRE(engine.size() == 2);
    // Content box starts at x = 10, child margin = 5 -> child x = 15
    CHECK(engine.rect[1].x == 15.0f);
    // Vertical center inside 80px available height (100 - 20 pad): (80 - 40) / 2 + 10 = 30
    CHECK(engine.rect[1].y == 30.0f);
    CHECK(engine.rect[1].w == 50.0f);
    CHECK(engine.rect[1].h == 40.0f);
}

TEST_CASE("akruti::layout: Spatial hash hit testing and rectangle queries", "[akruti][layout]") {
    LayoutTree tree;

    LayoutNode root;
    root.style.width = SizeSpec::Px(400.0f);
    root.style.height = SizeSpec::Px(400.0f);
    auto r_id = tree.insert(nullptr, root);

    LayoutNode box;
    box.style.width = SizeSpec::Px(100.0f);
    box.style.height = SizeSpec::Px(100.0f);
    tree.insert(r_id, box);

    Engine engine;
    engine.enable_spatial_hash = true;
    engine.bake(tree);

    Bounds2D viewport{{0.0f, 0.0f}, {400.0f, 400.0f}};
    engine.solve(viewport);

    auto hit = engine.hit_test(50.0f, 50.0f);
    REQUIRE(hit.has_value());
    CHECK(hit.value() == 1); // Hit box

    auto miss = engine.hit_test(350.0f, 350.0f);
    REQUIRE(miss.has_value());
    CHECK(miss.value() == 0); // Hit root container
}

TEST_CASE("akruti::layout: Constraint graph and parent matching", "[akruti][layout]") {
    LayoutTree tree;

    LayoutNode root;
    root.style.width = SizeSpec::Px(200.0f);
    root.style.height = SizeSpec::Px(200.0f);
    auto r_id = tree.insert(nullptr, root);

    LayoutNode child;
    child.style.match_parent_width = true;
    child.style.center_y = true;
    child.style.height = SizeSpec::Px(50.0f);
    tree.insert(r_id, child);

    Engine engine;
    engine.bake(tree);

    Bounds2D viewport{{0.0f, 0.0f}, {200.0f, 200.0f}};
    engine.solve(viewport);

    REQUIRE(engine.size() == 2);
    CHECK(engine.rect[1].w == 200.0f);
    CHECK(engine.rect[1].y == 75.0f);
}

TEST_CASE("akruti::layout: Text measurement integration", "[akruti][layout]") {
    LayoutTree tree;

    LayoutNode label;
    label.style.text = "Hello Akruti";
    label.style.text_id = 42;
    tree.insert(nullptr, label);

    Engine engine;
    engine.text_measure_callback = TextMeasure{
        [](const char* text, float, void*) -> Size2D {
            return Size2D{.w = 120.0f, .h = 24.0f};
        },
        nullptr
    };

    engine.bake(tree);
    Bounds2D viewport{{0.0f, 0.0f}, {500.0f, 500.0f}};
    engine.solve(viewport);

    REQUIRE(engine.size() == 1);
    CHECK(engine.measured[0].w == 120.0f);
    CHECK(engine.measured[0].h == 24.0f);
}

TEST_CASE("akruti::layout: Layout snapshot capture and restore", "[akruti][layout]") {
    LayoutTree tree;

    LayoutNode node;
    node.style.width = SizeSpec::Px(100.0f);
    node.style.height = SizeSpec::Px(100.0f);
    tree.insert(nullptr, node);

    Engine engine;
    engine.enable_snapshots = true;
    engine.bake(tree);

    Bounds2D viewport{{0.0f, 0.0f}, {200.0f, 200.0f}};
    engine.solve(viewport);

    CHECK(engine.measured[0].w == 100.0f);

    // Modify style & solve again
    LayoutStyle modified;
    modified.width = SizeSpec::Px(180.0f);
    modified.height = SizeSpec::Px(180.0f);
    engine.set_style(0, modified);
    engine.solve(viewport);

    CHECK(engine.measured[0].w == 180.0f);

    // Restore first snapshot (from original solve)
    bool restored = engine.restore_snapshot(0);
    REQUIRE(restored);
    CHECK(engine.measured[0].w == 100.0f);
}

TEST_CASE("akruti::layout: True incremental subtree solving", "[akruti][layout]") {
    LayoutTree tree;

    // Root container (Column, 400x600)
    LayoutNode root_node;
    root_node.style.axis = Axis::Column;
    root_node.style.width = SizeSpec::Px(400.0f);
    root_node.style.height = SizeSpec::Px(600.0f);
    auto root_id = tree.insert(nullptr, root_node);

    // Child 1 (Fixed 100px)
    LayoutNode child1;
    child1.style.width = SizeSpec::Px(200.0f);
    child1.style.height = SizeSpec::Px(100.0f);
    tree.insert(root_id, child1);

    // Child 2 (Flex 1.0)
    LayoutNode child2;
    child2.style.width = SizeSpec::Px(200.0f);
    child2.style.flex_grow = 1.0f;
    tree.insert(root_id, child2);

    Engine engine;
    engine.enable_perf_tracking = true;
    engine.bake(tree);

    Bounds2D viewport{{0.0f, 0.0f}, {400.0f, 600.0f}};
    engine.solve(viewport);

    CHECK(engine.rect[1].h == 100.0f);
    CHECK(engine.rect[2].h == 500.0f);

    // Incremental solve when not dirty does zero node measurements
    engine.solve_incremental(viewport);
    CHECK(engine.perf_stats.nodes_measured == 3); // Unchanged from initial solve

    // Modify child 1 height and run incremental solve
    LayoutStyle mod_child1;
    mod_child1.width = SizeSpec::Px(200.0f);
    mod_child1.height = SizeSpec::Px(150.0f);
    engine.set_style(1, mod_child1);

    engine.solve_incremental(viewport);
    CHECK(engine.rect[1].h == 150.0f);
    CHECK(engine.rect[2].h == 450.0f);
}

TEST_CASE("akruti::layout: Smriti ScopedArena zero-heap scratch integration", "[akruti][layout]") {
    smriti::pools::ScopedArena<4096> arena;
    void* ptr = arena.allocate(sizeof(Rect2D) * 10, alignof(Rect2D));
    REQUIRE(ptr != nullptr);
    REQUIRE(arena.used_bytes() >= sizeof(Rect2D) * 10);
    arena.reset();
    CHECK(arena.used_bytes() == 0);
}

