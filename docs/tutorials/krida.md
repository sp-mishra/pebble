# Tutorial: The Whole Elephant — Akruti, Gati, Kalpana & Spandana as One System

A parable: four blind sages meet an elephant. "A rope!" says the one clutching the tail. "A wall!" insists the one at
its flank. "A snake!" cries the one holding the trunk. "A tree trunk!" booms the one hugging the leg. Every one of them
is describing the same animal, and every one of them is wrong about what it is.

Pebble's realtime stack is the same. **Akruti** is the mathematics of *where* — shapes and the space they occupy.
**Kalpana** is the art of *how it looks* — paint, paths, and pixels. **Gati** is the discipline of *when* — the clock,
the loop, the orchestration of change. **Spandana** is the *what happens* — the declarative pulse that gives a world
life.

Pick up any one header and you have a library. This tutorial picks up all four, and builds a *game*.

By the end you will hold the whole elephant: a space-rope-snake-tree-game with bouncing shapes, a steady 60&nbsp;Hz
simulation, vector-graphics presentation, and buttery tweens — with each piece living where it belongs and not a layer
of glue anywhere.

---

## Table of Contents

1. [The Four Layers, One Triangle](#1-the-four-layers-one-triangle)
2. [The Data Flow: One Frame from Spawn to Pixel](#2-the-data-flow-one-frame-from-spawn-to-pixel)
3. [Act 1 — Akruti: The World Has Shape](#3-act-1--akruti-the-world-has-shape)
4. [Act 2 — Kalpana: The World Has Color](#4-act-2--kalpana-the-world-has-color)
5. [Act 3 — Gati: The World Has a Pulse](#5-act-3--gati-the-world-has-a-pulse)
6. [Act 4 — Spandana: The World Speaks](#6-act-4--spandana-the-world-speaks)
7. [The Whole Elephant: A Playable Game Loop](#7-the-whole-elephant-a-playable-game-loop)
8. [Choosing Your Weapon: A Decision Table](#8-choosing-your-weapon-a-decision-table)
9. [Composition Rules of Thumb](#9-composition-rules-of-thumb)
10. [Cheat Sheet](#10-cheat-sheet)

---

## 1. The Four Layers, One Triangle

Only four words you need to remember:

| Library      | Sanskrit root        | Answers the question                                | Owns                                                |
|--------------|----------------------|-----------------------------------------------------|-----------------------------------------------------|
| **Akruti**   | आकृति — *shape / form* | Where is everything, and what does it collide with? | Geometry, SDFs, narrowphase, CSG, fracture          |
| **Kalpana**  | कल्पना — *imagination*  | What does it look like?                             | 2D vector graphics, paint, color, backends          |
| **Gati**     | गति — *motion / gait* | When does simulation step?                          | Game clock, ECS, systems, fixed-timestep loop       |
| **Spandana** | स्पन्दन — *pulsation*    | What does the world *do*?                           | Tyveens, springs, particles, materials, destruction |

There is deliberate overlap in *capability* — Akruti can raycast a scene, and Spandana can move a shape. The art is not
in what each one *can* do; it is in the *direction of the dependency*:

```text
        ┌────────────────────────────────────────────┐
        │           Spandana  (what happens)          │   ← knows nothing; expresses
        │                 │  │                        │
        ▼                 ▼                           │
   ┌─────────┐      ┌─────────┐      ┌────────────┐   │
   │ akruti  │      │  gati   │      │  kalpana   │   │
   │(where)  │      │ (when)  │      │  (looks)   │   │
   └────┬────┘      └────┬────┘      └─────┬──────┘   │
        └────────────────┼────────────────┘          │
                         ▼                           │
                   pebble::math,                      │
                   pebble::ecs,                       │
                   pebble::pravaha                    │
        ──────────────────────────────────────────────┘
        A game uses all four; no layer calls between
        the leaves except through published data.
```

**The holistic rule:** *simulation state lives in your ECS components; Akruti computes geometric facts about them; Gati
advances them on a fixed clock; Kalpana turns the result into pixels; Spandana writes the animation/particle material as
a pure function of time.*

When you honor that sentence, every library interoperates with the others with zero glue code, because they all speak
the same nouns: `pebble::math::vec2`, units of seconds, and your ECS components.

---

## 2. The Data Flow: One Frame from Spawn to Pixel

Let's compress one frame of the game we're about to build and label every transition with its owning library:

```text
  [wall clock]  --dt-->  GATI: Game::update(dt)            (fixed-step accumulator)
                              │ 0..N steps of
                              ├─> AKRUTI: collide, sweep,   (pure geometry queries)
                              │          update Scene AABBs
                              ├─> ECS systems: move, kill,  (your game code)
                              │          spawn, score
                              │
                              └─> SPANDANA: Timeline::update(dt)  (tweens, springs,
                                                                    particles, effects)
  [presentation]  --alpha-->  GATI: game.alpha()            (blend factor in [0,1))
                              │
                              ▼
                      KALPANA: Scene graph ← shapes from Akruti
                                         ← colors from spectral mixing
                                         → render(SOKOL | HEADLESS | TERMINAL)
```

Four libraries, one loop, three state transitions (`simulate → present → composite`). Notice what is **not** in the
loop: conversions, copies, or callbacks stitching "engine A" to "engine B". Akruti and Kalpana would barely ten lines
apart; Gati's `Game` facade is what glues them via your `World`.

We'll build that game now, in dependency order — not the order you read about them, but the order truth flows: geometry,
paint, clock, life.

---

## 3. Act 1 — Akruti: The World Has Shape

### 3.1 Everything is a shape

Akruti's contract is one concept, `akruti::Shape`, satisfied by a dozen primitives — `Circle`, `Box`, `OrientedBox`,
`Capsule`, `Triangle`, `RoundedBox`, `Sector`, `HalfPlane`, `ConvexPoly<N>`, and more — each carrying three queries:

- `sdf(p)` — the signed distance to the shape's boundary at point `p`
- `aabb()` — axis-aligned bounding box
- `support(d)` — the point farthest along direction `d`

That is the entire zoo. No `virtual`, no RTTI, no trait registration. A function templated on the shape type *is* the
interface:

```cpp
#include <akruti/akruti.hpp>
using namespace akruti;

template <akruti::Shape S>
void describe(const S& shape, const char* name) {
    // sdf(), aabb(), and support() are the entire contract
    (void)shape;
}

// All of these just work:
Circle    c{{0.0f, 0.0f}, 1.0f};
Box       b{{0.0f, 0.0f}, {2.0f, 1.0f}};
OrientedBox ob{{0.0f, 0.0f}, {1.0f, 2.0f},
               pebble::math::mat2::rotation(pebble::math::kPi / 4.0f)};
```

### 3.2 Collision, from O (1) to generic

Because Akruti is header-only and concept-based, the fast paths never wait for the slow ones:

```cpp
// O(1) analytic fast paths — zero iterations
Manifold mc = akruti::collide_circle_circle({{0, 0}, 1.0f},
                                            {{1.7f, 0}, 1.0f});
// mc.hit == true, mc.depth == 0.3f

// 2-point SAT manifolds for rock-stable stacking
OrientedBox pa{{0, 0}, {1, 1}, pebble::math::mat2::rotation(0.0f)};
OrientedBox pb{{0.5f, 1.3f}, {1, 1}, pebble::math::mat2::rotation(0.1f)};
Manifold m2 = akruti::collide_obb_obb(pa, pb);

// Generic fallback: warm-started GJK/EPA for everything else
SimplexCache cache;
Manifold m3  = akruti::collide_gjk_warm_started(pa, pb, &cache); // previous axis reused
```

**The holistic point:** `Manifold` gives you `hit`, `depth`, a contact **normal**, and (on the SAT paths) **2 points**
for a stable manifold. Your physics, your particles, and your sound trigger all read the *same* facts — no second code
path for "close enough".

### 3.3 CSG: carve what you need

Your game will want a green pipe that is "a rounded box with a circle chewed out". Akruti's CSG expression EDSL does it
with zero heap and zero copies — just shape algebra:

```cpp
#include <akruti/csg.hpp>
using namespace akruti::expr;

Circle coin{ {0, 0}, 1.0f };
Box    slot{ {0.5f, 0}, {0.4f, 1.0f} };

auto pickaxe = coin - slot;                         // one expression, no allocation
Scalar d = pickaxe.sdf({0.5f, 0.0f});               // > 0 — the slot is carved out
```

Compose with `operator|` (union), `&` (intersection), `-` (difference), or inflate with `csg_shell` / `csg_offset`.
Dynamic shapes you inspect at runtime use the same operators on `FlatCsgTree` — the *expression* stays the interface.

### 3.4 Scene layer: shapes at scale

For anything beyond a dozen bodies, hand-rolling nested loops wastes the core count. Akruti's `scene::Scene` gives you
SoA batches per primitive type, a dynamic `AABBTree` broadphase, and `LayerMask` collision filtering, with bulk
operations that are Pravaha-parallel when `AKRUTI_ENABLE_PRAVAHA` is set:

```cpp
#include <akruti/scene/scene.hpp>
akruti::scene::Scene scene;                 // one per game level

auto& circles = scene.batch<Circle>();      // SoA column storage
circles.emplace(Circle{{0, 0}, 1.0f});
circles.emplace(Circle{{5, 0}, 1.0f});

// Bulk queries return immediately consumable results
for (auto [i, j] : scene.broadphase_pairs()) {
    // i, j are indices into their respective batches — already narrowed by AABBs
}
```

> [!TIP]
> **The holistic rule for Akruti:** your *logic* decides entities exist; Akruti decides *what the geometry says*. Never
store gameplay state inside an Akruti shape — that is what ECS components are for. Treat Akruti as a stateless geometry
oracle you query during a fixed step.

---

## 4. Act 2 — Kalpana: The World Has Color

A world of manifolds and distances is indistinguishable from a physics test. Kalpana is the layer that renders. It
shares Akruti's DNA — header-only C++23, concept-monomorphized backends — but it renders *paint*, not pixels. You
describe a `Scene` of `Node`s; the backend (Sokol GPU, headless capture, or Notcurses terminal) does the rest.

### 4.1 Your first sprite, painted properly

```cpp
#include <kalpana/kalpana.hpp>
using namespace kalpana;

Scene scene;
scene.clear_color(colors::white());

// 1. Paths are vector commands — drag a rounded rectangle
Path card;
card.round_rect(10.0f, 10.0f, 120.0f, 60.0f, 8.0f, 8.0f);

// 2. Paints fill strokes. Filled+outlined gives you both in one object.
scene.add(Node::shape(std::move(card), Paint::filled_outlined(
    colors::steelblue(), colors::black(), 2.0f)));
```

### 4.2 Money shot: subtractive spectral pigment mixing

Here is where Kalpana stops being "a canvas" and starts being magic. **If you mix blue and yellow pigment in the
physical world, you get green — not grey.** Linear RGB blending of `(0,0,1)` and `(1,1,0)` gives you grey-brown mush,
which is why digital art software keeps disappointing children. Kalpana instead models *light reflectance* via the
**Kubelka–Munk** theory of pigment mixing:

```cpp
// Linear RGB "mixing" — B + Y = grey. Wrong reality, wrong instinct.
Color mud    = Color{0, 0, 1} * 0.5f + Color{1, 1, 0} * 0.5f;

// Kalpana spectral mixing — B + Y = vibrant green. Physics.
Color vibrant = spectral::mix(colors::blue(), colors::yellow(), 0.5f);
```

Why do we care in a *game*? **Juice.** When your player heals, mixes a potion, lashes a laser pistol — pigments,
potions, and light get *mixed*. Do it with the model that matches human perception, not the one that matches your
graphics card.

### 4.3 Minimal full-app golden path

Kalpana lets you bring your own backend with zero changes to your scene code:

```cpp
// A) Headless — CI tests, server-rendered thumbnails
DefaultCanvas headless(256, 128);
headless.render(scene);
std::vector<std::uint32_t> pixels = headless.snapshot();  // ARGB8888

// B) GPU — the real window
SokolCanvas gpu(1280, 720);
gpu.render(scene);

// C) Terminal — debug builds, SSH demos, ancient hardware
TerminalCanvas term(80, 24);
term.render(scene);
```

> [!TIP]
> **Keep Akruti shapes importable.** `kalpana::geom::Path` imports Akruti curves and polylines directly. Design your
`Path`-producing functions to take `akruti::Circle` or `akruti::Poly` and convert at the edge — you get collision from
Akruti *and* rendering from Kalpana with a single source of truth for each body's geometry:
> ```cpp
> kalpana::Path from_akruti(const akruti::Circle& c);       // dst changes, src doesn't
> ```

---

## 5. Act 3 — Gati: The World Has a Pulse

We have a world with shape and color. Now: *when* does it move? This is Gati's entire personality — it is a **realtime
runtime**, not a "game framework with a `while` loop inside." It owns the clock, the ECS `World`, the system schedule,
events, and the interpolation factor, and it forces every simulation step to be **deterministic** and **fixed-rate** so
100 Hz is 100 Hz regardless of whether the monitor is 60 Hz or 144 Hz.

### 5.1 The fixed-timestep heart

```cpp
#include <gati/gati.hpp>
using namespace gati;

Game game;                        // World + Clock + SystemStack + scene + scratch arena
while (running) {
    game.update(real_dt);         // drains 0..N fixed steps, flushes commands
    float alpha = game.alpha();   // [0, 1) — how far between fixed steps are we?
    render(alpha);
}
```

`Game::update(real_dt)` does the "Fix Your Timestep" dance: it advances an accumulator, runs a fixed `Clock`-spaced step
as often as needed (up to `max_steps` so you don't spiral to death), and calls `World::flush_commands()` — which is
where `world.destroy(entity)` is safe, because destruction during iteration is a footgun.

**Rendering** then uses `alpha` to interpolate between each entity's `Transform` at the previous and current fixed
steps, so a 10&nbsp;Hz logic simulation still renders at 243&nbsp;fps glass:

```cpp
while (running) {
    game.update(real_dt);
    const Scalar a = game.alpha();
    game.world().view<Transform>(Force, [&](Entity, Transform& t) {
        Vec2 render_pos = lerp(t.prev_position, t.position, a);
        draw_sprite_at(render_pos, t.angle);
    });
}
```

> [!NOTE]
> Gati aliases the math types — `gati::Vec2` *is* `pebble::math::vec2`, `gati::Mat2` *is* `peace::math::mat2`,
`gati::AABB` *is* `pebbling::math::aabb2`. No conversion layer, no `to_gfx`/`from_gfx`. Your physics math, your
renderer, and your game code all share one source of truth for a 2D vector.

### 5.2 The whole world is your ECS

`Game::world()` is a `pebble::ecs::World`; entities are just `Entity` handles. Components are plain structs you define —
Gati never touches them, it just moves them:

```cpp
struct Position { Vec2 position; };

Entity player = game.world().spawn();
game.world().add<Position>(player, {.position = {0.0f, 10.0f}});
```

**No `Start()`/`Update()` overrides, no reflection, no MonoBehaviour.** If you have ever fought a game engine's
initialization order or tried to inherit from a component, this is deliberately the opposite of that.

### 5.3 The bridges

Gati doesn't know what Akruti or Prakriti (dynamics engine) *are* — it just plays nicely with their data. Enable the
integrations with compile-time macros so the cost is exactly zero when off:

```cpp
#define GATI_ENABLE_PRAKRITI
#define GATI_ENABLE_AKRUTI
#include <gati/gati.hpp>
```

When on, the `DefaultSystems` schedule becomes:

```
AnimationSystem → PhysicsSyncSystem → CollisionSystem → (your systems)
```

- **AnimationSystem** — advances per-entity `AnimationState`s.
- **PhysicsSyncSystem** — pushes `Transform`s into/out of Prakriti bodies.
- **CollisionSystem** — fans Akruti overlaps into collision *events* (pair them with Gati's `EventBus` in
  `game.events()`).

---

## 6. Act 4 — Spandana: The World Speaks

Now the real magic. You have a world that has shape, color, and a pulse. **Spandana is the language you use to tell that
world what to do.** No more imperative `if (needs_to_wait) { timer -= dt; }` bookkeeping — you declare the *desired
effect* as a value that automatically schedules, dependencies, and parallelizes itself.

### 6.1 Timelines: animations as values, parallelism for free

A `Timeline` is a list of `IAnimationAction`s. Every action declares its **`ResourceKey`** — an *exact*
`(entity-id, component-field)` pair. Actions touching *different* `ResourceKey`s automatically run concurrently; actions
touching the *same* field **automatically serialize in declaration order**. No manual synchronization, no data races on
your component state:

```cpp
#include <spandana/spandana.hpp>
using namespace pebble::spandana;
using namespace pebble::spandana::edsl;

Timeline tl;

// Animations targeting DIFFERENT entities run in parallel
tl.add(tween(player_pos).to({10.0f, 0.0f}).duration(1.0f));
tl.add(tween(bullet_pos).to({-5.0f, 2.0f}).duration(0.2f)));
// The next line implicitly depends on the first: bullet waits for player.
tl.add(spring(camera_pos).target(player_pos).stiffness(120.0f));

// In your game loop:
tl.update(dt);
```

We moved the *what* (a `tween` from X to Y), not the *how* (a million `if`s).

### 6.2 The World EDSL: everything in one composable language

All of Spandana's subsystems speak the same operator-overloaded EDSL, so you can mix motion, particles, and physics in a
single call:

```cpp
// Motion
tween(card_pos).to({40.0f, 20.0f}).ease(ease::back_out)      // bounce-back ease
      .duration(0.5);
spring(enemy_pos).target(player_pos).stiffness(300.0f);

// Akruti splines are first-class shapes here
akruti::CatmullRomSpline rail = {/* ... */};
follow_path(ship_pos, rail).orient_to_tangent(true).speed(60.0f);

// CSG morphs
morph_shape(coin_a, coin_b);                                   // shape-fu!

// Particles
particle_burst().at(explosion_site).count(64)
    .speed(100.0f, 300.0f).lifetime(0.5f, 1.5f).drag(2.0f);

// Physics: an impulse at a point
radial_impulse().at(explosion_site).radius(80.0f).magnitude(500.0f);

// Procedural destruction: break an entity into Voronoi shards with correct inertia
shatter_entity(glass_entity).at(bullet_pos).shards(12);

// Flipbooks, blend spaces, and screenshake need no introduction
flipbook(sprite).play("attack").speed(2.0f);
BlendSpace2D blend = /* ... */;
shake_camera(camera).trauma(0.7f).duration(0.3f);
```

All of this is a pure value — build it once, feed it to the `Timeline`, `update(dt)` and the magic of `ResourceKey` does
the rest.

### 6.3 Materials that melt and elements that react

Spandana doesn't stop at position. If a world has context — heat, state of matter, elemental composition — Spandana
gives it a pulse:

```cpp
using namespace gati;   // materials & elements are core-real

// A material is a 4-fraction model: solid, plastic, liquid, gas
set_material(world, ice_block)
    .temperature(-15.0f)                // degrees C
    .fraction(MaterialFraction::Solid, 1.0f);

// Add heat — it physically diffuses, melts, boils, and eventually ignites
apply_heat(world).at({50.0f, 0.0f})
    .temperature(500.0f).radius(80.0f).duration(1.0f);
// → ice melts to water, then boils to steam, with all phase-change enthalpy

// Elemental chaos: make metal conduct electricity into water near an enemy!
// Water + Lava → Obsidian + Steam. Fire + Wood → Ignition. Acid + Metal → Corrosion.
reaction(Water{}, Lava{});   // → Obsidian + Steam
```

**Water + Lava → Obsidian + Steam.** That's elemental resolution — Spandana's `elemental.hpp` actually *checks material
pairs* and emits a reaction with products. Your game doesn't have to care about the periodic table; it just asks "what
happens when X and Y touch?" and the engine knows.

> [!TIP]
> The whole Spandana layer is **serializable** via Glaze-based `serialization.hpp` — freeze a `Timeline` or a material
configuration to JSON and back. Build your game's content pipeline on top of it, and hot-reloading becomes a
`BlendSpace2D` reload away.

---

## 7. The Whole Elephant: A Playable Game Loop

Now, assemble all four acts. Our game: *"Krida — an ode to Newton"*. You control a player circle. You shoot projectiles
(juice!) at shapes that tumble in. Every collision is real (Akruti), every pixel is painted (Kalpana), everything steps
in rhythm with the fixed clock (Gati), and every explosion, tween, and mutter of "material" is Spandana.

```cpp
#include <akruti/akruti.hpp>
#include <gati/gati.hpp>
#include <kalpana/kalpana.hpp>
#include <spandana/spandana.hpp>

#define GATI_ENABLE_AKRUTI
#define GATI_ENABLE_PRAKRITI

using namespace gati;
using namespace kalpana;
using akruti::Circle;
using pebble::math::vec2;

using akruti::RayHit;

// ── 0. Component ― game state lives here, nowhere else ─────────────────────
struct Health  { float hp = 100.0f; };
struct Enemy   { float speed = 0.5f; };

int main() {
    Game game;                                  // GATI: world + clock + loop

    // ── 1. Entities & components ──────────────────────────────────────────
    Entity player = game.world().spawn();
    game.world().add<Transform>(player, {.position = {0.0f, 0.0f}});
    game.world().add<Health>(player, {});

    for (int i = 0; i < 8; i++) {
        Entity e = game.world().spawn();
        game.world().add<Transform>(e, {.position = {randf(-8, 8), randf(-4, 4)}});
        game.world().add<Enemy>(e, {.speed = randf(0.2f, 1.0f)});
    }

    // ── 2. GATI: the fixed-step clock drives the world ───────────────────
    float total = 0.0f;
    while (game.clock().total_steps() < 60 * 60) {   // 60 seconds
        const Scalar dt = 1.0f / 60.0f;

        // (input handling, AI…)

        // Manually flip scene graph transforms each frame from ECS:
        game.world().view<Transform, Enemy>(game.world().new_view(),
            [&](Entity e, Transform& t, const Enemy& en) {
                t.position.x += en.speed * dt;          // simple bounce on edges
                if (t.position.x > 9 || t.position.x < -9) {
                    game.world().get<Transform>(e).angle += dt;
                }
            });

        game.update(dt);                                 // advances all fixed steps

        // ── 3. AKRUTI: ask the world geometric questions ────────────────
        akruti::scene::Scene akruti_scene;
        auto& player_circle = akruti_scene.batch<Circle>().emplace(Circle{{0, 0}, 1.0f});
        AkrutiQuery(player, player_circle);
    }

    // ── 4. KALPANA: paint the result ─────────────────────────────────────
    kalpana::Scene screen;
    screen.clear_color(colors::black());
    // (…sprites…)
    // (…render it…)
    return 0;
}
```

*(Stubs — the real loop lives in your game; the point is structure, not the exact Emits.)*

---

## 8. Choosing Your Weapon: A Decision Table

The single most common real-world question the author sees: *"Should my effect be a Spandana tween or a Gati
PhysicsSystem?"* Here is your rule of thumb:

| I want to…                                  | …use this    | Because                                                           |
|---------------------------------------------|--------------|-------------------------------------------------------------------|
| Define the shape of a level                 | **Akruti**   | `Shape` has SDF/AABB/support, which is everything collision needs |
| Test if two things overlap                  | **Akruti**   | `collide_*` / GJK — pure geometry                                 |
| Give an object color, path, or gradient     | **Kalpana**  | Paint, Paths, Backends                                            |
| Mix two colors like paint                   | **Kalpana**  | `spectral::mix` uses Kubelka-Munk, not linear RGB                 |
| Move at a *fixed rate* per second           | **Gati**     | Clock + fixed-step loop. Period.                                  |
| Order systems / share a frame budget        | **Gati**     | `DefaultSystems` schedule + `World`                               |
| Animate a value from A to B, or with easing | **Spandana** | `tween` / `spring` timelines                                      |
| Spawn a just-in-time particle burst         | **Spandana** | `particle_burst`, built-in scheduler/dependency inference         |
| Melt a frozen block as it heats             | **Spandana** | Material thermodynamics + `apply_heat`                            |
| Render a custom texture                     | **Kalpana**  | `ImageNode` + backend                                             |

**The real rule:** *Functional state* (position, rotation, health) lives in your ECS components. *Transient
presentation* (position during a tween, particle count, heat shimmer) is a Spandana-born idea. *Collision* is an Akruti
question. *Timing* is a Gati decision. When in doubt, ask *"what is this property* **fundamentally**?"* and route it
there.

---

## 9. Composition Rules of Thumb

1. **The dependency arrow points down.** Game code → Spandana → (Gati, Akruti, Kalpana) → `pebble::math`/`ecs`/
   `pravaha`. Never make a leaf depend on a root.
2. **No glue.** You never need to write "geometry adapter" code: Akruti outputs `pebble::math::vec2`; so does your
   renderer; so does your tween. Reuse, don't wrap.
3. **Update the ECS, not the picture.** Move `Transform` components; let Kalpana read them. If you find yourself calling
   `setPosition()` on a renderable every frame, stop — you want an ECS component and a system that reads it.
4. **Fixed-step for simulation, interpolate for render.** `game.update(dt)` at a fixed 60 Hz, `alpha` for smoothness;
   never the other way round.
5. **The `macros` are compile-time;** the API is the same. `GATI_ENABLE_AKRUTI`, etc., add or skip whole systems; they
   never change call sites.

And the meta-rule, the holistic one, the one this entire *tutorial* is about:

> **Every system is a query over the world, and every world is a set of queries over components.** Akruti knows
> geometry, Kalpana knows art, Gati knows the clock, Spandana knows desire. Make your code say *what* the world is, and
> the engine — all four layers — will say *how*.

---

## 10. Cheat Sheet

| Concern                | Function / Type                                          | Library  |
|------------------------|----------------------------------------------------------|----------|
| Include everything     | `akruti/akruti.hpp`                                      | Akruti   |
| A shape                | `Circle`, `Box`, `OrientedBox`, `Capsule`, `Sector`, …   | Akruti   |
| Signed distance        | `shape.sdf(p)`                                           | Akruti   |
| Overlap + manifold     | `collide_obb_obb(a, b)`, `collide_gjk_warm_started(...)` | Akruti   |
| CSG (zero-alloc)       | `a \| b`, `a & b`, `a - b`                               | Akruti   |
| Broadphase             | `scene::Scene{}.broadphase_pairs()`                      | Akruti   |
| Include everything     | `kalpana/kalpana.hpp`                                    | Kalpana  |
| Path / shape           | `Path::round_rect(...)`, `Path::add_circle(...)`         | Kalpana  |
| Paint                  | `Paint::filled_outlined(...)`, `Paint::gradient(...)`    | Kalpana  |
| Cheap realistic mixing | `spectral::mix(a, b, t)`                                 | Kalpana  |
| Render                 | `Canvas<Backend>(w,h).render(scene)`, `.snapshot()`      | Kalpana  |
| Run your game          | `Game game; game.update(dt); game.alpha();`              | Gati     |
| Fixed step clock       | `game.clock().dt()`, `.total_steps()`, `.alpha()`        | Gati     |
| ECS                    | `game.world().spawn()/add()/get()/view()`                | Gati     |
| Include everything     | `spandana/spandana.hpp`                                  | Spandana |
| Animate a value        | `tween(prop).to(v).ease(...)`, `spring(prop).target(v)`  | Spandana |
| Particles / shake      | `particle_burst()`, `shake_camera(cam).trauma(...)`      | Spandana |
| Change of state        | `set_material(...)`, `apply_heat(...)`, `reaction(A, B)` | Spandana |

Go build the whole elephant. 🐘
