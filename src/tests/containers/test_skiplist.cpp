// ============================================================================
// test_skiplist.cpp — Unit tests for containers::SkipList
// ============================================================================

#include "catch_amalgamated.hpp"
#include "containers/associative/SkipList.hpp"

#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

TEST_CASE (
"containers::SkipList: Basic insertion, search, and deletion"
,
"[containers][skiplist]"
)
 {
    containers::SkipList<std::string, int> list;
    CHECK(list.empty());
    CHECK(list.size() == 0);

    auto [it1, inserted1] = list.insert_or_assign("apple", 10);
    REQUIRE(inserted1);
    CHECK(it1->first == "apple");
    CHECK(it1->second == 10);

    auto [it2, inserted2] = list.insert_or_assign("banana", 20);
    REQUIRE(inserted2);

    auto [it3, inserted3] = list.insert_or_assign("cherry", 30);
    REQUIRE(inserted3);

    CHECK(list.size() == 3);
    CHECK(list.contains("banana"));
    CHECK_FALSE(list.contains("durian"));

    auto found = list.find("banana");
    REQUIRE(found != list.end());
    CHECK(found->second == 20);

    // Overwrite
    auto [it4, inserted4] = list.insert_or_assign("banana", 25);
    CHECK_FALSE(inserted4);
    CHECK(list.find("banana")->second == 25);
    CHECK(list.size() == 3);

    // Erase
    CHECK(list.erase("banana"));
    CHECK_FALSE(list.contains("banana"));
    CHECK(list.size() == 2);
    CHECK_FALSE(list.erase("banana"));
}

TEST_CASE (
"containers::SkipList: lower_bound and range iteration"
,
"[containers][skiplist]"
)
 {
    containers::SkipList<int, std::string> list;

    for (int i = 10; i <= 100; i += 10) {
        list.insert_or_assign(i, "val_" + std::to_string(i));
    }

    std::vector<int> collected;
    for (auto it = list.lower_bound(30); it != list.end() && it->first < 80; ++it) {
        collected.push_back(it->first);
    }

    REQUIRE(collected == std::vector<int>{30, 40, 50, 60, 70});
}

TEST_CASE (
"containers::SkipList: upper_bound, equal_range, and count"
,
"[containers][skiplist]"
)
 {
    containers::SkipList<int, std::string> list;

    for (int i = 10; i <= 50; i += 10) {
        list.insert_or_assign(i, "v" + std::to_string(i));
    }

    auto ub = list.upper_bound(20);
    REQUIRE(ub != list.end());
    CHECK(ub->first == 30);

    auto [lb2, ub2] = list.equal_range(20);
    REQUIRE(lb2 != list.end());
    REQUIRE(ub2 != list.end());
    CHECK(lb2->first == 20);
    CHECK(ub2->first == 30);

    CHECK(list.count(20) == 1);
    CHECK(list.count(25) == 0);
}

TEST_CASE (
"containers::SkipList: map-like insertion APIs"
,
"[containers][skiplist]"
)
 {
    containers::SkipList<std::string, int> list;

    auto [i1, ok1] = list.insert(std::pair<const std::string, int>{"alpha", 1});
    REQUIRE(ok1);
    CHECK(i1->first == "alpha");
    CHECK(i1->second == 1);

    auto [i2, ok2] = list.emplace("beta", 2);
    REQUIRE(ok2);
    CHECK(i2->first == "beta");

    auto [i3, ok3] = list.try_emplace("beta", 999);
    CHECK_FALSE(ok3);
    CHECK(i3->second == 2);
}

TEST_CASE (
"containers::SkipList: at and operator[]"
,
"[containers][skiplist]"
)
 {
    containers::SkipList<std::string, int> list;

    list["x"] = 10;
    CHECK(list.at("x") == 10);

    list["x"] = 20;
    CHECK(list.at("x") == 20);

    CHECK_THROWS_AS(list.at("missing"), std::out_of_range);
}

TEST_CASE (
"containers::SkipList: erase by iterator and range"
,
"[containers][skiplist]"
)
 {
    containers::SkipList<int, int> list;
    for (int i = 1; i <= 6; ++i) {
        list.insert_or_assign(i, i * 10);
    }

    auto it = list.find(3);
    REQUIRE(it != list.end());
    auto next = list.erase(it);
    REQUIRE(next != list.end());
    CHECK(next->first == 4);
    CHECK_FALSE(list.contains(3));

    auto first = list.lower_bound(4);
    auto last = list.lower_bound(6);
    list.erase(first, last); // erase 4,5

    CHECK_FALSE(list.contains(4));
    CHECK_FALSE(list.contains(5));
    CHECK(list.contains(6));
}

