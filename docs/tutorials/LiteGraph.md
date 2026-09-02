# Tutorial: Learn graphs by mapping Pebble Island

You are the island cartographer. Each step adds one idea — nodes, edges, walks, shortest paths, schedules, rumors,
storms, fame — until you have a real map and the algorithms that make it useful.

Keep one file open. After each step, compile, print, then add the next block. C++23, include path `include/`.

Steps 1–9 fit in a small `main`. From Step 10 you also need the profiler, a random generator, and an execution policy:

```cpp
#include "containers/graph/LiteGraph.hpp"
#include "containers/graph/LiteGraphAlgorithms.hpp"
#include "containers/graph/LiteGraphHighway.hpp"  // Step 13, SIMD PageRank
#include "utils/profiler.hpp"                     // timings + compare()

#include <execution>
#include <iostream>
#include <numeric>
#include <random>
#include <string>
#include <thread>
#include <vector>

int main() {
    // The island grows here.
}
```

On some GCC setups `std::execution::par` needs the TBB library. If the linker complains, race with `std::execution::seq`
first, then add `-ltbb`. Apple Clang usually just works.

Need a function? Look it up in [`docs/containers/LiteGraph.md`](../containers/LiteGraph.md). Need a worked test?
`src/tests/test_LiteGraph.cpp`.

---

## Before you sail

A **graph** is places plus connections.

- A **node** is a place (Harbor, Lighthouse).
- An **edge** is a connection (a ferry, a trail, “must happen before”).
- **Directed** means one-way (`Harbor → Cove`). **Undirected** means you can walk either way.

LiteGraph stores both as data you choose: strings for names, `int`/`double` for minutes or tickets. IDs are strong types
(`NodeId`, `EdgeId`) so you cannot mix up a dock with a route.

---

## Step 1 — Plant the first four docks

**Goal:** create a directed map and print it.

Four settlements. Ferries run one way with a travel time in minutes.

```cpp
using Graph = litegraph::Graph<std::string, int, litegraph::Directed>;

Graph island;
island.reserve_nodes(8);
island.reserve_edges(16);

const auto harbor     = island.add_node("Harbor");
const auto market     = island.add_node("Market");
const auto cove       = island.add_node("Cove");
const auto lighthouse = island.add_node("Lighthouse");

island.add_edge(harbor, market, 12);       // Harbor -> Market, 12 min
island.add_edge(harbor, cove, 20);
island.add_edge(market, cove, 8);
island.add_edge(cove, lighthouse, 15);
island.add_edge(market, lighthouse, 40);   // scenic, slower

std::cout << "Docks: " << island.node_count()
          << "  ferries: " << island.edge_count() << "\n\n";

litegraph::to_ascii(
    island, std::cout,
    [](const std::string& name) { return name; },
    [](const int minutes) { return std::to_string(minutes) + "m"; }
);
```

`to_ascii` lives in `LiteGraphAlgorithms.hpp`. You should see Harbor pointing at Market and Cove, Market at Cove and
Lighthouse, Cove at Lighthouse.

**What you just learned:** `add_node` returns a `NodeId` you keep. `add_edge(from, to, data)` is always `from → to` on a
directed graph. Counts are *active* nodes and edges.

**Try:** add `"Cliffs"` and a 7-minute ferry from Cove. Print again. Then undo it for the next steps, or keep it and
adjust later answers.

---

## Step 2 — Stand on the pier and look around

**Goal:** neighbors, degree, and walking edges.

```cpp
std::cout << "From Harbor you can sail to:\n";
for (const auto dest : island.neighbors(harbor)) {
    std::cout << "  - " << island.node_data(dest) << "\n";
}

std::cout << "Harbor out-degree: " << island.out_degree(harbor)
          << "  (how many ferries leave)\n";
std::cout << "Cove in-degree:    " << island.in_degree(cove)
          << "  (how many ferries arrive)\n";

std::cout << "\nEach ferry leaving Harbor:\n";
for (const auto eid : island.out_edges(harbor)) {
    const auto& ferry = island.get_edge(eid);
    std::cout << "  " << island.node_data(ferry.from)
              << " -> " << island.node_data(ferry.to)
              << "  " << ferry.data << " min\n";
}
```

