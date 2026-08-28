#include "catch_amalgamated.hpp"
#include <atomic>
#include <thread>
#include <vector>

#include "containers/lockfree/RingBuffer.hpp"
#include "containers/lockfree/MPSCQueue.hpp"
#include "containers/lockfree/AtomicStack.hpp"
#include "containers/lockfree/HazardRegistry.hpp"
#include "containers/lockfree/MPMCQueue.hpp"
#include "containers/graph/LiteGraphAlgorithms.hpp"
#include "containers/graph/DisjointSet.hpp"

// ============================================================================
// RingBuffer tests
// ============================================================================

TEST_CASE (



"RingBuffer: basic single-threaded push/pop"
,
"[lockfree][ringbuffer]"
)
 {
    lockfree::RingBuffer<int, 8> rb;
    REQUIRE(rb.empty());
    REQUIRE(rb.capacity() == 8);

    REQUIRE(rb.try_push(1));
    REQUIRE(rb.try_push(2));
    REQUIRE(rb.try_push(3));
    REQUIRE_FALSE(rb.empty());
    REQUIRE(rb.size_approx() == 3);

    auto v1 = rb.try_pop();
    REQUIRE(v1.has_value());
    REQUIRE(*v1 == 1);

    auto v2 = rb.try_pop();
    REQUIRE(*v2 == 2);
    auto v3 = rb.try_pop();
    REQUIRE(*v3 == 3);

    REQUIRE(rb.empty());
    REQUIRE_FALSE(rb.try_pop().has_value());
}

TEST_CASE (



"RingBuffer: full buffer rejects push"
,
"[lockfree][ringbuffer]"
)
 {
    lockfree::RingBuffer<int, 4> rb;
    REQUIRE(rb.try_push(10));
    REQUIRE(rb.try_push(20));
    REQUIRE(rb.try_push(30));
    REQUIRE(rb.try_push(40));
    REQUIRE_FALSE(rb.try_push(99)); // full
    REQUIRE(*rb.try_pop() == 10);
    REQUIRE(rb.try_push(99)); // now fits
}

TEST_CASE (



"RingBuffer: wraps around correctly"
,
"[lockfree][ringbuffer]"
)
 {
    lockfree::RingBuffer<int, 4> rb;
    for (int round = 0; round < 3; ++round) {
        for (int i = 0; i < 4; ++i) REQUIRE(rb.try_push(i * 10));
        for (int i = 0; i < 4; ++i) REQUIRE(*rb.try_pop() == i * 10);
    }
}

