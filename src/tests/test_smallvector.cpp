// ============================================================================
// test_smallvector.cpp — Tests for SmallVector
// ============================================================================
// Covers: byte-budget formula, inline/spill paths, growth, emplace_back,
//         NonTrivial dtor correctness, move-only T, copy/move semantics,
//         reserve/shrink_to_fit, resize, erase, insert, swap, comparison,
//         SmritiAllocator integration, std::allocator default, stress
// ============================================================================

#include "catch_amalgamated.hpp"

#include "containers/dynamic/SmallVector.hpp"
#include "mem/smriti.hpp"
#include "mem/arena.hpp"

#include <deque>
#include <memory>
#include <ranges>
#include <stdexcept>
#include <string>

using namespace containers::dynamic;
using namespace smriti;

// ============================================================================
// Helpers
// ============================================================================

struct NonTrivial {
    int value;
    bool* destroyed;
    NonTrivial(int v, bool* d) : value{v}, destroyed{d} {}
    ~NonTrivial() { if (destroyed) *destroyed = true; }
    NonTrivial(const NonTrivial& o) : value{o.value}, destroyed{o.destroyed} {}
    NonTrivial(NonTrivial&& o) noexcept : value{o.value}, destroyed{o.destroyed} { o.destroyed = nullptr; }
    NonTrivial& operator=(const NonTrivial&) = default;

    NonTrivial& operator=(NonTrivial&& o) noexcept {
        value = o.value;
        destroyed = o.destroyed;
        o.destroyed = nullptr;
        return *this;
    }
};

struct MoveOnly {
    int value;
    explicit MoveOnly(int v) : value{v} {}
    MoveOnly(const MoveOnly&) = delete;
    MoveOnly& operator=(const MoveOnly&) = delete;
    MoveOnly(MoveOnly&& o) noexcept : value{o.value} { o.value = -1; }

    MoveOnly& operator=(MoveOnly&& o) noexcept {
        value = o.value;
        o.value = -1;
        return *this;
    }
};

using DefaultResource = ManagedResource<domains::SystemRAMDomain,
                                        pools::BumpPool<domains::SystemRAMDomain>>;

// ============================================================================
// Compile-time capacity formula
// ============================================================================

TEST_CASE (



"SmallVector: kInlineCap byte-budget formula"
,
"[smallvector][concepts]"
)
 {
    static_assert(SmallVector<int, 64>::kInlineCap == 16);
    static_assert(SmallVector<double, 64>::kInlineCap == 8);
    static_assert(SmallVector<char, 32>::kInlineCap == 32);
    static_assert(SmallVector<int, 2>::kInlineCap == 0); // T > budget → heap-only
    static_assert(SmallVector<int, 4>::kInlineCap == 1); // exact fit
    SUCCEED("byte-budget formula correct");
}

// ============================================================================
// Inline path
// ============================================================================

TEST_CASE (



"SmallVector: inline push_back stays in inline storage"
,
"[smallvector]"
)
 {
    SmallVector<int, 64> v;
    REQUIRE(v.capacity() == 16);
    CHECK(v.empty());

    for (int i = 0; i < 16; ++i) v.push_back(i);
    REQUIRE(v.size() == 16);
    CHECK_FALSE(v.spilled());

    for (int i = 0; i < 16; ++i)
        CHECK(v[i] == i);
}

TEST_CASE (



"SmallVector: inline emplace_back returns correct ref"
,
"[smallvector]"
)
 {
    SmallVector<int, 64> v;
    int& r = v.emplace_back(42);
    CHECK(r == 42);
    r = 99;
    CHECK(v[0] == 99);
}

TEST_CASE (



"SmallVector: front/back/data in inline mode"
,
"[smallvector]"
)
 {
    SmallVector<int, 64> v;
    v.push_back(10);
    v.push_back(20);
    v.push_back(30);
    CHECK(v.front() == 10);
    CHECK(v.back() == 30);
    CHECK(v.data() == v.begin());
}

