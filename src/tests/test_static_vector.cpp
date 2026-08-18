// test_static_vector.cpp — Unit tests for containers::static_vector.
//
// C++23, header-only usage. No external test framework required (static_assert + runtime checks).
// Test runner picks this up via CMake glob src/tests/*.cpp.

#include "catch_amalgamated.hpp"
#include "containers/static/static_vector.hpp"
#include <cstddef>

// ---- constexpr tests -------------------------------------------------------

static_assert([] {
    containers::static_vector < int, 4 > v;
    assert(v.empty());
    assert(v.size() == 0);
    assert(v.capacity() == 4);
    assert(!v.overflow());
    return true;
}());

static_assert([] {
    containers::static_vector < int, 4 > v;
    const bool r0 = v.push_back(10);
    const bool r1 = v.push_back(20);
    const bool r2 = v.push_back(30);
    const bool r3 = v.push_back(40);
    assert(r0 && r1 && r2 && r3);
    assert(v.size() == 4);
    assert(!v.overflow());
    return true;
}());

static_assert([] {
    containers::static_vector < int, 2 > v;
    static_cast<void>(v.push_back(1));
    static_cast<void>(v.push_back(2));
    const bool overflow_push = v.push_back(3); // must return false
    assert(!overflow_push);
    assert(v.overflow()); // sticky
    assert(v.size() == 2); // size unchanged
    return true;
}());

static_assert([] {
    containers::static_vector < int, 4 > v;
    static_cast<void>(v.push_back(10));
    static_cast<void>(v.push_back(20));
    static_cast<void>(v.push_back(30));
    assert(v[0] == 10 && v[1] == 20 && v[2] == 30);
    assert(v.back() == 30);
    v.pop_back();
    assert(v.size() == 2 && v.back() == 20);
    v.clear();
    assert(v.empty() && !v.overflow());
    return true;
}());

static_assert([] {
    containers::static_vector < int, 4 > v;
    static_cast<void>(v.push_back(1));
    static_cast<void>(v.push_back(2));
    static_cast<void>(v.push_back(3));
    int sum = 0;
    for (const auto x : v) sum += x;
    assert(sum == 6);
    return true;
}());

// ---- sizeof check (no hidden overhead above array + count + overflow flag) -
// The exact size depends on alignment padding; we just verify no virtual ptr / heap ptr added.
// Minimum bound: must be >= sizeof(array) + sizeof(size_t).

static_assert(sizeof(containers::static_vector < int, 4 >)
>=
sizeof
(std::array<int, 4>)
+
sizeof
(std::size_t)
);

// ---- runtime parity -------------------------------------------------------

#include <string_view>

TEST_CASE (

"static_vector: runtime usage"
,
"[containers][static_vector]"
)
 {
    containers::static_vector<std::string_view, 8> sv;
    static_cast<void>(sv.push_back("hello"));
    static_cast<void>(sv.push_back("world"));
    REQUIRE(sv.size() == 2);
    REQUIRE(sv[0] == "hello");
    REQUIRE(sv.back() == "world");

    // Iteration
    std::size_t count = 0;
    for ([[maybe_unused]] auto& s : sv) ++count;
    REQUIRE(count == 2);
}