`neighbors` and `out_edges` are lazy views: cheap, but **do not add or remove** edges while you iterate them. If you
must mutate, snapshot:

```cpp
const auto leaving = island.out_edge_ids(harbor);  // a real vector
```

Incoming ferries exist only because this graph is `Directed`:

```cpp
for (const auto eid : island.in_edges(lighthouse)) {
    const auto& ferry = island.get_edge(eid);
    std::cout << island.node_data(ferry.from) << " arrives at the light\n";
}
```

**What you just learned:** degree is “how busy is this dock.” Outgoing vs incoming is the difference between leaving and
arriving. Edge payload is `ferry.data` or `island.edge_data(eid)`.

**Try:** print every dock with `island.active_node_ids()` and its `out_degree`. Which dock is the busiest origin?

---

## Step 3 — The lost tourist (BFS) vs the cliff rambler (DFS)

**Goal:** two ways to *visit* every reachable place.

A tourist at Harbor wants the **fewest ferry hops**, not the fewest minutes. That is breadth-first search: wave 0 =
Harbor, wave 1 = everything one hop out, wave 2 = the next ring.

```cpp
std::cout << "\nTourist (BFS, fewest hops):\n";
litegraph::bfs(island, harbor, [](litegraph::NodeId, const std::string& name) {
    std::cout << "  visits " << name << "\n";
});
```

A rambler follows one route as far as it goes, then backtracks. That is depth-first search:

```cpp
std::cout << "Rambler (DFS, follow one chain):\n";
litegraph::dfs(island, harbor, [](litegraph::NodeId, const std::string& name) {
    std::cout << "  visits " << name << "\n";
});
```

Compare the two printouts. BFS should hit Market and Cove before Lighthouse. DFS may dive Harbor → Market → Cove →
Lighthouse before it ever lists the other Harbor ferry.

**What you just learned:** BFS = shortest *unweighted* path (hop count). DFS = “go deep, then backtrack.” Both take
`(NodeId, const payload&)`. They only visit nodes **reachable from the start**.

**Try:** start BFS at Cove. Who is missing? (Anyone who cannot be reached *from Cove* — Harbor and maybe Market.)

---

## Step 4 — The courier’s race (Dijkstra)

**Goal:** cheapest *weighted* path. Minutes matter now.

The scenic Harbor → Market → Lighthouse ferry is 12 + 40 = 52 minutes. Harbor → Cove → Lighthouse is 20 + 15 = 35.
Harbor → Market → Cove → Lighthouse is 12 + 8 + 15 = 35. Dijkstra finds the minimum.

```cpp
auto [minutes, came_from] = litegraph::dijkstra(
    island, harbor,
    [](const int& w) { return static_cast<double>(w); }
);

std::cout << "\nCourier times from Harbor:\n";
for (const auto nid : island.active_node_ids()) {
    std::cout << "  " << island.node_data(nid) << ": " << minutes[nid.value] << " min\n";
}

auto route = litegraph::reconstruct_path(lighthouse, came_from);
std::cout << "Fastest Harbor -> Lighthouse: ";
for (const auto nid : route) {
    std::cout << island.node_data(nid) << " ";
}
std::cout << "\n";
```

Expect Lighthouse at **35**. The path should include Cove, not the 40-minute scenic hop.

Notes that bite:

- `minutes` is indexed by `nid.value` and sized to `node_capacity()`, not `node_count()`.
- Unreachable docks stay at `+infinity`.
- `reconstruct_path(target, pred)` is empty if the target is unreachable, **or if the target is the start** (the start
  has no predecessor).
- Dijkstra needs **non-negative** weights. A “refund” edge of −5 minutes would be a job for `bellman_ford`.

**Try:** change the Market → Lighthouse ferry from 40 to 10. Re-run. Does the courier now take the scenic line?

---

## Step 5 — Festival weekend (a DAG and a cycle)