// ============================================================================
// Spill path
// ============================================================================

TEST_CASE (



"SmallVector: spills to heap when inline capacity exceeded"
,
"[smallvector]"
)
 {
    SmallVector<int, 16> v; // kInlineCap = 4
    REQUIRE(SmallVector<int, 16>::kInlineCap == 4);

    for (int i = 0; i < 4; ++i) v.push_back(i);
    CHECK_FALSE(v.spilled());

    v.push_back(4);
    CHECK(v.spilled());
    REQUIRE(v.size() == 5);
    for (int i = 0; i < 5; ++i)
        CHECK(v[i] == i);
}

TEST_CASE (



"SmallVector: heap-only when T larger than InlineBytes"
,
"[smallvector]"
)
 {
    SmallVector<int, 2> v;
    CHECK(SmallVector<int, 2>::kInlineCap == 0);
    v.push_back(1);
    CHECK(v.spilled());
    v.push_back(2);
    CHECK(v[0] == 1);
    CHECK(v[1] == 2);
}

// ============================================================================
// Growth
// ============================================================================

TEST_CASE (



"SmallVector: growth keeps all values intact"
,
"[smallvector][stress]"
)
 {
    SmallVector<int, 16> v; // kInlineCap = 4
    constexpr int N = 1000;
    for (int i = 0; i < N; ++i) v.push_back(i);
    REQUIRE(v.size() == N);
    for (int i = 0; i < N; ++i)
        CHECK(v[i] == i);
}

TEST_CASE (



"SmallVector: capacity grows monotonically"
,
"[smallvector]"
)
 {
    SmallVector<int, 16> v;
    std::size_t last_cap = v.capacity();
    for (int i = 0; i < 128; ++i) {
        v.push_back(i);
        CHECK(v.capacity() >= last_cap);
        last_cap = v.capacity();
    }
}

// ============================================================================
// NonTrivial dtor correctness
// ============================================================================

TEST_CASE (



"SmallVector: dtors called on clear"
,
"[smallvector]"
)
 {
    int destroyed = 0;
    struct Probe {
        int* counter;
        explicit Probe(int* c) : counter{c} {}
        ~Probe() { if (counter) ++(*counter); }
        Probe(const Probe& o) : counter{o.counter} {}
        Probe(Probe&& o) noexcept : counter{o.counter} { o.counter = nullptr; }
    };
    {
        SmallVector<Probe, 64> v;
        v.emplace_back(&destroyed);
        v.emplace_back(&destroyed);
        v.emplace_back(&destroyed);
        CHECK(destroyed == 0);
        v.clear();
        CHECK(destroyed == 3);
    }
    CHECK(destroyed == 3);
}

TEST_CASE (



"SmallVector: dtors called on pop_back"
,
"[smallvector]"
)
 {
    bool d1 = false, d2 = false;
    {
        SmallVector<NonTrivial, 64> v;
        v.emplace_back(1, &d1);
        v.emplace_back(2, &d2);
        v.pop_back();
        CHECK(d2);
        CHECK_FALSE(d1);
    }
    CHECK(d1);
}

TEST_CASE (



"SmallVector: dtors called on vector destruction (inline)"
,
"[smallvector]"
)
 {
    bool d1 = false, d2 = false;
    {
        SmallVector<NonTrivial, 64> v;
        v.emplace_back(1, &d1);
        v.emplace_back(2, &d2);
        CHECK_FALSE(v.spilled());
    }
    CHECK(d1);
    CHECK(d2);
}

TEST_CASE (



"SmallVector: dtors called on vector destruction (spilled)"
,
"[smallvector]"
)
 {
    bool flags[8] = {};
    {
        SmallVector<NonTrivial, 8> v; // NonTrivial ~16 bytes → kInlineCap = 0 or 1
        for (int i = 0; i < 8; ++i) v.emplace_back(i, &flags[i]);
    }
    for (bool f : flags)
        CHECK(f);
}

