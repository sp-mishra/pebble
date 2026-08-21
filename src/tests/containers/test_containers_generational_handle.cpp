// =============================================================================
// test_containers_generational_handle.cpp — tests for generational_handle
// =============================================================================

#include "catch_amalgamated.hpp"

#include <type_traits>
#include <unordered_map>

#include "containers/handle/generational_handle.hpp"

namespace {
    struct backend_tag {};

    struct code_version_tag {};

    using BackendHandle = containers::generational_handle<backend_tag>;
    using VersionHandle = containers::generational_handle<code_version_tag>;
}

// ---------------------------------------------------------------------------
// Trivially copyable
// ---------------------------------------------------------------------------
static_assert(std::is_trivially_copyable_v<BackendHandle>);
static_assert(std::is_trivially_copyable_v<VersionHandle>);

// ---------------------------------------------------------------------------
// Null handle detection
// ---------------------------------------------------------------------------
TEST_CASE (



"generational_handle null detection"
,
"[containers][generational_handle]"
)
 {
    BackendHandle h;
    REQUIRE(h.is_null());
    REQUIRE(h.index == 0);
    REQUIRE(h.generation == 0);

    BackendHandle non_null{1, 1};
    REQUIRE_FALSE(non_null.is_null());
}

// ---------------------------------------------------------------------------
// Equality
// ---------------------------------------------------------------------------
TEST_CASE (



"generational_handle equality"
,
"[containers][generational_handle]"
)
 {
    BackendHandle a{3, 7};
    BackendHandle b{3, 7};
    BackendHandle c{3, 8}; // different generation

    REQUIRE(a == b);
    REQUIRE(a != c);
}

// ---------------------------------------------------------------------------
// Generation bump: old handle detects stale generation
// ---------------------------------------------------------------------------
TEST_CASE (



"generational_handle stale detection via generation mismatch"
,
"[containers][generational_handle]"
)
 {
    BackendHandle live{5, 2};
    BackendHandle stale{5, 1}; // same index, older generation

    REQUIRE(live != stale);
    REQUIRE(stale.index == live.index);
    REQUIRE(stale.generation != live.generation);
}

// ---------------------------------------------------------------------------
// Phantom Tag: BackendHandle and VersionHandle with same numeric fields
// are different types — no comparison between them at compile time.
// We verify they are distinct types (no implicit conversion).
// ---------------------------------------------------------------------------
static_assert(!std::is_same_v<BackendHandle, VersionHandle>,
              "handles with different tags must be distinct types");
static_assert(!std::is_convertible_v<BackendHandle, VersionHandle>,
              "handles with different tags must not be implicitly convertible");

// ---------------------------------------------------------------------------
// std::hash usable as a map key
// ---------------------------------------------------------------------------
TEST_CASE (



"generational_handle usable as unordered_map key"
,
"[containers][generational_handle]"
)
 {
    std::unordered_map<BackendHandle, int> m;

    BackendHandle h1{1, 1};
    BackendHandle h2{2, 1};
    BackendHandle h1_stale{1, 0}; // same index, different gen → different hash bucket

    m[h1] = 10;
    m[h2] = 20;

    REQUIRE(m.at(h1) == 10);
    REQUIRE(m.at(h2) == 20);
    REQUIRE(m.find(h1_stale) == m.end());
}

// ---------------------------------------------------------------------------
// null_handle constant
// ---------------------------------------------------------------------------
TEST_CASE (



"null_handle constant is null"
,
"[containers][generational_handle]"
)
 {
    const BackendHandle n = containers::null_handle<backend_tag>;
    REQUIRE(n.is_null());
    REQUIRE(n == BackendHandle{});
}