**Goal:** directed edges as *dependencies*, not boats.

The island festival is a graph of “this must finish before that.” That is a **DAG** (directed acyclic graph). Kahn’s
topological sort lines up the work.

Use a **new** graph so boats and schedules stay separate:

```cpp
litegraph::Graph<std::string, std::monostate, litegraph::Directed> fest;

const auto unload = fest.add_node("Unload barges");
const auto stage  = fest.add_node("Build stage");
const auto sound  = fest.add_node("Sound check");
const auto open   = fest.add_node("Open gates");
const auto fireworks = fest.add_node("Fireworks");

fest.add_edge(unload, stage);     // stage waits on barges
fest.add_edge(stage, sound);
fest.add_edge(sound, open);
fest.add_edge(open, fireworks);
fest.add_edge(unload, fireworks); // extra powder on the last barge

if (litegraph::has_cycle(fest)) {
    std::cout << "Festival is stuck in a loop!\n";
} else {
    std::cout << "\nFestival order:\n";
    for (const auto nid : litegraph::topological_sort(fest)) {
        std::cout << "  " << fest.node_data(nid) << "\n";
    }
}
```

You should see Unload before Stage before Sound before Open before Fireworks. Unload can also sit before Fireworks
without being immediately before it.

Now the plot twist — a clerk adds “Fireworks must finish before we unload.” That is a **cycle**. Sort cannot pick a
first task:

```cpp
fest.add_edge(fireworks, unload);

std::cout << std::boolalpha << "Cycle after the clerk? "
          << litegraph::has_cycle(fest) << "\n";

auto stuck = litegraph::topological_sort(fest);
std::cout << "Sorted steps: " << stuck.size() << " (empty means cycle)\n";
```

`topological_sort` returns an **empty vector** when a cycle exists. Check `has_cycle` first, or check that
`order.size() == fest.node_count()`.

**What you just learned:** the same `Graph` type models roads *or* “happens-before.” Cycles are illegal for schedules,
normal for ferry loops.

**Try:** remove the clerk’s edge (`fest.remove_edge` — keep the `EdgeId` when you `add_edge`) and sort again.

---

## Step 6 — The rumor mill (strongly connected components)

**Goal:** groups where everyone can reach everyone else.

Add a return ferry so rumors can travel in circles. Keep using `island`:

```cpp
island.add_edge(lighthouse, harbor, 25);  // night boat home
island.add_edge(cove, market, 9);         // local hop

auto clumps = litegraph::strongly_connected_components(island);
std::cout << "\nRumor clumps (" << clumps.size() << "):\n";
for (const auto& group : clumps) {
    std::cout << "  { ";
    for (const auto nid : group) {
        std::cout << island.node_data(nid) << " ";
    }
    std::cout << "}\n";
}
```

A **strongly connected component** is a set of docks where you can sail from any member to any other (maybe via several
hops) and back. After the night boat, Harbor, Market, Cove, and Lighthouse may collapse into one clump. Drop the night
boat and they split again.

**What you just learned:** “Can I get there?” is BFS. “Are we all in the same gossip circle?” is SCC. Directed-only.

**Try:** comment out `lighthouse → harbor` and re-run. How many clumps?

---

## Step 7 — Storm season (lazy delete and compact)

**Goal:** remove a dock without rewriting the whole map, then pack memory.

A storm wrecks Market. LiteGraph does not shuffle every ID on delete. It **marks** Market and its ferries inactive.
Fast, a little messy.

```cpp
std::cout << "\nBefore storm: nodes=" << island.node_count()
          << " capacity=" << island.node_capacity() << "\n";

island.remove_node(market);

std::cout << "After storm (logical): nodes=" << island.node_count()
          << " capacity=" << island.node_capacity() << "\n";
std::cout << "Market still a valid id? " << island.valid_node(market) << "\n";
```

`node_count()` dropped; `node_capacity()` did not. Harbor’s old `NodeId` is still Harbor. Market’s id is dead.
`active_node_ids()` skips wrecks.

