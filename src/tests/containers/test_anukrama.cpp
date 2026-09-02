#include "containers/anukrama/anukrama.hpp"

#include "catch_amalgamated.hpp"

#include <string>
#include <vector>

TEST_CASE (
"Anukrama: snapshots retain the version visible at acquisition"
,
"[anukrama][mvcc][snapshot]"
)
 {
    anukrama::store<std::string, std::string> values;
    REQUIRE(values.begin().put("theme", "light").commit().has_value());
    auto stable = values.snapshot_at_current();
    REQUIRE(values.begin().put("theme", "dark").commit().has_value());
    CHECK(stable.get("theme") == "light");
    CHECK(values.get("theme") == "dark");
}

TEST_CASE (
"Anukrama: concurrent same-key writers conflict"
,
"[anukrama][mvcc][conflict]"
)
 {
    anukrama::store<std::string, int> values;
    REQUIRE(values.begin().put("counter", 1).commit().has_value());
    auto first = values.begin();
    auto second = values.begin();
    first.put("counter", 2);
    second.put("counter", 3);
    REQUIRE(first.commit().has_value());
    CHECK_FALSE(second.commit().has_value());
    CHECK(values.get("counter") == 2);
}

TEST_CASE (
"Anukrama: tombstones retain historical visibility while a snapshot is live"
,
"[anukrama][mvcc][tombstone]"
)
 {
    anukrama::store<std::string, int> values;
    REQUIRE(values.begin().put("answer", 42).commit().has_value());
    auto before_delete = values.snapshot_at_current();
    REQUIRE(values.begin().erase("answer").commit().has_value());
    CHECK(before_delete.get("answer") == 42);
    CHECK_FALSE(values.get("answer").has_value());
    values.prune();
    CHECK(before_delete.get("answer") == 42);
}

TEST_CASE (
"Anukrama: point-read validation is an opt-in policy"
,
"[anukrama][mvcc][policy]"
)
 {
    using strict_store = anukrama::store<std::string, int, std::less<>,
                                         anukrama::skip_list_index,
                                         anukrama::atomic_clock,
                                         anukrama::optimistic_point_serializable>;
    strict_store values;
    REQUIRE(values.begin().put("a", 1).commit().has_value());
    auto reader = values.begin();
    REQUIRE(reader.get("a") == 1);
    REQUIRE(values.begin().put("a", 2).commit().has_value());
    reader.put("b", 3);
    CHECK_FALSE(reader.commit().has_value());
}

TEST_CASE (
"Anukrama: a transaction has one terminal commit"
,
"[anukrama][mvcc][transaction]"
)
 {
    anukrama::store<std::string, int> values;
    auto tx = values.begin();
    tx.put("once", 1);
    REQUIRE(tx.commit().has_value());
    const auto second_commit = tx.commit();
    REQUIRE_FALSE(second_commit.has_value());
    CHECK(second_commit.error() == anukrama::error::transaction_finished);
}

TEST_CASE (
"Anukrama: externally ordered durable commits and snapshot scans"
,
"[anukrama][mvcc][durable][scan]"
)
 {
    anukrama::store<std::string, int> values;
    std::vector<anukrama::store<std::string, int>::write> first{{"a", 1}, {"b", 2}};
    REQUIRE(values.apply_at(first, 100).has_value());
    auto snapshot = values.snapshot_at_current();
    std::vector<anukrama::store<std::string, int>::write> second{{"a", 3}};
    REQUIRE(values.apply_at(second, 200).has_value());

    std::vector<std::string> observed;
    snapshot.scan("a", "z", [&](const auto& key, const auto& value, const auto stamp) {
        observed.push_back(key + ":" + std::to_string(value) + ":" + std::to_string(stamp));
    });
    CHECK(observed == std::vector<std::string>{"a:1:100", "b:2:100"});
    CHECK_FALSE(values.apply_at(second, 100).has_value());
}

TEST_CASE (
"Anukrama: coordinator validation and publication share one critical section"
,
"[anukrama][mvcc][coordinator]"
)
 {
    anukrama::store<std::string, int> values;
    REQUIRE(values.begin().put("source", 1).commit().has_value());
    const auto observed = values.version_of("source");
    REQUIRE(values.begin().put("source", 2).commit().has_value());

    std::vector<anukrama::store<std::string, int>::observation> reads{{"source", observed}};
    std::vector<anukrama::store<std::string, int>::write> writes{{"derived", 3}};
    CHECK_FALSE(values.commit_if_unchanged(reads, writes).has_value());
    CHECK_FALSE(values.get("derived").has_value());

    reads[0].version = values.version_of("source");
    REQUIRE(values.commit_if_unchanged(reads, writes).has_value());
    CHECK(values.get("derived") == 3);
}