// ============================================================================
// Move-only T
// ============================================================================

TEST_CASE (



"SmallVector: move-only T (std::unique_ptr)"
,
"[smallvector]"
)
 {
    SmallVector<std::unique_ptr<int>, 64> v;
    for (int i = 0; i < 20; ++i)
        v.push_back(std::make_unique<int>(i));

    REQUIRE(v.size() == 20);
    for (int i = 0; i < 20; ++i)
        CHECK(*v[i] == i);
}

TEST_CASE (



"SmallVector: move-only T via emplace_back"
,
"[smallvector]"
)
 {
    SmallVector<MoveOnly, 32> v;
    for (int i = 0; i < 10; ++i) v.emplace_back(i);
    REQUIRE(v.size() == 10);
    for (int i = 0; i < 10; ++i)
        CHECK(v[i].value == i);
}

// ============================================================================
// Copy ctor / copy assign
// ============================================================================

TEST_CASE (



"SmallVector: copy constructor produces independent deep copy"
,
"[smallvector]"
)
 {
    SmallVector<int, 64> a;
    for (int i = 0; i < 8; ++i) a.push_back(i);

    SmallVector b{a};
    REQUIRE(b.size() == 8);
    for (int i = 0; i < 8; ++i)
        CHECK(b[i] == i);

    b[0] = 999;
    CHECK(a[0] == 0);
}

TEST_CASE (



"SmallVector: copy constructor (spilled src)"
,
"[smallvector]"
)
 {
    SmallVector<int, 16> a; // kInlineCap = 4
    for (int i = 0; i < 20; ++i) a.push_back(i);
    CHECK(a.spilled());

    SmallVector b{a};
    REQUIRE(b.size() == 20);
    for (int i = 0; i < 20; ++i)
        CHECK(b[i] == i);
}

TEST_CASE (



"SmallVector: copy assignment deep copies"
,
"[smallvector]"
)
 {
    SmallVector<int, 64> a, b;
    for (int i = 0; i < 5; ++i) a.push_back(i * 2);
    b = a;
    REQUIRE(b.size() == 5);
    for (int i = 0; i < 5; ++i)
        CHECK(b[i] == i * 2);
    b[0] = -1;
    CHECK(a[0] == 0);
}

// ============================================================================
// Move ctor / move assign
// ============================================================================

TEST_CASE (



"SmallVector: move constructor steals heap pointer"
,
"[smallvector]"
)
 {
    SmallVector<int, 16> a; // kInlineCap = 4
    for (int i = 0; i < 20; ++i) a.push_back(i);
    CHECK(a.spilled());

    SmallVector b{std::move(a)};
    CHECK(a.empty());
    REQUIRE(b.size() == 20);
    for (int i = 0; i < 20; ++i)
        CHECK(b[i] == i);
}

TEST_CASE (



"SmallVector: move constructor moves inline elements"
,
"[smallvector]"
)
 {
    SmallVector<int, 64> a;
    for (int i = 0; i < 4; ++i) a.push_back(i);
    CHECK_FALSE(a.spilled());

    SmallVector b{std::move(a)};
    CHECK(a.empty());
    REQUIRE(b.size() == 4);
    for (int i = 0; i < 4; ++i)
        CHECK(b[i] == i);
}

TEST_CASE (



"SmallVector: move assignment"
,
"[smallvector]"
)
 {
    SmallVector<int, 16> a, b;
    for (int i = 0; i < 10; ++i) a.push_back(i);
    b = std::move(a);
    CHECK(a.empty());
    REQUIRE(b.size() == 10);
    for (int i = 0; i < 10; ++i)
        CHECK(b[i] == i);
}

// ============================================================================
// reserve / shrink_to_fit
// ============================================================================