When you want a packed map (no holes), compact. **Every old ID is then stale** — including Harbor’s.

```cpp
auto [new_nodes, new_edges] = island.compact();

litegraph::NodeId harbor2{};
if (new_nodes[harbor.value]) {
    harbor2 = litegraph::NodeId{*new_nodes[harbor.value]};
}
// lighthouse, cove: same remap. market maps to nullopt.

std::cout << "After compact: nodes=" << island.node_count()
          << " capacity=" << island.node_capacity() << "\n";
std::cout << "Remapped Harbor index: " << harbor2.value << "\n";
```

From here, use `harbor2` (and remapped Cove / Lighthouse). Do not call `island.node_data(harbor)`.

**What you just learned:** delete is O (1) marking. Compact rebuilds storage and returns old→new maps. This is the
physical vs logical split described in the reference.

**Try:** `island.get_stats()` and print `load_factor` before and after compact.

Rebuild a clean island if you compacted — copy Step 1’s nodes and edges into a fresh `Graph` — before Step 8, so
PageRank sees all four docks.

---

## Step 8 — Who runs the island? (PageRank)

**Goal:** freeze the editable map into CSR and score influence.

PageRank pretends a visitor randomly follows ferries (and sometimes teleports). Docks that many routes point at rank
higher. The algorithm wants a packed, immutable snapshot: **CSR** (compressed sparse row).

```cpp
// Fresh four-dock island from Step 1, including the night boat if you like.
auto csr = litegraph::freeze_to_csr(island);

litegraph::CsrPageRankOptions opts;
opts.damping_factor = 0.85;
opts.max_iterations = 100;
opts.tolerance = 1e-9;

auto fame = litegraph::pagerank(csr, opts);

std::cout << "\nFame (converged=" << fame.converged
          << ", iters=" << fame.iterations << "):\n";
for (std::size_t i = 0; i < csr.node_count(); ++i) {
    const auto orig = csr.original_node_id(i);
    std::cout << "  " << island.node_data(orig) << ": " << fame.ranks[i] << "\n";
}
```

CSR indices are `0 .. n-1`, not your original `NodeId`s if you ever left holes. Translate with
`csr.compact_index(harbor)` and `csr.original_node_id(i)`.

Pretty picture for the wall of the cartography office:

```cpp
litegraph::to_dot(island, std::cout);
// save to island.dot and run:  dot -Tsvg island.dot -o island.svg
```

**What you just learned:** mutable `Graph` is for editing; CSR is for bulk sweeps. PageRank answers “who is central?”
not “who is closest?”

**Try:** add a dozen ferries into Harbor only. Does Harbor’s rank jump?

---

## Step 9 — Side quests (pick one)

Same library, new questions. Each is a small extra graph.

### Walking trails (undirected MST)

Trails are two-way. A **minimum spanning tree** is the cheapest set of trails that still connects every lookout — no
loops, minimum total length.

```cpp
litegraph::WeightedUndirectedGraph trails;
const auto a = trails.add_node();
const auto b = trails.add_node();
const auto c = trails.add_node();
const auto d = trails.add_node();
trails.add_edge(a, b, 4.0);
trails.add_edge(a, c, 9.0);
trails.add_edge(b, c, 3.0);
trails.add_edge(b, d, 7.0);
trails.add_edge(c, d, 2.0);

auto tree = litegraph::kruskal_mst(trails, [](double w) { return w; });
double total = 0;
for (const auto eid : tree) {
    total += trails.edge_data(eid);
}
std::cout << "Trail network km: " << total << "  edges: " << tree.size() << "\n";
```

Expect total **9** (b–c 3, c–d 2, a–b 4). `prim_mst` should pick the same weight.

### Ferry capacity (max flow)

Treat edge data as **passengers per hour**. How many people can you push from Harbor to Lighthouse if every ferry is a
pipe?

```cpp
double people = litegraph::edmonds_karp_max_flow(
    island, harbor, lighthouse,
    [](const int& cap) { return static_cast<double>(cap); }
);
std::cout << "Peak Harbor -> Lighthouse flow: " << people << "\n";
```

