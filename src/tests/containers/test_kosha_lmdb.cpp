// ============================================================================
// test_kosha_lmdb.cpp — Tests for kosha LMDBAdapter
// ============================================================================
#if __has_include(<lmdb.h>)
#include "catch_amalgamated.hpp"
#include "containers/cache/adapters/lmdb.hpp"

#include <chrono>
#include <filesystem>
#include <string>

namespace {
    // RAII helper: creates a unique tmp directory and removes it on destruction.
    struct TmpLMDB {
        std::string path;

        TmpLMDB() {
            auto ns = std::chrono::steady_clock::now().time_since_epoch().count();
            path = (std::filesystem::temp_directory_path() /
                ("kosha_lmdb_test_" + std::to_string(ns))).string();
        }

        ~TmpLMDB() {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    };

    using StrCache = kosha::LRUCache<std::string, std::string>;
    using Adapter = kosha::adapter::LMDBAdapter<StrCache>;
} // anonymous namespace

// ============================================================================
// Tests
// ============================================================================

TEST_CASE ("LMDBAdapter: put then get from cache", "[lmdb]") {
    TmpLMDB tmp;
    Adapter a{tmp.path, StrCache{16}};

    REQUIRE(a.put("key1", "val1").has_value());
    auto r = a.get("key1");
    REQUIRE(r.has_value());
    CHECK(*r == "val1");
}

TEST_CASE ("LMDBAdapter: get after cache clear reads from LMDB", "[lmdb]") {
    TmpLMDB tmp;
    Adapter a{tmp.path, StrCache{16}};

    REQUIRE(a.put("key2", "val2").has_value());
    a.clear(); // evict from in-memory cache
    CHECK(a.size() == 0);

    auto r = a.get("key2");
    REQUIRE(r.has_value());
    CHECK(*r == "val2");
    CHECK(a.size() == 1); // re-populated from LMDB
}

TEST_CASE ("LMDBAdapter: erase removes from cache and LMDB", "[lmdb]") {
    TmpLMDB tmp;
    Adapter a{tmp.path, StrCache{16}};

    REQUIRE(a.put("key3", "val3").has_value());
    REQUIRE(a.erase("key3").has_value());
    a.clear(); // ensure cache miss forces LMDB lookup

    auto r = a.get("key3");
    CHECK_FALSE(r.has_value());
    CHECK(r.error() == kosha::adapter::LMDBError::NotFound);
}

TEST_CASE ("LMDBAdapter: load_all warms cache", "[lmdb]") {
    TmpLMDB tmp;

    {
        Adapter a{tmp.path, StrCache{32}};
        for (int i = 0; i < 10; ++i)
            REQUIRE(a.put(std::to_string(i), "v" + std::to_string(i)).has_value());
    }

    // New adapter, empty in-memory cache.
    Adapter b{tmp.path, StrCache{32}};
    CHECK(b.size() == 0);
    std::size_t loaded = b.load_all(10);
    CHECK(loaded == 10);
    CHECK(b.size() == 10);

    for (int i = 0; i < 10; ++i) {
        auto r = b.get(std::to_string(i));
        REQUIRE(r.has_value());
        CHECK(*r == "v" + std::to_string(i));
    }
}

TEST_CASE ("LMDBAdapter: persistence across instances", "[lmdb]") {
    TmpLMDB tmp;

    {
        Adapter a{tmp.path, StrCache{8}};
        REQUIRE(a.put("persist_key", "persist_val").has_value());
    } // adapter a destroyed, env closed

    Adapter b{tmp.path, StrCache{8}};
    auto r = b.get("persist_key"); // must read from LMDB
    REQUIRE(r.has_value());
    CHECK(*r == "persist_val");
}

TEST_CASE ("LMDBAdapter: LRU eviction still works with persistence", "[lmdb]") {
    TmpLMDB tmp;
    Adapter a{tmp.path, StrCache{3}}; // cap=3

    for (int i = 0; i < 5; ++i)
        REQUIRE(a.put(std::to_string(i), "v" + std::to_string(i)).has_value());

    // In-memory cache holds at most 3; LMDB holds all 5.
    CHECK(a.size() <= 3);

    // Evicted keys should still be retrievable from LMDB.
    int db_hits = 0;
    for (int i = 0; i < 5; ++i) {
        auto r = a.get(std::to_string(i));
        if (r.has_value()) ++db_hits;
    }
    CHECK(db_hits == 5);
}

TEST_CASE ("LMDBAdapter: clear_all empties both cache and LMDB", "[lmdb]") {
    TmpLMDB tmp;
    Adapter a{tmp.path, StrCache{8}};

    REQUIRE(a.put("k1", "v1").has_value());
    REQUIRE(a.put("k2", "v2").has_value());
    REQUIRE(a.clear_all().has_value());

    CHECK(a.size() == 0);
    CHECK_FALSE(a.get("k1").has_value());
    CHECK_FALSE(a.get("k2").has_value());
}

TEST_CASE ("LMDBAdapter: custom serializer support", "[lmdb]") {
    struct IntSerializer {
        static std::string serialize_key(int k) { return std::to_string(k); }
        static std::string serialize_value(int v) { return std::to_string(v); }
        static int deserialize_key(std::string_view sv) { return std::stoi(std::string(sv)); }
        static int deserialize_value(std::string_view sv) { return std::stoi(std::string(sv)); }
    };

    TmpLMDB tmp;
    using IntCache = kosha::LRUCache<int, int>;
    using IntAdapter = kosha::adapter::LMDBAdapter<IntCache, IntSerializer>;

    IntAdapter a{tmp.path, IntCache{4}};
    REQUIRE(a.put(42, 1337).has_value());
    a.clear();

    auto r = a.get(42);
    REQUIRE(r.has_value());
    CHECK(*r == 1337);
}

#endif // __has_include(<lmdb.h>)