TEST_CASE (
"containers::SkipList: copy constructor and assignment are deep"
,
"[containers][skiplist]"
)
 {
    containers::SkipList<std::string, int> a;
    a.insert_or_assign("a", 1);
    a.insert_or_assign("b", 2);

    containers::SkipList<std::string, int> b{a};
    REQUIRE(b.size() == a.size());
    CHECK(b.at("a") == 1);
    CHECK(b.at("b") == 2);

    b.insert_or_assign("c", 3);
    CHECK_FALSE(a.contains("c"));

    containers::SkipList<std::string, int> c;
    c = a;
    REQUIRE(c.size() == a.size());
    c.insert_or_assign("d", 4);
    CHECK_FALSE(a.contains("d"));
}

TEST_CASE (
"containers::SkipList: transparent erase with string_view"
,
"[containers][skiplist]"
)
 {
    containers::SkipList<std::string, int> list;
    list.insert_or_assign("alpha", 1);
    list.insert_or_assign("beta", 2);

    std::string_view key = "beta";
    CHECK(list.erase(key));
    CHECK_FALSE(list.contains("beta"));
    CHECK_FALSE(list.erase(key));
}

TEST_CASE (
"containers::SkipList: PMR polymorphic allocator support"
,
"[containers][skiplist][pmr]"
)
 {
    std::array<std::byte, 64 * 1024> buffer{};
    std::pmr::monotonic_buffer_resource pool{buffer.data(), buffer.size()};

    containers::pmr::SkipList<int, std::string> list{&pool};
    CHECK(list.empty());

    for (int i = 1; i <= 100; ++i) {
        list.insert_or_assign(i, "val_" + std::to_string(i));
    }

    CHECK(list.size() == 100);
    CHECK(list.contains(50));
    CHECK(list.at(50) == "val_50");

    list.erase(50);
    CHECK_FALSE(list.contains(50));
    CHECK(list.size() == 99);
}

TEST_CASE (
"containers::SkipList: Differential property test against std::map"
,
"[containers][skiplist][property]"
)
 {
    containers::SkipList<int, int> skip;
    std::map<int, int> stdmap;

    std::uint64_t state = 0xDEADBEEFCAFE1234ULL;
    auto next_rand = [&state](int limit) -> int {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return static_cast<int>(state % static_cast<std::uint64_t>(limit));
    };

    constexpr int kOperations = 50000;
    for (int op = 0; op < kOperations; ++op) {
        int action = next_rand(4);
        int k = next_rand(1000);
        int v = next_rand(10000);

        if (action == 0 || action == 1) {
            // Insert / assign
            auto [s_it, s_ok] = skip.insert_or_assign(k, v);
            auto [m_it, m_ok] = stdmap.insert_or_assign(k, v);
            REQUIRE(s_it->first == m_it->first);
            REQUIRE(s_it->second == m_it->second);
        } else if (action == 2) {
            // Erase
            bool s_erased = skip.erase(k);
            bool m_erased = (stdmap.erase(k) > 0);
            REQUIRE(s_erased == m_erased);
        } else {
            // Lookup & lower_bound
            auto s_find = skip.find(k);
            auto m_find = stdmap.find(k);
            REQUIRE((s_find != skip.end()) == (m_find != stdmap.end()));
            if (s_find != skip.end()) {
                REQUIRE(s_find->second == m_find->second);
            }

            auto s_lb = skip.lower_bound(k);
            auto m_lb = stdmap.lower_bound(k);
            REQUIRE((s_lb != skip.end()) == (m_lb != stdmap.end()));
            if (s_lb != skip.end()) {
                REQUIRE(s_lb->first == m_lb->first);
                REQUIRE(s_lb->second == m_lb->second);
            }
        }
    }

    REQUIRE(skip.size() == stdmap.size());

    // Verify in-order full scan equivalence
    auto s_it = skip.begin();
    auto m_it = stdmap.begin();
    while (s_it != skip.end() && m_it != stdmap.end()) {
        REQUIRE(s_it->first == m_it->first);
        REQUIRE(s_it->second == m_it->second);
        ++s_it;
        ++m_it;
    }
    REQUIRE(s_it == skip.end());
    REQUIRE(m_it == stdmap.end());
}