(If you already stormed Market, rebuild Step 1 first.) Flow is limited by bottlenecks, not by the longest path.

### Color the lookouts

If two lookouts share a trail, they cannot use the same signal-flag color:

```cpp
auto colors = litegraph::greedy_graph_coloring(trails);
```

Not always the fewest colors (that problem is hard), but fast and good enough for flags.

---

## Step 10 — The Grand Regatta (stress-test the map)

**Goal:** generate a *huge* archipelago, print LiteGraph stats, and time the build.

Four docks were a postcard. Festival week, visiting fleets need a full chart: thousands of docks, tens of thousands of
ferries. `to_ascii` will drown you. `get_stats()` and the [profiler](../utils/profiler.md) will not.

Each dock gets an `int` label and `double` minute-weights. `batch_add_nodes` plants every dock in one go; then we
sprinkle random outbound ferries. Same seed ⇒ same map, so later races are fair.

```cpp
using Archipelago = litegraph::Graph<int, double, litegraph::Directed>;

Archipelago make_archipelago(std::size_t n, std::size_t out_deg, std::uint32_t seed) {
    Archipelago g;
    g.reserve_nodes(n);
    g.reserve_edges(n * out_deg);

    std::vector<int> labels(n);
    std::iota(labels.begin(), labels.end(), 0);
    g.batch_add_nodes(labels);  // NodeId{0} .. NodeId{n-1}

    std::mt19937 rng(seed);
    std::uniform_int_distribution<std::size_t> pick(0, n - 1);
    std::uniform_real_distribution<double> minutes(1.0, 25.0);

    for (std::size_t i = 0; i < n; ++i) {
        const litegraph::NodeId from{i};
        for (std::size_t k = 0; k < out_deg; ++k) {
            auto j = pick(rng);
            if (j == i) j = (j + 1) % n;
            g.add_edge(from, litegraph::NodeId{j}, minutes(rng));
        }
    }
    return g;
}

Archipelago ocean;
{
    PROFILE_SCOPE("Raise the archipelago");
    ocean = make_archipelago(4'000, 6, /*seed=*/42);
}

const auto stats = ocean.get_stats();
std::cout << "\nGrand Regatta chart\n"
          << "  active docks:  " << stats.active_nodes << "\n"
          << "  active ferries:" << stats.active_edges << "\n"
          << "  load factor:   " << stats.load_factor << "  (1.0 = no wrecks)\n"
          << "  ~bytes:        " << stats.memory_usage << "\n";
```

`PROFILE_SCOPE` (from `utils/profiler.hpp`) prints when the block ends — one-shot construction time, not a full
benchmark.

Start at **4 000 docks × 6 ferries** (~24 k edges). If that is instant, bump to `12'000` and `8`. If it crawls, drop to
`1'500`. The races below assume the graph is **read-only** after this: no `add_edge` / `remove_node` while threads are
running.

**What you just learned:** `reserve_*` + `batch_add_nodes` avoid realloc storms. `GraphStats` is the island’s census:
logical size vs allocated slots vs approximate RAM.

**Try:** call `make_archipelago` twice with the same seed and check `ocean.edge_count()` matches. Change the seed;
counts stay similar, paths will not.

---

## Step 11 — Two kinds of parallelism (census + many couriers)

**Goal:** see *algorithm* parallelism vs *benchmark* parallelism, and why they are not the same.

Imagine the cartography office on regatta morning.

| Kind                      | What runs in parallel                         | LiteGraph / profiler                                                          |
|---------------------------|-----------------------------------------------|-------------------------------------------------------------------------------|
| **One job, many clerks**  | Work *inside* a single BFS / Dijkstra / count | `std::execution::par` + `litegraph::parallel::*` or `parallel_count_nodes_if` |
| **Many jobs, many desks** | Independent repeats of the *same* query       | `profiler::ProfileConfig::parallelism`                                        |

Mixing them without thinking is how you hire two crews to paint one dinghy.

### Census: how many “hub” docks?