TEST_CASE (



"SmallVector: reserve pre-allocates without constructing"
,
"[smallvector]"
)
 {
    SmallVector<int, 64> v;
    v.reserve(100);
    CHECK(v.capacity() >= 100);
    CHECK(v.empty());
}

TEST_CASE (



"SmallVector: reserve below current capacity is no-op"
,
"[smallvector]"
)
 {
    SmallVector<int, 64> v;
    v.reserve(100);
    std::size_t cap = v.capacity();
    v.reserve(50);
    CHECK(v.capacity() == cap);
}

TEST_CASE (



"SmallVector: shrink_to_fit collapses back to inline"
,
"[smallvector]"
)
 {
    SmallVector<int, 64> v; // kInlineCap = 16
    for (int i = 0; i < 20; ++i) v.push_back(i);
    CHECK(v.spilled());

    while (v.size() > 8) v.pop_back();
    CHECK(v.spilled()); // still spilled — no automatic shrink

    v.shrink_to_fit();
    CHECK_FALSE(v.spilled());
    REQUIRE(v.size() == 8);
    for (int i = 0; i < 8; ++i)
        CHECK(v[i] == i);
}

// ============================================================================
// resize
// ============================================================================

TEST_CASE (



"SmallVector: resize grows with default-constructed elements"
,
"[smallvector]"
)
 {
    SmallVector<int, 64> v;
    v.resize(5);
    REQUIRE(v.size() == 5);
    for (int x : v)
        CHECK(x == 0);
}

TEST_CASE (



"SmallVector: resize grows with value"
,
"[smallvector]"
)
 {
    SmallVector<int, 64> v;
    v.resize(4, 7);
    REQUIRE(v.size() == 4);
    for (int x : v)
        CHECK(x == 7);
}

TEST_CASE (



"SmallVector: resize shrinks and destroys excess"
,
"[smallvector]"
)
 {
    bool d[4] = {};
    SmallVector<NonTrivial, 256> v;
    for (int i = 0; i < 4; ++i) v.emplace_back(i, &d[i]);
    v.resize(2, NonTrivial{0, nullptr});
    REQUIRE(v.size() == 2);
    CHECK_FALSE(d[0]);
    CHECK_FALSE(d[1]);
    CHECK(d[2]);
    CHECK(d[3]);
}

// ============================================================================
// erase / insert
// ============================================================================

TEST_CASE (



"SmallVector: erase single element"
,
"[smallvector]"
)
 {
    SmallVector v{1, 2, 3, 4, 5};
    auto it = v.erase(v.begin() + 2);
    REQUIRE(v.size() == 4);
    CHECK(*it == 4);
    CHECK(v[0] == 1);
    CHECK(v[1] == 2);
    CHECK(v[2] == 4);
    CHECK(v[3] == 5);
}

TEST_CASE (



"SmallVector: erase range"
,
"[smallvector]"
)
 {
    SmallVector v{10, 20, 30, 40, 50};
    auto it = v.erase(v.begin() + 1, v.begin() + 4);
    REQUIRE(v.size() == 2);
    CHECK(*it == 50);
    CHECK(v[0] == 10);
    CHECK(v[1] == 50);
}

TEST_CASE (



"SmallVector: insert at position"
,
"[smallvector]"
)
 {
    SmallVector v{1, 2, 4, 5};
    auto it = v.insert(v.begin() + 2, 3);
    REQUIRE(v.size() == 5);
    CHECK(*it == 3);
    for (int i = 0; i < 5; ++i)
        CHECK(v[i] == i + 1);
}

TEST_CASE (



"SmallVector: insert at begin"
,
"[smallvector]"
)
 {
    SmallVector v{2, 3, 4};
    v.insert(v.begin(), 1);
    REQUIRE(v.size() == 4);
    CHECK(v[0] == 1);
    CHECK(v[1] == 2);
}

TEST_CASE (



"SmallVector: insert at end equivalent to push_back"
,
"[smallvector]"
)
 {
    SmallVector v{1, 2, 3};
    v.insert(v.end(), 4);
    REQUIRE(v.size() == 4);
    CHECK(v.back() == 4);
}