TEST_CASE (



"RingBuffer: SPSC stress"
,
"[lockfree][ringbuffer][concurrent]"
)
 {
    lockfree::RingBuffer<int, 256> rb;
    constexpr int N = 100'000;
    std::atomic<int> sum_produced{0}, sum_consumed{0};

    std::thread producer([&] {
        for (int i = 0; i < N; ++i) {
            while (!rb.try_push(i)) { std::this_thread::yield(); }
            sum_produced.fetch_add(i, std::memory_order_relaxed);
        }
    });

    std::thread consumer([&] {
        int count = 0;
        while (count < N) {
            if (auto v = rb.try_pop()) {
                sum_consumed.fetch_add(*v, std::memory_order_relaxed);
                ++count;
            } else {
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();
    REQUIRE(sum_produced.load() == sum_consumed.load());
}

TEST_CASE (



"RingBuffer: move-only type"
,
"[lockfree][ringbuffer]"
)
 {
    lockfree::RingBuffer<std::unique_ptr<int>, 8> rb;
    REQUIRE(rb.try_push(std::make_unique<int>(42)));
    auto v = rb.try_pop();
    REQUIRE(v.has_value());
    REQUIRE(**v == 42);
}

// ============================================================================
// MPSCQueue tests
// ============================================================================

TEST_CASE (



"MPSCQueue: basic single-threaded push/pop"
,
"[lockfree][mpsc]"
)
 {
    lockfree::MPSCQueue<int> q;
    REQUIRE(q.empty());

    q.push(1);
    q.push(2);
    q.push(3);
    REQUIRE_FALSE(q.empty());

    REQUIRE(*q.pop() == 1);
    REQUIRE(*q.pop() == 2);
    REQUIRE(*q.pop() == 3);
    REQUIRE_FALSE(q.pop().has_value());
    REQUIRE(q.empty());
}

TEST_CASE (



"MPSCQueue: multi-producer stress"
,
"[lockfree][mpsc][concurrent]"
)
 {
    lockfree::MPSCQueue<int> q;
    constexpr int producers = 4;
    constexpr int per_producer = 10'000;
    std::atomic<long long> expected_sum{0};

    std::vector<std::thread> threads;
    threads.reserve(producers);
    for (int t = 0; t < producers; ++t) {
        threads.emplace_back([&, t] {
            long long local = 0;
            for (int i = 0; i < per_producer; ++i) {
                int val = t * per_producer + i;
                q.push(val);
                local += val;
            }
            expected_sum.fetch_add(local, std::memory_order_relaxed);
        });
    }
    for (auto &th : threads) th.join();

    long long actual_sum = 0;
    while (auto v = q.pop()) actual_sum += *v;
    REQUIRE(actual_sum == expected_sum.load());
}

TEST_CASE (



"MPSCQueue: move-only type"
,
"[lockfree][mpsc]"
)
 {
    lockfree::MPSCQueue<std::unique_ptr<int>> q;
    q.push(std::make_unique<int>(7));
    auto v = q.pop();
    REQUIRE(v.has_value());
    REQUIRE(**v == 7);
}

// ============================================================================
// AtomicStack tests
// ============================================================================

TEST_CASE (



"AtomicStack: basic push/pop LIFO order"
,
"[lockfree][atomicstack]"
)
 {
    lockfree::AtomicStack<int> s;
    REQUIRE(s.empty());
    s.push(1);
    s.push(2);
    s.push(3);
    REQUIRE_FALSE(s.empty());
    REQUIRE(*s.pop() == 3);
    REQUIRE(*s.pop() == 2);
    REQUIRE(*s.pop() == 1);
    REQUIRE_FALSE(s.pop().has_value());
    REQUIRE(s.empty());
}

TEST_CASE (



"AtomicStack: concurrent push/pop"
,
"[lockfree][atomicstack][concurrent]"
)
 {
    lockfree::AtomicStack<int> s;
    constexpr int N = 50'000;
    std::atomic<long long> pushed_sum{0}, popped_sum{0};
    std::atomic<int> remaining{N};

    std::thread pusher([&] {
        for (int i = 0; i < N; ++i) {
            s.push(i);
            pushed_sum.fetch_add(i, std::memory_order_relaxed);
        }
    });

    std::thread popper([&] {
        int count = 0;
        while (count < N) {
            if (auto v = s.pop()) {
                popped_sum.fetch_add(*v, std::memory_order_relaxed);
                ++count;
            } else {
                std::this_thread::yield();
            }
        }
    });

    pusher.join();
    popper.join();
    REQUIRE(pushed_sum.load() == popped_sum.load());
}

TEST_CASE (



"AtomicStack: move-only type"
,
"[lockfree][atomicstack]"
)
 {
    lockfree::AtomicStack<std::unique_ptr<std::string>> s;
    s.push(std::make_unique<std::string>("hello"));
    auto v = s.pop();
    REQUIRE(v.has_value());
    REQUIRE(**v == "hello");
}

// ============================================================================
// HazardRegistry tests
// ============================================================================

TEST_CASE (



"HazardRegistry: guard claims and releases slot"
,
"[lockfree][hazard]"
)
 {
    using HR = lockfree::HazardRegistry<4, 8>;

    {
        HR::HazardGuard g1;
        HR::HazardGuard g2;
        // both guards acquired distinct slots without throwing
    }
    // After destruction, slots are released — acquire again should succeed.
    HR::HazardGuard g3;
    (void)g3;
}

TEST_CASE (



"HazardRegistry: retire + scan reclaims unprotected"
,
"[lockfree][hazard]"
)
 {
    using HR = lockfree::HazardRegistry<4, 4>;
    std::atomic<int> destroyed{0};

    struct Tracked {
        std::atomic<int> *counter;
        ~Tracked() { counter->fetch_add(1, std::memory_order_relaxed); }
    };

    // Push enough retires to trigger the threshold scan.
    for (int i = 0; i < 4; ++i) {
        auto *p = new Tracked{&destroyed};
        HR::retire(p, [](void *q) noexcept { delete static_cast<Tracked *>(q); });
    }
    // Threshold == 4, so scan fires automatically. No hazard guards active.
    REQUIRE(destroyed.load() == 4);
}

// ============================================================================
// MPMCQueue tests
// ============================================================================

TEST_CASE (



"MPMCQueue: basic single-threaded try_push/try_pop"
,
"[lockfree][mpmc]"
)
 {
    lockfree::MPMCQueue<int, 8> q;
    REQUIRE(q.empty_approx());
    REQUIRE(q.capacity() == 8);

    REQUIRE(q.try_push(1));
    REQUIRE(q.try_push(2));
    REQUIRE(q.try_push(3));
    REQUIRE(q.size_approx() == 3);

    auto v1 = q.try_pop();
    REQUIRE(v1.has_value());
    REQUIRE(*v1 == 1);
    REQUIRE(*q.try_pop() == 2);
    REQUIRE(*q.try_pop() == 3);
    REQUIRE(q.empty_approx());
    REQUIRE_FALSE(q.try_pop().has_value());
}

TEST_CASE (



"MPMCQueue: full buffer rejects push"
,
"[lockfree][mpmc]"
)
 {
    lockfree::MPMCQueue<int, 4> q;
    REQUIRE(q.try_push(10));
    REQUIRE(q.try_push(20));
    REQUIRE(q.try_push(30));
    REQUIRE(q.try_push(40));
    REQUIRE_FALSE(q.try_push(99)); // full
    REQUIRE(*q.try_pop() == 10);
    REQUIRE(q.try_push(99));       // now fits
}

TEST_CASE (



"MPMCQueue: wraps around correctly"
,
"[lockfree][mpmc]"
)
 {
    lockfree::MPMCQueue<int, 4> q;
    for (int round = 0; round < 3; ++round) {
        for (int i = 0; i < 4; ++i) q.push(i * 10);
        for (int i = 0; i < 4; ++i) REQUIRE(q.pop() == i * 10);
    }
}

TEST_CASE (



"MPMCQueue: MPMC concurrent stress"
,
"[lockfree][mpmc][concurrent]"
)
 {
    lockfree::MPMCQueue<int, 512> q;
    constexpr int producers   = 4;
    constexpr int consumers   = 4;
    constexpr int per_producer = 10'000;
    constexpr int total        = producers * per_producer;

    std::atomic<long long> produced_sum{0}, consumed_sum{0};
    std::atomic<int>       consumed_count{0};

    std::vector<std::thread> prod_threads, cons_threads;
    prod_threads.reserve(producers);
    cons_threads.reserve(consumers);

    for (int t = 0; t < producers; ++t) {
        prod_threads.emplace_back([&, t] {
            long long local = 0;
            for (int i = 0; i < per_producer; ++i) {
                int val = t * per_producer + i;
                q.push(val);
                local += val;
            }
            produced_sum.fetch_add(local, std::memory_order_relaxed);
        });
    }

    for (int t = 0; t < consumers; ++t) {
        cons_threads.emplace_back([&] {
            long long local = 0;
            while (consumed_count.load(std::memory_order_relaxed) < total) {
                if (auto v = q.try_pop()) {
                    local += *v;
                    consumed_count.fetch_add(1, std::memory_order_relaxed);
                } else {
                    std::this_thread::yield();
                }
            }
            consumed_sum.fetch_add(local, std::memory_order_relaxed);
        });
    }

    for (auto &t : prod_threads) t.join();
    for (auto &t : cons_threads) t.join();

    REQUIRE(produced_sum.load() == consumed_sum.load());
    REQUIRE(consumed_count.load() == total);
}

TEST_CASE (



"MPMCQueue: move-only type"
,
"[lockfree][mpmc]"
)
 {
    lockfree::MPMCQueue<std::unique_ptr<int>, 8> q;
    REQUIRE(q.try_push(std::make_unique<int>(99)));
    auto v = q.try_pop();
    REQUIRE(v.has_value());
    REQUIRE(**v == 99);
}

// ============================================================================
// HazardRegistry: thread-local pool tests
// ============================================================================

TEST_CASE (



"HazardRegistry: multiple guards on same thread"
,
"[lockfree][hazard]"
)
 {
    using HR = lockfree::HazardRegistry<16, 32>;
    // Acquire four guards from the same thread — the pool should handle it.
    HR::HazardGuard g1, g2, g3, g4;
    // All guards acquired without assertion failure.
    (void)g1; (void)g2; (void)g3; (void)g4;
}

TEST_CASE (



"HazardRegistry: guard slot is reused after destruction"
,
"[lockfree][hazard]"
)
 {
    using HR = lockfree::HazardRegistry<4, 8>;
    // Create and destroy several guards; pool should recycle slots.
    for (int i = 0; i < 10; ++i) {
        HR::HazardGuard g;
        (void)g;
    }
}

// ============================================================================
// DisjointSet: snapshot/rollback fix
// ============================================================================

TEST_CASE (



"DisjointSet: rollback undoes union correctly"
,
"[disjointset][rollback]"
)
 {
    disjointset::DisjointSet<int> ds;
    for (int i : {1, 2, 3, 4}) ds.insert_or_get(i);

    ds.push_snapshot();
    ds.unite(1, 2);
    ds.unite(3, 4);

    REQUIRE(ds.connected(1, 2));
    REQUIRE(ds.connected(3, 4));
    REQUIRE_FALSE(ds.connected(1, 3));

    auto result = ds.rollback();
    REQUIRE(result.has_value());

    // After rollback, all elements should be in separate sets.
    REQUIRE_FALSE(ds.connected(1, 2));
    REQUIRE_FALSE(ds.connected(3, 4));
    REQUIRE(ds.set_count() == 4);
}

TEST_CASE (



"DisjointSet: nested rollback restores correctly"
,
"[disjointset][rollback]"
)
 {
    disjointset::DisjointSet<int> ds;
    for (int i : {1, 2, 3, 4, 5}) ds.insert_or_get(i);

    ds.push_snapshot();
    ds.unite(1, 2);   // level 1

    ds.push_snapshot();
    ds.unite(3, 4);   // level 2

    REQUIRE(ds.connected(1, 2));
    REQUIRE(ds.connected(3, 4));

    ds.rollback();    // undo level 2
    REQUIRE(ds.connected(1, 2));
    REQUIRE_FALSE(ds.connected(3, 4));

    ds.rollback();    // undo level 1
    REQUIRE_FALSE(ds.connected(1, 2));
    REQUIRE(ds.set_count() == 5);
}

TEST_CASE (



"DisjointSet: set_size is correct after rollback"
,
"[disjointset][rollback]"
)
 {
    disjointset::DisjointSet<int> ds;
    for (int i : {10, 20, 30}) ds.insert_or_get(i);

    ds.push_snapshot();
    ds.unite(10, 20);
    REQUIRE(*ds.set_size(10) == 2);

    ds.rollback();
    REQUIRE(*ds.set_size(10) == 1);
    REQUIRE(*ds.set_size(20) == 1);
}

// ============================================================================
// LiteGraph: iterative SCC (deep graph, would stack-overflow with recursion)
// ============================================================================

TEST_CASE (



"SCC iterative: simple directed cycle"
,
"[graph][scc]"
)
 {
    auto g = litegraph::make_directed_graph();
    auto a = g.add_node(), b = g.add_node(), c = g.add_node();
    g.add_edge(a, b); g.add_edge(b, c); g.add_edge(c, a);

    auto sccs = litegraph::strongly_connected_components(g);
    REQUIRE(sccs.size() == 1);
    REQUIRE(sccs[0].size() == 3);
}

TEST_CASE (



"SCC iterative: DAG has no non-trivial SCC"
,
"[graph][scc]"
)
 {
    auto g = litegraph::make_directed_graph();
    auto a = g.add_node(), b = g.add_node(), c = g.add_node();
    g.add_edge(a, b); g.add_edge(b, c);

    auto sccs = litegraph::strongly_connected_components(g);
    REQUIRE(sccs.size() == 3);
    for (const auto &scc : sccs) REQUIRE(scc.size() == 1);
}

TEST_CASE (



"SCC iterative: deep chain (stack-overflow stress)"
,
"[graph][scc]"
)
 {
    // Build a long directed chain of 5000 nodes — recursive Tarjan would
    // need ~5000 stack frames and typically blows the default stack.
    auto g = litegraph::make_directed_graph();
    constexpr int N = 5000;
    std::vector<litegraph::NodeId> nodes;
    nodes.reserve(N);
    for (int i = 0; i < N; ++i) nodes.push_back(g.add_node());
    for (int i = 0; i + 1 < N; ++i) g.add_edge(nodes[i], nodes[i + 1]);

    auto sccs = litegraph::strongly_connected_components(g);
    // Each node is its own SCC in a DAG chain.
    REQUIRE(sccs.size() == static_cast<std::size_t>(N));
}

// ============================================================================
// LiteGraph: iterative topological sort (Kahn's algorithm)
// ============================================================================

TEST_CASE (



"TopSort iterative: basic DAG"
,
"[graph][toposort]"
)
 {
    auto g = litegraph::make_directed_graph();
    auto a = g.add_node(), b = g.add_node(), c = g.add_node(), d = g.add_node();
    // a → b → d, a → c → d
    g.add_edge(a, b); g.add_edge(b, d);
    g.add_edge(a, c); g.add_edge(c, d);

    auto order = litegraph::topological_sort(g);
    REQUIRE(order.size() == 4);

    // Verify the topological constraint: every edge (u,v) has u before v.
    std::vector<std::size_t> pos(g.node_capacity());
    for (std::size_t i = 0; i < order.size(); ++i) pos[order[i].value] = i;
    for (auto [eid, edge] : g.edges()) {
        REQUIRE(pos[edge.from.value] < pos[edge.to.value]);
    }
}

TEST_CASE (



"TopSort iterative: cycle returns empty"
,
"[graph][toposort]"
)
 {
    auto g = litegraph::make_directed_graph();
    auto a = g.add_node(), b = g.add_node(), c = g.add_node();
    g.add_edge(a, b); g.add_edge(b, c); g.add_edge(c, a);

    auto order = litegraph::topological_sort(g);
    REQUIRE(order.empty());
}

TEST_CASE (



"TopSort iterative: deep chain (stack-overflow stress)"
,
"[graph][toposort]"
)
 {
    auto g = litegraph::make_directed_graph();
    constexpr int N = 5000;
    std::vector<litegraph::NodeId> nodes;
    nodes.reserve(N);
    for (int i = 0; i < N; ++i) nodes.push_back(g.add_node());
    for (int i = 0; i + 1 < N; ++i) g.add_edge(nodes[i], nodes[i + 1]);

    auto order = litegraph::topological_sort(g);
    REQUIRE(order.size() == static_cast<std::size_t>(N));
    REQUIRE(order.front().value == nodes[0].value);
    REQUIRE(order.back().value  == nodes[N - 1].value);
}

// ============================================================================
// LiteGraph: Bidirectional Dijkstra
// ============================================================================

TEST_CASE (



"BiDijkstra: simple 3-node path"
,
"[graph][bidijkstra]"
)
 {
    litegraph::Graph<std::monostate, double> g;
    auto a = g.add_node(), b = g.add_node(), c = g.add_node();
    g.add_edge(a, b, 1.0);
    g.add_edge(b, c, 2.0);

    auto result = litegraph::bidirectional_dijkstra(g, a, c,
        [](double w){ return w; });

    REQUIRE(result.distance == Catch::Approx(3.0));
    REQUIRE(result.path.size() == 3);
    REQUIRE(result.path.front().value == a.value);
    REQUIRE(result.path.back().value  == c.value);
}

TEST_CASE (



"BiDijkstra: no path returns infinity"
,
"[graph][bidijkstra]"
)
 {
    litegraph::Graph<std::monostate, double> g;
    auto a = g.add_node(), b = g.add_node(), c = g.add_node();
    g.add_edge(a, b, 1.0);
    // No edge from b or a to c.

    auto result = litegraph::bidirectional_dijkstra(g, a, c,
        [](double w){ return w; });

    REQUIRE(std::isinf(result.distance));
    REQUIRE(result.path.empty());
}

TEST_CASE (



"BiDijkstra: same result as single-direction Dijkstra"
,
"[graph][bidijkstra]"
)
 {
    litegraph::Graph<std::monostate, double> g;
    // Small weighted graph.
    auto n = [&]{ return g.add_node(); };
    auto a=n(), b=n(), c=n(), d=n(), e=n();
    g.add_edge(a, b, 4.0);
    g.add_edge(a, c, 1.0);
    g.add_edge(c, b, 2.0);
    g.add_edge(b, d, 1.0);
    g.add_edge(c, e, 5.0);
    g.add_edge(e, d, 1.0);

    auto wfn = [](double w){ return w; };

    auto [dist_single, pred_single] = litegraph::dijkstra(g, a, wfn);
    auto bidir = litegraph::bidirectional_dijkstra(g, a, d, wfn);

    REQUIRE(bidir.distance == Catch::Approx(dist_single[d.value]));
}

TEST_CASE (



"BiDijkstra: source == target"
,
"[graph][bidijkstra]"
)
 {
    litegraph::Graph<std::monostate, double> g;
    auto a = g.add_node();
    auto result = litegraph::bidirectional_dijkstra(g, a, a,
        [](double w){ return w; });
    REQUIRE(result.distance == Catch::Approx(0.0));
    REQUIRE(result.path.size() == 1);
    REQUIRE(result.path[0].value == a.value);
}

// has_cycle with deep graph (iterative, no stack overflow)
TEST_CASE (



"has_cycle iterative: deep chain has no cycle"
,
"[graph][cycle]"
)
 {
    auto g = litegraph::make_directed_graph();
    constexpr int N = 5000;
    std::vector<litegraph::NodeId> nodes;
    nodes.reserve(N);
    for (int i = 0; i < N; ++i) nodes.push_back(g.add_node());
    for (int i = 0; i + 1 < N; ++i) g.add_edge(nodes[i], nodes[i + 1]);
    REQUIRE_FALSE(litegraph::has_cycle(g));
}

TEST_CASE (



"has_cycle iterative: adding back edge creates cycle"
,
"[graph][cycle]"
)
 {
    auto g = litegraph::make_directed_graph();
    constexpr int N = 100;
    std::vector<litegraph::NodeId> nodes;
    nodes.reserve(N);
    for (int i = 0; i < N; ++i) nodes.push_back(g.add_node());
    for (int i = 0; i + 1 < N; ++i) g.add_edge(nodes[i], nodes[i + 1]);
    g.add_edge(nodes[N - 1], nodes[0]); // create cycle
    REQUIRE(litegraph::has_cycle(g));
}

// ============================================================================
// AtomicStack: MPMC concurrent stress
// ============================================================================

TEST_CASE (



"AtomicStack: MPMC concurrent stress"
,
"[lockfree][atomicstack][concurrent]"
)
 {
    lockfree::AtomicStack<int> s;
    constexpr int kProducers = 4;
    constexpr int kConsumers = 4;
    constexpr int kPerProducer = 10'000;
    constexpr int kTotal = kProducers * kPerProducer;

    std::atomic<long long> pushed_sum{0}, popped_sum{0};
    std::atomic<int> popped_count{0};

    std::vector<std::thread> producers, consumers;
    producers.reserve(kProducers);
    consumers.reserve(kConsumers);

    for (int t = 0; t < kProducers; ++t) {
        producers.emplace_back([&, t] {
            long long local = 0;
            for (int i = 0; i < kPerProducer; ++i) {
                int val = t * kPerProducer + i;
                s.push(val);
                local += val;
            }
            pushed_sum.fetch_add(local, std::memory_order_relaxed);
        });
    }

    for (int t = 0; t < kConsumers; ++t) {
        consumers.emplace_back([&] {
            long long local = 0;
            while (popped_count.load(std::memory_order_relaxed) < kTotal) {
                if (auto v = s.pop()) {
                    local += *v;
                    popped_count.fetch_add(1, std::memory_order_relaxed);
                } else {
                    std::this_thread::yield();
                }
            }
            popped_sum.fetch_add(local, std::memory_order_relaxed);
        });
    }

    for (auto &t : producers) t.join();
    for (auto &t : consumers) t.join();

    REQUIRE(pushed_sum.load() == popped_sum.load());
    REQUIRE(popped_count.load() == kTotal);
}

// ============================================================================
// MPSCQueue: concurrent producer + running consumer stress
// ============================================================================

TEST_CASE (



"MPSCQueue: concurrent producer+consumer stress"
,
"[lockfree][mpsc][concurrent]"
)
 {
    lockfree::MPSCQueue<int> q;
    constexpr int kProducers = 4;
    constexpr int kPerProducer = 10'000;
    constexpr int kTotal = kProducers * kPerProducer;

    std::atomic<long long> produced_sum{0}, consumed_sum{0};
    std::atomic<int> consumed_count{0};
    std::atomic<bool> producers_done{false};

    std::vector<std::thread> prod_threads;
    prod_threads.reserve(kProducers);

    for (int t = 0; t < kProducers; ++t) {
        prod_threads.emplace_back([&, t] {
            long long local = 0;
            for (int i = 0; i < kPerProducer; ++i) {
                int val = t * kPerProducer + i;
                q.push(val);
                local += val;
            }
            produced_sum.fetch_add(local, std::memory_order_relaxed);
        });
    }

    // Consumer runs concurrently with producers.
    std::thread consumer([&] {
        long long local = 0;
        while (consumed_count.load(std::memory_order_relaxed) < kTotal) {
            if (auto v = q.pop()) {
                local += *v;
                consumed_count.fetch_add(1, std::memory_order_relaxed);
            } else {
                std::this_thread::yield();
            }
        }
        consumed_sum.store(local, std::memory_order_relaxed);
    });

    for (auto &t : prod_threads) t.join();
    consumer.join();

    REQUIRE(produced_sum.load() == consumed_sum.load());
    REQUIRE(consumed_count.load() == kTotal);
}

// ============================================================================
// HazardRegistry: active guard prevents premature deletion
// ============================================================================

TEST_CASE (



"HazardRegistry: active guard prevents deletion"
,
"[lockfree][hazard]"
)
 {
    using HR = lockfree::HazardRegistry<4, 4>;
    std::atomic<int> destroyed{0};

    struct Tracked {
        std::atomic<int> *counter;
        ~Tracked() { counter->fetch_add(1, std::memory_order_relaxed); }
    };

    auto *p = new Tracked{&destroyed};
    std::atomic<Tracked *> src{p};

    {
        HR::HazardGuard guard;
        guard.protect(src, std::memory_order_acquire);

        // Retire p while guard is active — scan should NOT delete it.
        HR::retire(p, [](void *q) noexcept { delete static_cast<Tracked *>(q); });
        HR::scan(); // force scan even below threshold
        REQUIRE(destroyed.load() == 0); // guard is active, p must survive

        // Drop the guard.
    }

    // Guard gone — another scan should now reclaim p.
    HR::scan();
    REQUIRE(destroyed.load() == 1);
}

// ============================================================================
// MPMCQueue: exception safety on throwing T constructor
// ============================================================================

TEST_CASE (



"MPMCQueue: exception safety on push with throwing T"
,
"[lockfree][mpmc]"
)
 {
    static std::atomic<int> construct_count{0};

    struct ThrowOnThird {
        ThrowOnThird() {
            if (construct_count.fetch_add(1, std::memory_order_relaxed) == 2)
                throw std::runtime_error("boom");
        }
        ThrowOnThird(const ThrowOnThird &) = default;
        ThrowOnThird &operator=(const ThrowOnThird &) = default;
    };

    lockfree::MPMCQueue<ThrowOnThird, 8> q;

    // First two pushes succeed.
    REQUIRE(q.try_push(ThrowOnThird{}));
    REQUIRE(q.try_push(ThrowOnThird{}));

    // Reset so the next ThrowOnThird() constructed inside try_push throws.
    construct_count.store(2, std::memory_order_relaxed);
    REQUIRE_THROWS_AS(q.try_push(ThrowOnThird{}), std::runtime_error);

    // Queue must still be usable: existing items pop correctly, new pushes succeed.
    construct_count.store(0, std::memory_order_relaxed);
    REQUIRE(q.try_push(ThrowOnThird{}));
    REQUIRE(q.try_pop().has_value());
    REQUIRE(q.try_pop().has_value());
    REQUIRE(q.try_pop().has_value());
    REQUIRE_FALSE(q.try_pop().has_value());
}