A hub is a dock whose label is divisible by 10. Serial `count_if` vs the graph’s parallel helper:

```cpp
const auto hubs_serial = std::count_if(
    ocean.active_node_ids().begin(), ocean.active_node_ids().end(),
    [&](litegraph::NodeId nid) { return ocean.node_data(nid) % 10 == 0; });

const auto hubs_par = ocean.parallel_count_nodes_if(
    std::execution::par,
    [](const auto& node) { return node.data % 10 == 0; });

std::cout << "Hub docks: serial=" << hubs_serial
          << "  parallel=" << hubs_par << "\n";
```

The predicate for `parallel_count_nodes_if` sees the **internal `Node`** (`node.data`, already required to be active).
Both counts must match.

### Level-synchronous BFS

`litegraph::parallel::parallel_bfs` visits one hop-ring at a time; visits on the **same** ring may run together. The
visitor must be thread-safe — use an atomic, not `int++`.

```cpp
std::atomic<std::size_t> reached{0};
litegraph::parallel::parallel_bfs(
    std::execution::par, ocean, litegraph::NodeId{0},
    [&](litegraph::NodeId, const int&) { reached.fetch_add(1, std::memory_order_relaxed); });
std::cout << "Docks reachable from 0: " << reached.load() << "\n";
```

### Many couriers at once (profiler threads)

The profiler can fire the **same** read-only query on several threads. That measures *throughput* (queries per second),
not “is Dijkstra itself parallel.” Keep `parallelism = 1` when you compare two algorithms fairly (next step).

```cpp
profiler::ProfileConfig desks;
desks.label = "Many serial couriers";
desks.iterations = 64;
desks.warmup_iterations = 8;
desks.parallelism = std::max(2u, std::thread::hardware_concurrency());
desks.trim_outliers_percentage = 10.0;
desks.output_unit = profiler::TimeUnit::Milliseconds;

const litegraph::NodeId start{0};
auto many = profiler::measure(desks, [&] {
    (void)litegraph::dijkstra(ocean, start);
});
std::cout << profiler::format_result(many) << "\n";
std::cout << "Threads used: " << many.parallelism_used
          << "  CV: " << many.coefficient_of_variation() << "\n";
if (many.is_bimodal()) {
    std::cout << "Bimodal timings — laptop turbo / other apps. More warmup helps.\n";
}
```

CV under ~0.15 is a calm race. Much higher, and the stopwatch is racing the OS, not Dijkstra.

**What you just learned:** `par` inside one algorithm can help *or* lose to thread overhead. Profiler `parallelism`
answers “how many tourists can we route at once?” on a **shared, immutable** graph.

**Try:** set `desks.parallelism = 1` and measure again. Throughput (iterations / total time) should drop. Wall time per
iteration may *improve* (no cache fighting).

---

## Step 12 — The Courier Cup (timed races + `compare`)

**Goal:** same start and finish, three skippers, one scoreboard.

Dock `0` is the harbor. Dock `n-1` is the far lighthouse. Everyone must report the **same** travel time or the race is
void.

```cpp
const litegraph::NodeId harbor{0};
const litegraph::NodeId far{ocean.node_count() - 1};

auto weight = [](const double& w) { return w; };

const auto [serial_dist, serial_pred] = litegraph::dijkstra(ocean, harbor, weight);
const auto bi = litegraph::bidirectional_dijkstra(ocean, harbor, far, weight);
const auto par_try = litegraph::parallel::parallel_dijkstra(
    std::execution::par, ocean, harbor, weight);

if (!par_try) {
    std::cerr << "parallel Dijkstra: bad source\n";
    return 1;
}
const auto& [par_dist, par_pred] = *par_try;

std::cout << "\nSanity (minutes to far light)\n"
          << "  serial Dijkstra:        " << serial_dist[far.value] << "\n"
          << "  bidirectional Dijkstra: " << bi.distance << "\n"
          << "  parallel Dijkstra:      " << par_dist[far.value] << "\n";
```

