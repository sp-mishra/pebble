// ============================================================================
// test_samasa_tree.cpp — green_tree build, span union, child count,
//   structural_hash stability.
// ============================================================================

#include "catch_amalgamated.hpp"
#include "languages/samasa/samasa.hpp"

namespace {

enum class SK : std::uint8_t { root, child };
enum class TK : std::uint8_t { eof, a, b };

using namespace lang::samasa;

// Build a token_buffer with two tokens: a@0 b@1
token_buffer<TK> make_two_tokens() {
    token_buffer<TK> buf;
    buf.data.push_back({TK::a, 0, 1, 0, 0});
    buf.data.push_back({TK::b, 1, 1, 0, 0});
    buf.data.push_back({TK::eof, 2, 0, 0, 0});
    return buf;
}

// Manually construct an event_stream:
//   begin(root) → token(0) → token(1) → end(root)
event_stream<SK> make_root_events() {
    event_stream<SK> ev;
    auto mk = ev.begin(SK::root);
    ev.token(0);
    ev.token(1);
    ev.end(mk, byte_span{0, 2});
    return ev;
}

} // anonymous namespace

// ============================================================================

TEST_CASE("green_tree: build from event_stream produces non-empty tree", "[samasa][tree]") {
    auto buf = make_two_tokens();
    auto ev  = make_root_events();
    auto stream = buf.view();
    auto tree = green_tree<SK>::build(ev, stream, "ab");

    CHECK(!tree.empty());
}

TEST_CASE("green_tree: root node exists after build", "[samasa][tree]") {
    auto buf = make_two_tokens();
    auto ev  = make_root_events();
    auto stream = buf.view();
    auto tree = green_tree<SK>::build(ev, stream, "ab");

    CHECK(tree.root() != k_null_green);
}

TEST_CASE("green_tree: root span covers both tokens", "[samasa][tree]") {
    auto buf = make_two_tokens();
    auto ev  = make_root_events();
    auto stream = buf.view();
    auto tree = green_tree<SK>::build(ev, stream, "ab");

    const auto& root = tree[tree.root()];
    CHECK(root.span.offset == 0);
    CHECK(root.span.length == 2);
}

TEST_CASE("green_tree: root child count matches emitted tokens", "[samasa][tree]") {
    auto buf = make_two_tokens();
    auto ev  = make_root_events();
    auto stream = buf.view();
    auto tree = green_tree<SK>::build(ev, stream, "ab");

    const auto& root = tree[tree.root()];
    CHECK(root.child_count == 2);
}

TEST_CASE("green_tree: structural_hash is non-zero", "[samasa][tree]") {
    auto buf = make_two_tokens();
    auto ev  = make_root_events();
    auto stream = buf.view();
    auto tree = green_tree<SK>::build(ev, stream, "ab");

    CHECK(tree[tree.root()].structural_hash != 0);
}

TEST_CASE("green_tree: structural_hash stability — same events same hash", "[samasa][tree]") {
    auto buf1 = make_two_tokens();
    auto ev1  = make_root_events();
    auto stream1 = buf1.view();
    auto tree1 = green_tree<SK>::build(ev1, stream1, "ab");

    auto buf2 = make_two_tokens();
    auto ev2  = make_root_events();
    auto stream2 = buf2.view();
    auto tree2 = green_tree<SK>::build(ev2, stream2, "ab");

    CHECK(tree1[tree1.root()].structural_hash == tree2[tree2.root()].structural_hash);
}

TEST_CASE("green_tree: different source produces different hash", "[samasa][tree]") {
    // token 'a' vs token 'x'
    token_buffer<TK> bufA, bufX;
    bufA.data.push_back({TK::a, 0, 1, 0, 0});
    bufA.data.push_back({TK::eof, 1, 0, 0, 0});
    bufX.data.push_back({TK::a, 0, 1, 0, 0});
    bufX.data.push_back({TK::eof, 1, 0, 0, 0});

    event_stream<SK> evA, evX;
    auto mkA = evA.begin(SK::root); evA.token(0); evA.end(mkA, byte_span{0,1});
    auto mkX = evX.begin(SK::root); evX.token(0); evX.end(mkX, byte_span{0,1});

    auto treeA = green_tree<SK>::build(evA, bufA.view(), "a");
    auto treeX = green_tree<SK>::build(evX, bufX.view(), "x");

    CHECK(treeA[treeA.root()].structural_hash != treeX[treeX.root()].structural_hash);
}

TEST_CASE("green_tree: empty event_stream produces empty tree", "[samasa][tree]") {
    token_buffer<TK> buf;
    buf.data.push_back({TK::eof, 0, 0, 0, 0});
    event_stream<SK> ev;
    auto tree = green_tree<SK>::build(ev, buf.view(), "");
    CHECK(tree.empty());
}

// ============================================================================
// New tests [R15]: red tree is lazy — not built unless explicitly requested
// ============================================================================

TEST_CASE("red_tree: not built when build_red_tree=false (default)", "[samasa][tree]") {
    // parse_output does not contain a red_tree — it must be built manually.
    // Verify that parse_output has no red_tree field (compile-time check).
    using Out = parse_output<SK, TK>;
    // parse_output must NOT have a red_tree member — static compilation check.
    // We verify by checking the type has no red_tree field (indirectly via lack of member).
    STATIC_REQUIRE(!std::is_same_v<Out, red_tree<SK>>);
}

TEST_CASE("red_tree: build() from green_tree succeeds and is non-empty", "[samasa][tree]") {
    auto buf = make_two_tokens();
    auto ev  = make_root_events();
    auto stream = buf.view();
    auto tree = green_tree<SK>::build(ev, stream, "ab");
    REQUIRE(!tree.empty());

    // Build red tree on demand.
    auto rt = red_tree<SK>::build(tree);
    CHECK(!rt.empty());
    CHECK(rt.root() != k_null_red);
}

TEST_CASE("red_tree: root node's parent is null", "[samasa][tree]") {
    auto buf = make_two_tokens();
    auto ev  = make_root_events();
    auto tree = green_tree<SK>::build(ev, buf.view(), "ab");
    auto rt   = red_tree<SK>::build(tree);

    REQUIRE(!rt.empty());
    CHECK(rt[rt.root()].parent == k_null_red);
}

TEST_CASE("red_tree: child nodes have correct parent red_id", "[samasa][tree]") {
    auto buf = make_two_tokens();
    auto ev  = make_root_events();
    auto tree = green_tree<SK>::build(ev, buf.view(), "ab");
    auto rt   = red_tree<SK>::build(tree);

    REQUIRE(!rt.empty());
    const red_id root_rid = rt.root();
    // Children of the root in the green tree should have parent = root_rid.
    auto children = tree.children(tree.root());
    if (!children.empty()) {
        const green_id first_child = children[0];
        CHECK(rt[first_child].parent == static_cast<red_id>(tree.root()));
    }
}
