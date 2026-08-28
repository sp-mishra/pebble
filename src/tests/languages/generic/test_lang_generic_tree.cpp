// test_lang_generic_tree.cpp — Unit tests for lang::byte_span, event_log, green_arena, static_event_buffer.
//
// C++23. Verifies span primitives, event_log marker/rollback/tombstone, arena build + hashes,
// and static_event_buffer compile-time behavior.

#include "catch_amalgamated.hpp"
#include "languages/generic/tree/spans.hpp"
#include "languages/generic/tree/event_log.hpp"
#include "languages/generic/tree/green_arena.hpp"
#include "languages/generic/tree/static_buffers.hpp"
#include <cstdint>

// ---- byte_span primitives -------------------------------------------------

static_assert([] {
    lang::byte_span a{2, 3}; // [2, 5)
    lang::byte_span b{4, 4}; // [4, 8)
    auto h = lang::byte_span::hull(a, b);
    assert(h.offset == 2 && h.end() == 8 && h.length == 6);
    return true;
}());

static_assert([] {
    lang::byte_span empty{};
    lang::byte_span s{5, 3};
    assert(lang::byte_span::hull(empty, s) == s);
    assert(lang::byte_span::hull(s, empty) == s);
    return true;
}());

static_assert([] {
    // hull associativity: hull(hull(a,b), c) == hull(a, hull(b,c))
    lang::byte_span a{1, 2}, b{4, 2}, c{7, 3};
    auto lhs = lang::byte_span::hull(lang::byte_span::hull(a, b), c);
    auto rhs = lang::byte_span::hull(a, lang::byte_span::hull(b, c));
    assert(lhs == rhs);
    return true;
}());

// token_range
static_assert([] {
    lang::token_range r{3, 7};
    assert(r.size() == 4 && !r.empty());
    lang::token_range empty{5, 5};
    assert(empty.empty());
    return true;
}());

// ---- event_log: marker / rollback / tombstone ----------------------------

enum class TK : std::uint8_t { root = 0, inner = 1 };

TEST_CASE("event_log: basic begin/token/end", "[lang][tree]") {
    lang::event_log<TK> log;
    const auto m = log.begin(TK::root);
    log.token(0);
    log.token(1);
    log.end(m, lang::byte_span{0, 10});

    REQUIRE(log.event_count() == 4); // begin + 2 tokens + end
    REQUIRE(log.depth() == 0);
    REQUIRE(log.all()[0].kind == lang::event_kind::begin_node);
    REQUIRE(log.all()[3].kind == lang::event_kind::end_node);
}

TEST_CASE("event_log: rollback clean", "[lang][tree]") {
    lang::event_log<TK> log;
    const auto snap = log.snapshot();
    const auto m = log.begin(TK::inner);
    (void)m;
    // No tokens emitted → clean rollback truncates
    const auto m2 = log.begin(TK::inner);
    log.rollback(m2);
    REQUIRE(log.event_count() == 1); // m's begin_node still present (opened before snap reuse)
    // Rollback to snapshot (before any begin)
    log.rollback(snap);
    REQUIRE(log.event_count() == 0);
}

TEST_CASE("event_log: rollback tombstone", "[lang][tree]") {
    lang::event_log<TK> log;
    const auto m = log.begin(TK::inner);
    log.token(0); // committed token
    // Rollback after committed token → tombstone, not truncate
    log.rollback(m);
    REQUIRE(log.event_count() == 2); // begin (tombstoned) + token
    REQUIRE(log.all()[0].kind == lang::event_kind::tombstone);
}

// ---- green_arena::build — structure + stable hashes ----------------------

TEST_CASE("green_arena: build structure and stable hashes", "[lang][tree]") {
    lang::event_log<TK> log;
    const auto m = log.begin(TK::root);
    log.token(0);
    log.token(1);
    log.end(m, lang::byte_span{0, 10});

    auto span_fn = [](std::uint32_t i) -> lang::byte_span {
        return i == 0 ? lang::byte_span{0, 5} : lang::byte_span{5, 5};
    };
    auto hash_fn = [](std::uint32_t i) -> std::uint64_t {
        return i == 0 ? 0xAAAA : 0xBBBB;
    };

    auto arena = lang::green_arena<TK>::build(log, span_fn, hash_fn);
    REQUIRE(!arena.empty());
    REQUIRE(arena.root() != lang::k_null_arena);

    const auto& root_node = arena[arena.root()];
    REQUIRE(root_node.child_count == 2);
    REQUIRE(root_node.span == lang::byte_span(lang::byte_span{0, 10}));

    // Hash must be stable (same inputs → same hash)
    auto arena2 = lang::green_arena<TK>::build(log, span_fn, hash_fn);
    REQUIRE(arena[arena.root()].structural_hash ==
            arena2[arena2.root()].structural_hash);
}

// ---- static_event_buffer: compile-time begin/token/end + rollback + overflow --

static_assert([] {
    lang::static_event_buffer<TK, std::uint16_t, 16> buf;
    const auto m = buf.begin(TK::root);
    buf.token(0);
    buf.token(1);
    buf.end(m, lang::byte_span{0, 10});
    assert(buf.event_count() == 4);
    assert(!buf.overflow());
    return true;
}());

static_assert([] {
    // With MaxEvents=3: begin + token fills 2; third push_back fills exactly; next overflows.
    lang::static_event_buffer<TK, std::uint16_t, 3> buf;
    static_cast<void>(buf.begin(TK::root)); // event 0
    buf.token(0);                            // event 1
    buf.token(1);                            // event 2 — now at capacity
    buf.token(2);                            // overflow: push_back returns false, sticky set
    assert(buf.overflow());
    assert(buf.event_count() == 3);          // count stays at max
    return true;
}());

static_assert([] {
    // rollback without committed tokens → clean truncate
    lang::static_event_buffer<TK, std::uint16_t, 16> buf;
    const auto snap = buf.snapshot();
    static_cast<void>(buf.begin(TK::inner)); // no tokens after
    buf.rollback(snap);
    assert(buf.event_count() == 0);
    return true;
}());