Bidirectional search is two parties walking toward each other (forward on out-edges, backward on in-edges). It is still
*one thread*, but often less work than flooding the whole ocean. Parallel Dijkstra parallelizes the *phases* of one
search (`litegraph::parallel`). It returns `std::expected` — invalid source ⇒ `GraphError::InvalidNode`.

Now time them. **One thread per measurement** so the comparison is algorithm vs algorithm, not “who hogged the cores.”

```cpp
auto cup = [](std::string name) {
    profiler::ProfileConfig c;
    c.label = std::move(name);
    c.iterations = 40;
    c.warmup_iterations = 6;
    c.parallelism = 1;
    c.trim_outliers_percentage = 10.0;  // needs >= 20 iterations
    c.output_unit = profiler::TimeUnit::Milliseconds;
    return c;
};

auto t_serial = profiler::measure(cup("Serial Dijkstra"), [&] {
    return litegraph::dijkstra(ocean, harbor, weight).first[far.value];
});
auto t_bi = profiler::measure(cup("Bidirectional Dijkstra"), [&] {
    return litegraph::bidirectional_dijkstra(ocean, harbor, far, weight).distance;
});
auto t_par = profiler::measure(cup("Parallel Dijkstra (par)"), [&] {
    return litegraph::parallel::parallel_dijkstra(
        std::execution::par, ocean, harbor, weight)->first[far.value];
});
auto t_seq_policy = profiler::measure(cup("Parallel Dijkstra (seq policy)"), [&] {
    return litegraph::parallel::parallel_dijkstra(
        std::execution::seq, ocean, harbor, weight)->first[far.value];
});

std::cout << profiler::format_result(t_serial);
std::cout << profiler::format_result(t_bi);
std::cout << profiler::format_result(t_par);
std::cout << profiler::format_result(t_seq_policy);

std::cout << profiler::format_comparison(profiler::compare(t_serial, t_bi));
std::cout << profiler::format_comparison(profiler::compare(t_serial, t_par));
std::cout << profiler::format_comparison(profiler::compare(t_par, t_seq_policy));
```

Read the scoreboard like a steward:

- `speedup_factor < 1` ⇒ the **candidate** (second argument to `compare`) is faster.
- `p < 0.05` ⇒ the difference is probably real, not noise (Mann–Whitney).
- Returning the distance from the lambda stops the compiler from deleting the race.

On 4 k docks you may see bidirectional win and `par` *lose* to serial. That is the lesson: parallel min-finding has
atomics and barriers; the island is not always big enough to pay for the crew. `seq` vs `par` on the *same*
`parallel_dijkstra` isolates policy cost.

**What you just learned:** correctness first (same minutes), then `measure` + `compare`. Two search frontiers ≠ two CPU
threads. More threads ≠ automatically faster.

**Try:** raise `n` to `12'000` and `out_deg` to `8`. Does `par` catch up? Time `litegraph::bfs` vs
`parallel::parallel_bfs` the same way (count visits with an atomic). Export `t_serial.to_chrome_trace()` and open it in
`chrome://tracing` if you like flame charts.

---

## Step 13 — SIMD fame vs scalar PageRank

**Goal:** freeze CSR once, then race scalar PageRank against the Highway boundary.

Fame on 4 k docks is a tight loop over ranks. That is SIMD country — *if* you compiled with Highway.

```cpp
const auto csr = litegraph::freeze_to_csr(ocean);

litegraph::CsrPageRankOptions pr_opts;
pr_opts.max_iterations = 80;
pr_opts.tolerance = 1e-9;

auto t_pr = profiler::measure(cup("Scalar PageRank"), [&] {
    return litegraph::pagerank(csr, pr_opts).iterations;
});
auto t_hwy = profiler::measure(cup("Highway PageRank"), [&] {
    return litegraph::highway::pagerank(csr, pr_opts).iterations;
});

std::cout << "Highway SIMD enabled? " << std::boolalpha
          << litegraph::highway::enabled() << "\n";
std::cout << profiler::format_result(t_pr);
std::cout << profiler::format_result(t_hwy);
std::cout << profiler::format_comparison(profiler::compare(t_pr, t_hwy));
```