// ============================================================================
// Constructors
// ============================================================================

TEST_CASE (



"SmallVector: initializer_list constructor"
,
"[smallvector]"
)
 {
    SmallVector v{10, 20, 30, 40};
    REQUIRE(v.size() == 4);
    CHECK(v[0] == 10);
    CHECK(v[1] == 20);
    CHECK(v[2] == 30);
    CHECK(v[3] == 40);
}

TEST_CASE (



"SmallVector: range constructor from deque"
,
"[smallvector]"
)
 {
    std::deque dq{1, 2, 3, 4, 5};
    SmallVector<int, 64> v{dq.begin(), dq.end()};
    REQUIRE(v.size() == 5);
    for (int i = 0; i < 5; ++i)
        CHECK(v[i] == i + 1);
}

// ============================================================================
// swap
// ============================================================================

TEST_CASE (



"SmallVector: swap (both inline)"
,
"[smallvector]"
)
 {
    SmallVector a{1, 2, 3};
    SmallVector b{10, 20};
    a.swap(b);
    REQUIRE(a.size() == 2);
    CHECK(a[0] == 10);
    CHECK(a[1] == 20);
    REQUIRE(b.size() == 3);
    CHECK(b[0] == 1);
    CHECK(b[1] == 2);
    CHECK(b[2] == 3);
}

TEST_CASE (



"SmallVector: swap (both spilled)"
,
"[smallvector]"
)
 {
    SmallVector<int, 16> a, b; // kInlineCap = 4
    for (int i = 0; i < 10; ++i) a.push_back(i);
    for (int i = 0; i < 6; ++i) b.push_back(i * 10);
    a.swap(b);
    REQUIRE(a.size() == 6);
    REQUIRE(b.size() == 10);
    CHECK(a[0] == 0);
    CHECK(a[1] == 10);
    CHECK(b[0] == 0);
    CHECK(b[1] == 1);
}

// ============================================================================
// Comparison
// ============================================================================

TEST_CASE (



"SmallVector: equality and three-way comparison"
,
"[smallvector]"
)
 {
    SmallVector a{1, 2, 3};
    SmallVector b{1, 2, 3};
    SmallVector c{1, 2, 4};
    CHECK(a == b);
    CHECK_FALSE(a == c);
    CHECK((a <=> c) < 0);
    CHECK((c <=> a) > 0);
}

// ============================================================================
// at() bounds checking / iterators
// ============================================================================

TEST_CASE (



"SmallVector: at() throws on out-of-range"
,
"[smallvector]"
)
 {
    SmallVector v{1, 2, 3};
    CHECK(v.at(0) == 1);
    CHECK_THROWS_AS(v.at(3), std::out_of_range);
    CHECK_THROWS_AS(v.at(99), std::out_of_range);
}

TEST_CASE (



"SmallVector: range-for and reverse iteration"
,
"[smallvector]"
)
 {
    SmallVector v{1, 2, 3, 4, 5};
    int sum = 0;
    for (int x : v) sum += x;
    CHECK(sum == 15);

    int rev_sum = 0;
    for (int & it : std::views::reverse(v)) rev_sum += it;
    CHECK(rev_sum == 15);
}

// ============================================================================
// SmritiAllocator integration
// ============================================================================

TEST_CASE (



"SmallVector: BumpPool-backed spill via SmritiAllocator"
,
"[smallvector][allocator]"
)
 {
    pools::BumpPool<domains::SystemRAMDomain> pool{1 << 16};
    SmritiAllocator<int, decltype(pool)> alloc{pool};

    SmallVector<int, 16, decltype(alloc)> v{alloc}; // kInlineCap = 4
    for (int i = 0; i < 100; ++i) v.push_back(i);

    REQUIRE(v.size() == 100);
    for (int i = 0; i < 100; ++i)
        CHECK(v[i] == i);
}

