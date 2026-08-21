#include "containers/anukrama/anukrama.hpp"

#include "catch_amalgamated.hpp"

#include <string>
#include <vector>

TEST_CASE("Anukrama: snapshots retain the version visible at acquisition", "[anukrama][mvcc][snapshot]") {
    anukrama::store<std::string, std::string> values;
    REQUIRE(values.begin().put("theme", "light").commit().has_value());
    auto stable = values.snapshot_at_current();
    REQUIRE(values.begin().put("theme", "dark").commit().has_value());
    CHECK(stable.get("theme") == "light");
    CHECK(values.get("theme") == "dark");
}

TEST_CASE("Anukrama: concurrent same-key writers conflict", "[anukrama][mvcc][conflict]") {
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

TEST_CASE("Anukrama: tombstones retain historical visibility while a snapshot is live", "[anukrama][mvcc][tombstone]") {
    anukrama::store<std::string, int> values;
    REQUIRE(values.begin().put("answer", 42).commit().has_value());
    auto before_delete = values.snapshot_at_current();
    REQUIRE(values.begin().erase("answer").commit().has_value());
    CHECK(before_delete.get("answer") == 42);
    CHECK_FALSE(values.get("answer").has_value());
    values.prune();
    CHECK(before_delete.get("answer") == 42);
}

TEST_CASE("Anukrama: point-read validation is an opt-in policy", "[anukrama][mvcc][policy]") {
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

TEST_CASE("Anukrama: a transaction has one terminal commit", "[anukrama][mvcc][transaction]") {
    anukrama::store<std::string, int> values;
    auto tx = values.begin();
    tx.put("once", 1);
    REQUIRE(tx.commit().has_value());
    CHECK(tx.commit().error() == anukrama::error::transaction_finished);
}

TEST_CASE("Anukrama: externally ordered durable commits and snapshot scans", "[anukrama][mvcc][durable][scan]") {
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

TEST_CASE("Anukrama: coordinator validation and publication share one critical section", "[anukrama][mvcc][coordinator]") {
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