Without `-DLITEGRAPH_ENABLE_HIGHWAY`, `highway::pagerank` still compiles and should match scalar times (`enabled()` is
false). With Highway on, expect the candidate to pull ahead on large CSR graphs — same ranks, fatter vector registers.

Sanity: run both once and check `ranks` are within `1e-8` (the Catch2 tests do this).

**What you just learned:** CSR is the frozen racetrack; Highway is an optional engine. Profiler `compare` is how you
prove an optimization, not a vibe.

**Try:** `cup("...").iterations = 20` is the minimum for outlier trim. For a serious gate, use 100+ iterations and fail
CI if `compare(baseline, candidate).speedup_factor > 1.1` *and* `is_significant` (regression). See [
`docs/utils/profiler.md`](../utils/profiler.md).

---

## You now know

| Idea                   | Island story       | LiteGraph                                               |
|------------------------|--------------------|---------------------------------------------------------|
| Node / edge            | Dock / ferry       | `add_node`, `add_edge`                                  |
| Directed               | One-way ferry      | `Directed` tag                                          |
| Degree                 | How busy a dock is | `out_degree` / `in_degree`                              |
| BFS                    | Fewest hops        | `bfs`                                                   |
| DFS                    | Follow a chain     | `dfs`                                                   |
| Weighted shortest path | Fastest courier    | `dijkstra` + `reconstruct_path`                         |
| DAG / cycle            | Festival order     | `topological_sort`, `has_cycle`                         |
| SCC                    | Rumor circles      | `strongly_connected_components`                         |
| Lazy delete            | Storm              | `remove_node`, then `compact`                           |
| Centrality             | Fame               | `freeze_to_csr` + `pagerank`                            |
| MST / flow             | Trails / capacity  | `kruskal_mst`, `edmonds_karp_max_flow`                  |
| Stress map             | Grand Regatta      | `batch_add_nodes`, `reserve_*`, `get_stats`             |
| Parallel count / BFS   | Census, hop rings  | `parallel_count_nodes_if`, `parallel::parallel_bfs`     |
| Parallel shortest path | Night-shift clerks | `parallel::parallel_dijkstra`, `bidirectional_dijkstra` |
| Timed race             | Courier Cup        | `profiler::measure` + `compare`                         |
| SIMD                   | Fame on CSR        | `litegraph::highway::pagerank`                          |

---

## Watch these rocks

1. Include `LiteGraphAlgorithms.hpp` for BFS, Dijkstra, ASCII, DOT, CSR, PageRank.
2. Do not mutate while iterating `out_edges` / `neighbors`. Snapshot with `out_edge_ids`.
3. After `compact()`, remap every stored `NodeId` / `EdgeId`.
4. `NodeId{i}` is explicit — no raw `size_t` where an id is required.
5. Dijkstra: non-negative weights only. Negative: `bellman_ford`.
6. `in_edges` / topological sort / SCC / max-flow / bidirectional Dijkstra need a **directed** graph. MST needs *
   *undirected**.
7. Stress-test **read-only** graphs under `profiler` `parallelism` — do not `add`/`remove` from several threads.
8. `parallel_bfs` visitors must be thread-safe (`std::atomic`, not `int++`).
9. `std::execution::par` can be *slower* than serial on small maps; believe `compare`, not the function name.
10. Outlier trimming needs at least **20** measured iterations.

---

## Keep exploring

- Full API: [`docs/containers/LiteGraph.md`](../containers/LiteGraph.md)
- A\* (heuristic courier), Bellman-Ford, Floyd-Warshall, VF2 pattern search, betweenness: same algorithms header
- SIMD PageRank: `LiteGraphHighway.hpp` and `litegraph::highway::enabled()`
- Profiler stats, `compare`, Chrome traces: [`docs/utils/profiler.md`](../utils/profiler.md)
- Dominator trees of a control-flow graph: `include/containers/graph/DominatorTree.hpp`

Add one more dock whenever you want. The map is yours. Race the couriers when it gets crowded.