TEST_CASE (



"SmallVector: ManagedResource-backed spill"
,
"[smallvector][allocator]"
)
 {
    DefaultResource res{
        domains::SystemRAMDomain{},
        pools::BumpPool<domains::SystemRAMDomain>{1 << 16}
    };
    auto alloc = make_allocator<int>(res);

    SmallVector<int, 16, decltype(alloc)> v{alloc};
    for (int i = 0; i < 50; ++i) v.push_back(i * 3);
    REQUIRE(v.size() == 50);
    for (int i = 0; i < 50; ++i)
        CHECK(v[i] == i * 3);
}

TEST_CASE (



"SmallVector: ScopedArena-backed spill"
,
"[smallvector][allocator]"
)
 {
    pools::ScopedArena<8192> arena;
    SmritiAllocator<int, decltype(arena)> alloc{arena};

    SmallVector<int, 16, decltype(alloc)> v{alloc}; // kInlineCap = 4
    for (int i = 0; i < 32; ++i) v.push_back(i);
    REQUIRE(v.size() == 32);
    for (int i = 0; i < 32; ++i)
        CHECK(v[i] == i);
}

TEST_CASE (



"SmallVector: std::allocator default (no smriti needed)"
,
"[smallvector]"
)
 {
    SmallVector<std::string, 128> v;
    v.push_back("hello");
    v.push_back("world");
    v.emplace_back("!");
    REQUIRE(v.size() == 3);
    CHECK(v[0] == "hello");
    CHECK(v[1] == "world");
    CHECK(v[2] == "!");
}

// ============================================================================
// Stress
// ============================================================================

TEST_CASE (



"SmallVector: stress push/clear/push cycle"
,
"[smallvector][stress]"
)
 {
    SmallVector<int, 64> v;
    for (int round = 0; round < 10; ++round) {
        for (int i = 0; i < 500; ++i) v.push_back(i);
        CHECK(v.size() == 500);
        v.clear();
        CHECK(v.empty());
    }
}

// ============================================================================
// New tests
// ============================================================================

TEST_CASE (



"SmallVector: erase single NonTrivial calls dtor exactly once"
,
"[smallvector]"
)
 {
    // erase(pos) shifts elements left via move-assign (no dtor on assign),
    // then destroys the vacated tail slot. Exactly one dtor fires — for the
    // element whose value was at the tail (value=4), not the erased position.
    // This matches std::vector behaviour.
    int dtors[5] = {};
    struct Tracked {
        int value;
        int* counters;
        Tracked(int v, int* c) : value{v}, counters{c} {}
        ~Tracked() { ++counters[value]; }
        Tracked(const Tracked&) = default;
        Tracked(Tracked&& o) noexcept : value{o.value}, counters{o.counters} {}

        Tracked& operator=(Tracked&& o) noexcept {
            value = o.value;
            counters = o.counters;
            return *this;
        }

        Tracked& operator=(const Tracked&) = default;
    };
    SmallVector<Tracked, 256> v;
    for (int i = 0; i < 5; ++i) v.emplace_back(i, dtors);
    v.erase(v.begin() + 2);
    REQUIRE(v.size() == 4);
    // Exactly one dtor fired: the vacated tail slot (which held value=4 after shift)
    int total = dtors[0] + dtors[1] + dtors[2] + dtors[3] + dtors[4];
    CHECK(total == 1);
    // Remaining elements intact (no spurious dtors)
    CHECK(dtors[0] == 0);
    CHECK(dtors[1] == 0);
    CHECK(dtors[3] == 0);
    // Values after erase: 0,1,3,4
    CHECK(v[0].value == 0);
    CHECK(v[1].value == 1);
    CHECK(v[2].value == 3);
    CHECK(v[3].value == 4);
}