// --- Anukrama Next: additive policy coverage ---------------------------------

#include <atomic>
#include <thread>

TEST_CASE (
"anukrama: smriti_node_pool version parity"
,
"[anukrama][policy][smriti]"
)
 {
    using arena_store = anukrama::store<std::string, std::string, std::less<>,
                                        anukrama::skip_list_index,
                                        anukrama::atomic_clock,
                                        anukrama::snapshot_isolation,
                                        anukrama::smriti_node_pool>;

    // Same snapshot-visibility scenario as the heap default must read identically.
    anukrama::store<std::string, std::string> heap;
    arena_store arena;
    REQUIRE(heap.begin().put("theme", "light").commit().has_value());
    REQUIRE(arena.begin().put("theme", "light").commit().has_value());
    auto heap_stable = heap.snapshot_at_current();
    auto arena_stable = arena.snapshot_at_current();
    REQUIRE(heap.begin().put("theme", "dark").commit().has_value());
    REQUIRE(arena.begin().put("theme", "dark").commit().has_value());
    CHECK(heap_stable.get("theme") == arena_stable.get("theme"));
    CHECK(heap.get("theme") == arena.get("theme"));

    // Churn + prune: reclaimed nodes must be recycled, so repeated overwrite of a
    // single key stays bounded rather than allocating without bound. Fresh store so
    // size() reflects only the churned key.
    arena_store churn;
    for (int i = 0; i < 1000; ++i)
        REQUIRE(churn.begin().put("k", std::to_string(i)).commit().has_value());
    churn.prune();
    CHECK(churn.get("k") == "999");
    CHECK(churn.size() == 1U);
    for (int i = 1000; i < 2000; ++i)
        REQUIRE(churn.begin().put("k", std::to_string(i)).commit().has_value());
    churn.prune();
    CHECK(churn.get("k") == "1999");
    CHECK(churn.size() == 1U);
}

// Current striped_lock contract: correctness-first locking for commit/publish paths.
// Disjoint-key parallel commit is intentionally conservative until the backing index
// supports concurrent structural mutation without global commit serialization.
TEST_CASE (
"anukrama: striped_lock disjoint-key concurrency"
,
"[anukrama][concurrency]"
)
 {
    using striped_store = anukrama::store<int, int, std::less<>,
                                          anukrama::skip_list_index,
                                          anukrama::atomic_clock,
                                          anukrama::snapshot_isolation,
                                          anukrama::heap_node_pool,
                                          anukrama::striped_lock<64>>;
    striped_store values;

    // Disjoint keys committed from many threads all succeed.
    constexpr int threads = 8;
    std::atomic<int> succeeded{0};
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t)
        pool.emplace_back([&values, &succeeded, t] {
            if (values.begin().put(t, t * 10).commit().has_value())
                succeeded.fetch_add(1, std::memory_order_relaxed);
        });
    for (auto& th : pool) th.join();
    CHECK(succeeded.load() == threads);
    CHECK(values.size() == static_cast<std::size_t>(threads));

    // Same-key conflict: first-writer-wins, exactly one survives.
    REQUIRE(values.begin().put(100, 1).commit().has_value());
    auto first = values.begin();
    auto second = values.begin();
    first.put(100, 2);
    second.put(100, 3);
    REQUIRE(first.commit().has_value());
    CHECK_FALSE(second.commit().has_value());
    CHECK(values.get(100) == 2);
}

TEST_CASE (
"anukrama: prune retains min-active-visible version"
,
"[anukrama][prune]"
)
 {
    anukrama::store<std::string, int> values;
    REQUIRE(values.begin().put("k", 1).commit().has_value());

    auto early = values.snapshot_at_current();     // sees version 1
    REQUIRE(values.begin().put("k", 2).commit().has_value());
    auto late = values.snapshot_at_current();      // sees version 2
    REQUIRE(values.begin().put("k", 3).commit().has_value());

    // With both snapshots live, prune must retain everything the oldest sees.
    values.prune();
    CHECK(early.get("k") == 1);
    CHECK(late.get("k") == 2);
    CHECK(values.get("k") == 3);

    // Close the oldest: version 1 is now unreachable and may be reclaimed.
    { auto drop = std::move(early); }
    values.prune();
    CHECK(late.get("k") == 2);
    CHECK(values.get("k") == 3);

    // All snapshots closed: prune collapses to the latest.
    { auto drop = std::move(late); }
    values.prune();
    CHECK(values.get("k") == 3);
}