TEST_CASE (



"SmallVector: swap (one inline, one spilled)"
,
"[smallvector]"
)
 {
    SmallVector<int, 16> a, b; // kInlineCap = 4
    a.push_back(1);
    a.push_back(2);
    a.push_back(3); // inline
    for (int i = 0; i < 10; ++i) b.push_back(i); // spilled
    a.swap(b);
    REQUIRE(a.size() == 10);
    REQUIRE(b.size() == 3);
    CHECK(b[0] == 1);
    CHECK(b[2] == 3);
    for (int i = 0; i < 10; ++i)
        CHECK(a[i] == i);
}

TEST_CASE (



"SmallVector: copy self-assign is no-op"
,
"[smallvector]"
)
 {
    SmallVector v{1, 2, 3};
    v = v;
    REQUIRE(v.size() == 3);
    CHECK(v[0] == 1);
    CHECK(v[2] == 3);
}

TEST_CASE (



"SmallVector: initializer_list assign"
,
"[smallvector]"
)
 {
    SmallVector v{1, 2, 3, 4, 5};
    v = {10, 20};
    REQUIRE(v.size() == 2);
    CHECK(v[0] == 10);
    CHECK(v[1] == 20);
}

TEST_CASE (



"SmallVector: const at() bounds check"
,
"[smallvector]"
)
 {
    const SmallVector cv{1, 2, 3};
    CHECK(cv.at(1) == 2);
    CHECK_THROWS_AS(cv.at(5), std::out_of_range);
}

// ============================================================================
// operator== equality_comparable constraint
// ============================================================================
// Regression: operator== was previously unconstrained. Instantiating
// SmallVector<T> for a T lacking operator== hard-errored the moment any
// generic facility probed `t == t` on the container (e.g. Glaze's
// equality_comparable check during serialization). The `requires
// std::equality_comparable<T>` constraint makes operator== SFINAE-friendly,
// so non-comparable element types are now fully usable.

namespace {
    struct NoEq {
        int v;
        // deliberately no operator==, no operator<=>
    };

    struct WithEq {
        int v;
        bool operator==(const WithEq&) const = default;
    };
} // namespace

TEST_CASE (



"SmallVector: non-equality-comparable T instantiates and works"
,
"[smallvector][concepts]"
)
 {
    // The mere existence of these instantiations proves operator== no longer
    // poisons a non-comparable element type.
    SmallVector<NoEq, 64> v;
    v.push_back(NoEq{1});
    v.emplace_back(2);
    v.push_back(NoEq{3});
    REQUIRE(v.size() == 3);
    CHECK(v[0].v == 1);
    CHECK(v[1].v == 2);
    CHECK(v[2].v == 3);

    // Spill path must also work for non-comparable T.
    for (int i = 0; i < 200; ++i) v.push_back(NoEq{i});
    CHECK(v.spilled());
    REQUIRE(v.size() == 203);
    CHECK(v[202].v == 199);

    // Copy / move must not require operator== either.
    SmallVector<NoEq, 64> copy{v};
    REQUIRE(copy.size() == v.size());
    SmallVector<NoEq, 64> moved{std::move(copy)};
    REQUIRE(moved.size() == v.size());
}

TEST_CASE (



"SmallVector: operator== is SFINAE-friendly (concept propagates)"
,
"[smallvector][concepts]"
)
 {
    // Container is equality_comparable iff its element type is.
    static_assert(std::equality_comparable<SmallVector<int, 64>>);
    static_assert(std::equality_comparable<SmallVector<WithEq, 64>>);
    static_assert(!std::equality_comparable<SmallVector<NoEq, 64>>);

    // And the constrained == still yields correct results when available.
    SmallVector<WithEq, 64> a{WithEq{1}, WithEq{2}};
    SmallVector<WithEq, 64> b{WithEq{1}, WithEq{2}};
    SmallVector<WithEq, 64> c{WithEq{1}, WithEq{9}};
    CHECK(a == b);
    CHECK_FALSE(a == c);
    SUCCEED("equality_comparable propagates through SmallVector");
}

