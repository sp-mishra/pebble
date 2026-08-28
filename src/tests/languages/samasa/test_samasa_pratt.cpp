// ============================================================================
// test_samasa_pratt.cpp — Pratt parser: left/right associativity, postfix,
//   non-assoc chain rejection.
// ============================================================================

#include "catch_amalgamated.hpp"
#include "languages/samasa/samasa.hpp"

namespace {

enum class SK : std::uint8_t { root };

// Token kinds: eof, num (primary), plus (left, bp=10), caret (right, bp=20),
//              bang (postfix, bp=30), eq (non-assoc, bp=5).
enum class TK : std::uint8_t { eof, num, plus, caret, bang, eq };

using namespace lang::samasa;

// Operator table
using MyOps = operator_table<
    op<"+", TK::plus,  10, associativity::left,  fixity::infix>,
    op<"^", TK::caret, 20, associativity::right, fixity::infix>,
    op<"!", TK::bang,  30, associativity::left,  fixity::postfix>,
    op<"=", TK::eq,     5, associativity::none,  fixity::infix>
>;

// Primary rule: matches a single TK::num token.
struct num_rule {
    template <class Ctx>
    [[nodiscard]] auto match(Ctx& ctx) const {
        return tok<TK::num>{}.match(ctx);
    }
};

using MyPratt = pratt_expression<MyOps, num_rule>;

// Build a minimal parse_context from a token sequence.
struct PrattCtx {
    token_buffer<TK>                      buf;
    token_stream<TK>                      stream;
    event_stream<SK>                      events;
    lang::collecting_sink<diagnostic>     sink;
    lang::parse_tree_stats                stats;
    std::optional<parse_context<SK, TK>>  ctx;

    parse_context<SK, TK>& operator*() { return *ctx; }

    explicit PrattCtx(std::initializer_list<TK> kinds) {
        std::uint32_t off = 0;
        for (TK k : kinds) {
            buf.data.push_back({k, off, 1, 0, 0});
            ++off;
        }
        buf.data.push_back({TK::eof, off, 0, 0, 0});
        stream = buf.view();
        ctx.emplace(stream, std::string_view{}, events, sink, stats);
    }
};

} // anonymous namespace

// ============================================================================
// Operator table static properties
// ============================================================================

TEST_CASE("pratt: infix_bp left assoc returns (bp, bp+1)", "[samasa][pratt]") {
    const auto bp = MyOps::infix_bp(TK::plus);
    REQUIRE(bp.has_value());
    CHECK(bp->first  == 10);   // left bp
    CHECK(bp->second == 11);   // right bp = left+1 for left-assoc
}

TEST_CASE("pratt: infix_bp right assoc returns (bp, bp)", "[samasa][pratt]") {
    const auto bp = MyOps::infix_bp(TK::caret);
    REQUIRE(bp.has_value());
    CHECK(bp->first  == 20);
    CHECK(bp->second == 20);   // same → right-assoc
}

TEST_CASE("pratt: postfix_bp returns binding power", "[samasa][pratt]") {
    const auto bp = MyOps::postfix_bp(TK::bang);
    REQUIRE(bp.has_value());
    CHECK(*bp == 30);
}

TEST_CASE("pratt: non-assoc infix returns (bp, bp+1)", "[samasa][pratt]") {
    // none assoc uses rbp = lbp+1 just like left-assoc (prevents chaining).
    const auto bp = MyOps::infix_bp(TK::eq);
    REQUIRE(bp.has_value());
    CHECK(bp->first  == 5);
    CHECK(bp->second == 6);
}

TEST_CASE("pratt: infix_bp returns nullopt for non-operator token", "[samasa][pratt]") {
    const auto bp = MyOps::infix_bp(TK::num);
    CHECK(!bp.has_value());
}

// ============================================================================
// Runtime Pratt parsing
// ============================================================================

TEST_CASE("pratt: single primary succeeds", "[samasa][pratt]") {
    PrattCtx pc{TK::num};
    MyPratt p{};
    auto r = p.match(*pc.ctx);
    CHECK(r.ok());
    CHECK(pc.ctx->cursor().pos == 1);
}

TEST_CASE("pratt: left-assoc chain a+b+c consumes all tokens", "[samasa][pratt]") {
    // num + num + num
    PrattCtx pc{TK::num, TK::plus, TK::num, TK::plus, TK::num};
    MyPratt p{};
    auto r = p.match(*pc.ctx);
    CHECK(r.ok());
    CHECK(pc.ctx->cursor().pos == 5);
}

TEST_CASE("pratt: right-assoc chain a^b^c consumes all tokens", "[samasa][pratt]") {
    // num ^ num ^ num  — right assoc: same rbp allows deeper recursion
    PrattCtx pc{TK::num, TK::caret, TK::num, TK::caret, TK::num};
    MyPratt p{};
    auto r = p.match(*pc.ctx);
    CHECK(r.ok());
    CHECK(pc.ctx->cursor().pos == 5);
}

TEST_CASE("pratt: postfix operator consumed after primary", "[samasa][pratt]") {
    // num !
    PrattCtx pc{TK::num, TK::bang};
    MyPratt p{};
    auto r = p.match(*pc.ctx);
    CHECK(r.ok());
    CHECK(pc.ctx->cursor().pos == 2);
}

TEST_CASE("pratt: non-assoc a=b stops before second =", "[samasa][pratt]") {
    // num = num = num — non-assoc: rbp=lbp+1 prevents the second = from being
    // consumed as the rhs of the first = (recursive call uses min_bp=6, rejects lbp=5).
    // The outer loop (min_bp=0) still consumes the second =.
    // All 5 tokens consumed — pos==5.
    PrattCtx pc{TK::num, TK::eq, TK::num, TK::eq, TK::num};
    MyPratt p{};
    auto r = p.match(*pc.ctx);
    CHECK(r.ok());
    CHECK(pc.ctx->cursor().pos == 5);
}

TEST_CASE("pratt: higher precedence ^ binds tighter than +", "[samasa][pratt]") {
    // num + num ^ num — ^ consumed in recursive call from +
    PrattCtx pc{TK::num, TK::plus, TK::num, TK::caret, TK::num};
    MyPratt p{};
    auto r = p.match(*pc.ctx);
    CHECK(r.ok());
    CHECK(pc.ctx->cursor().pos == 5);
}

// ============================================================================
// New tests [R11]: default action emits CST token events (not AST construction)
// ============================================================================

TEST_CASE("pratt: default cst_pratt_action emits token events into event_stream", "[samasa][pratt]") {
    // Parse num + num: expect token events for both num tokens and the + token.
    PrattCtx pc{TK::num, TK::plus, TK::num};
    const auto events_before = pc.events.event_count();
    MyPratt p{};
    auto r = p.match(*pc.ctx);
    REQUIRE(r.ok());
    // Event stream must have grown (token events for num, +, num).
    CHECK(pc.events.event_count() > events_before);
}

TEST_CASE("pratt: cst_pratt_action is default — no explicit action type needed", "[samasa][pratt]") {
    // Verify the default template argument compiles and produces the same result
    // as explicitly naming cst_pratt_action.
    using ExplicitType = pratt_expression<MyOps, num_rule, cst_pratt_action>;
    using DefaultType  = pratt_expression<MyOps, num_rule>;
    STATIC_REQUIRE(std::is_same_v<ExplicitType, DefaultType>);
}

TEST_CASE("pratt: token events count matches tokens consumed in a+b*c parse", "[samasa][pratt]") {
    // num + num ^ num (5 tokens consumed) → 5 token events expected in stream.
    PrattCtx pc{TK::num, TK::plus, TK::num, TK::caret, TK::num};
    const auto events_before = pc.events.event_count();
    MyPratt p{};
    auto r = p.match(*pc.ctx);
    REQUIRE(r.ok());
    // Each consumed token emits exactly one event.
    const auto events_added = pc.events.event_count() - events_before;
    CHECK(events_added == 5); // num, +, num, ^, num
}

// ============================================================================
// New tests [design.md]: flat_pratt_action vs structured_pratt_action
// ============================================================================

// Syntax kinds for structured Pratt tests
enum class PSK : std::uint8_t { root, binary_expr, prefix_expr, postfix_expr };

TEST_CASE("pratt: flat_pratt_action is default — same as cst_pratt_action alias", "[samasa][pratt]") {
    using ExplicitFlat = pratt_expression<MyOps, num_rule, flat_pratt_action>;
    using DefaultType  = pratt_expression<MyOps, num_rule>;
    STATIC_REQUIRE(std::is_same_v<ExplicitFlat, DefaultType>);
}

TEST_CASE("pratt: flat_pratt_action emits no begin/end node events", "[samasa][pratt]") {
    // With flat action, only token events are emitted — no begin_node/end_node.
    using namespace lang::samasa;
    token_buffer<TK>                      buf;
    token_stream<TK>                      stream;
    event_stream<PSK>                     events;
    lang::collecting_sink<diagnostic>     sink;
    lang::parse_tree_stats                stats;

    std::uint32_t off = 0;
    for (TK k : {TK::num, TK::plus, TK::num}) {
        buf.data.push_back({k, off, 1, 0, 0});
        ++off;
    }
    buf.data.push_back({TK::eof, off, 0, 0, 0});
    stream = buf.view();
    std::optional<parse_context<PSK, TK>> ctx;
    ctx.emplace(stream, std::string_view{}, events, sink, stats);

    pratt_expression<MyOps, num_rule, flat_pratt_action> p{};
    auto r = p.match(*ctx);
    REQUIRE(r.ok());

    // All events should be token events — no begin_node or end_node.
    bool has_node_event = false;
    for (const auto& ev : events.all())
        if (ev.kind == event_kind::begin_node || ev.kind == event_kind::end_node)
            has_node_event = true;
    CHECK(!has_node_event);
}

TEST_CASE("pratt: structured_pratt_action emits begin_node events for binary ops", "[samasa][pratt]") {
    // structured_pratt_action::begin_infix() calls events.begin(BinaryKind).
    // For num + num, we expect at least one begin_node event.
    using namespace lang::samasa;
    token_buffer<TK>                      buf;
    token_stream<TK>                      stream;
    event_stream<PSK>                     events;
    lang::collecting_sink<diagnostic>     sink;
    lang::parse_tree_stats                stats;

    std::uint32_t off = 0;
    for (TK k : {TK::num, TK::plus, TK::num}) {
        buf.data.push_back({k, off, 1, 0, 0});
        ++off;
    }
    buf.data.push_back({TK::eof, off, 0, 0, 0});
    stream = buf.view();
    std::optional<parse_context<PSK, TK>> ctx;
    ctx.emplace(stream, std::string_view{}, events, sink, stats);

    using StructuredAction = structured_pratt_action<PSK::binary_expr, PSK::prefix_expr, PSK::postfix_expr>;
    pratt_expression<MyOps, num_rule, StructuredAction> p{};
    auto r = p.match(*ctx);
    REQUIRE(r.ok());

    // Expect at least one begin_node event of kind binary_expr.
    bool has_binary_begin = false;
    for (const auto& ev : events.all())
        if (ev.kind == event_kind::begin_node && ev.syntax == PSK::binary_expr)
            has_binary_begin = true;
    CHECK(has_binary_begin);
}

TEST_CASE("pratt: structured_pratt_action — num+num has more events than flat", "[samasa][pratt]") {
    // Structured emits begin_node in addition to token events, so event count > flat count.
    using namespace lang::samasa;

    auto build_ctx = [](token_buffer<TK>& b, token_stream<TK>& s,
                        event_stream<PSK>& e, lang::collecting_sink<diagnostic>& sk,
                        lang::parse_tree_stats& st,
                        std::optional<parse_context<PSK,TK>>& c) {
        std::uint32_t off = 0;
        for (TK k : {TK::num, TK::plus, TK::num}) {
            b.data.push_back({k, off, 1, 0, 0});
            ++off;
        }
        b.data.push_back({TK::eof, off, 0, 0, 0});
        s = b.view();
        c.emplace(s, std::string_view{}, e, sk, st);
    };

    token_buffer<TK> b1; token_stream<TK> s1; event_stream<PSK> e1;
    lang::collecting_sink<diagnostic> sk1; lang::parse_tree_stats st1;
    std::optional<parse_context<PSK,TK>> c1;
    build_ctx(b1, s1, e1, sk1, st1, c1);

    token_buffer<TK> b2; token_stream<TK> s2; event_stream<PSK> e2;
    lang::collecting_sink<diagnostic> sk2; lang::parse_tree_stats st2;
    std::optional<parse_context<PSK,TK>> c2;
    build_ctx(b2, s2, e2, sk2, st2, c2);

    pratt_expression<MyOps, num_rule, flat_pratt_action> pf{};
    (void)pf.match(*c1);

    using SA = structured_pratt_action<PSK::binary_expr, PSK::prefix_expr, PSK::postfix_expr>;
    pratt_expression<MyOps, num_rule, SA> ps{};
    (void)ps.match(*c2);

    // Structured must produce more events (has begin_node) than flat (token-only).
    CHECK(e2.event_count() > e1.event_count());
}

// ============================================================================
// Conformance suite — structured_pratt_action exact event ordering
// ============================================================================

// Helper: build a token sequence and run structured Pratt, return event list.
namespace {
using SEvt = parse_event<PSK>;

std::vector<SEvt> run_structured_pratt(std::initializer_list<TK> kinds) {
    using SA = structured_pratt_action<PSK::binary_expr, PSK::prefix_expr, PSK::postfix_expr>;

    token_buffer<TK>                  buf;
    std::uint32_t off = 0;
    for (TK k : kinds) { buf.data.push_back({k, off, 1, 0, 0}); ++off; }
    buf.data.push_back({TK::eof, off, 0, 0, 0});

    token_stream<TK>                      stream = buf.view();
    event_stream<PSK>                     events;
    lang::collecting_sink<diagnostic>     sink;
    lang::parse_tree_stats                stats;
    parse_context<PSK, TK>               ctx{stream, std::string_view{}, events, sink, stats};

    pratt_expression<MyOps, num_rule, SA> p{};
    (void)p.match(ctx);

    return std::vector<SEvt>(events.all().begin(), events.all().end());
}
} // anonymous namespace

TEST_CASE("pratt: structured — binary 'num + num' event order: begin, tok(num), tok(plus), tok(num), end", "[samasa][pratt][conformance]") {
    // a + b: begin_node wraps both operands (Stage 5 left-wrap fix).
    // Actual order: begin_node(binary_expr), tok(a), tok(+), tok(b), end_node.
    const auto evts = run_structured_pratt({TK::num, TK::plus, TK::num});

    // Exactly 5 events: begin + tok(a) + tok(+) + tok(b) + end.
    REQUIRE(evts.size() == 5);
    CHECK(evts[0].kind == event_kind::begin_node);     // begin binary_expr
    CHECK(evts[0].syntax == PSK::binary_expr);
    CHECK(evts[1].kind == event_kind::token);           // tok(num) — left operand
    CHECK(evts[2].kind == event_kind::token);           // tok(+)
    CHECK(evts[3].kind == event_kind::token);           // tok(num) — RHS
    CHECK(evts[4].kind == event_kind::end_node);        // end binary_expr
}

TEST_CASE("pratt: structured — 'num + num * num' precedence nesting", "[samasa][pratt][conformance]") {
    // a + b ^ c (caret bp=20 > plus bp=10):
    // begin(+), tok(a), tok(+), begin(^), tok(b), tok(^), tok(c), end(^), end(+)
    const auto evts = run_structured_pratt({TK::num, TK::plus, TK::num, TK::caret, TK::num});

    // Total events: 9
    REQUIRE(evts.size() == 9);
    CHECK(evts[0].kind == event_kind::begin_node);   // begin binary_expr (plus) — wraps all
    CHECK(evts[0].syntax == PSK::binary_expr);
    CHECK(evts[1].kind == event_kind::token);         // tok(a)
    CHECK(evts[2].kind == event_kind::token);         // tok(+)
    CHECK(evts[3].kind == event_kind::begin_node);   // begin binary_expr (caret)
    CHECK(evts[3].syntax == PSK::binary_expr);
    CHECK(evts[4].kind == event_kind::token);         // tok(b)
    CHECK(evts[5].kind == event_kind::token);         // tok(^)
    CHECK(evts[6].kind == event_kind::token);         // tok(c)
    CHECK(evts[7].kind == event_kind::end_node);      // end binary_expr (caret)
    CHECK(evts[8].kind == event_kind::end_node);      // end binary_expr (plus)
}

TEST_CASE("pratt: structured — postfix 'num !' event order", "[samasa][pratt][conformance]") {
    // a!: begin(postfix_expr), tok(a), tok(!), end(postfix_expr)
    const auto evts = run_structured_pratt({TK::num, TK::bang});

    REQUIRE(evts.size() == 4);
    CHECK(evts[0].kind == event_kind::begin_node);   // begin postfix_expr — wraps operand
    CHECK(evts[0].syntax == PSK::postfix_expr);
    CHECK(evts[1].kind == event_kind::token);         // tok(a)
    CHECK(evts[2].kind == event_kind::token);         // tok(!)
    CHECK(evts[3].kind == event_kind::end_node);      // end postfix_expr
}

// ============================================================================
// Stage 5 tests — structured_pratt_action: left-operand wrapping + exact spans
//
// After Stage 5, structured_pratt_action uses insert_begin_at so the binary
// open-node wraps its left operand. The conformance tests above encode the old
// flat-sibling behavior and will fail; these new tests assert the correct shape.
//
// Token layout: each token gets offset == stream_position, length == 1.
//   Stream: num(0) op(1) num(2) op(3) num(4)  →  offsets 0..4
// ============================================================================

namespace {

// Extend TK with a minus for prefix tests.
enum class TK5 : std::uint8_t { eof, num, plus, caret, bang, minus };

// Operator table with prefix minus (bp=25), infix +/^ and postfix !.
using Ops5 = operator_table<
    op<"+",  TK5::plus,  10, associativity::left,  fixity::infix>,
    op<"^",  TK5::caret, 20, associativity::right, fixity::infix>,
    op<"!",  TK5::bang,  30, associativity::left,  fixity::postfix>,
    op<"-",  TK5::minus, 25, associativity::left,  fixity::prefix>
>;

struct num5_rule {
    template <class Ctx>
    [[nodiscard]] auto match(Ctx& ctx) const {
        return tok<TK5::num>{}.match(ctx);
    }
};

using SA5 = structured_pratt_action<PSK::binary_expr, PSK::prefix_expr, PSK::postfix_expr>;

// Build context from a token list and run structured Pratt; return event vector.
std::vector<SEvt> run_s5(std::initializer_list<TK5> kinds) {
    token_buffer<TK5>                  buf;
    std::uint32_t off = 0;
    for (TK5 k : kinds) { buf.data.push_back({k, off, 1, 0, 0}); ++off; }
    buf.data.push_back({TK5::eof, off, 0, 0, 0});

    token_stream<TK5>                      stream = buf.view();
    event_stream<PSK>                      events;
    lang::collecting_sink<diagnostic>      sink;
    lang::parse_tree_stats                 stats;
    parse_context<PSK, TK5>               ctx{stream, std::string_view{}, events, sink, stats};

    pratt_expression<Ops5, num5_rule, SA5> p{};
    (void)p.match(ctx);

    return std::vector<SEvt>(events.all().begin(), events.all().end());
}

} // anonymous namespace (stage5)

TEST_CASE("pratt stage5: structured — left-wrap: binary node encloses left operand",
          "[samasa][pratt][stage5]") {
    // num + num: begin wraps BOTH operands (left-wrap fix).
    // Expected event order: begin_node, tok(0/num), tok(1/+), tok(2/num), end_node
    const auto evts = run_s5({TK5::num, TK5::plus, TK5::num});

    REQUIRE(evts.size() == 5);
    CHECK(evts[0].kind == event_kind::begin_node);
    CHECK(evts[0].syntax == PSK::binary_expr);
    CHECK(evts[1].kind == event_kind::token);          // tok(num)
    CHECK(evts[2].kind == event_kind::token);          // tok(+)
    CHECK(evts[3].kind == event_kind::token);          // tok(num)
    CHECK(evts[4].kind == event_kind::end_node);
    CHECK(evts[4].syntax == PSK::binary_expr);
}

TEST_CASE("pratt stage5: structured — exact span: binary num+num covers whole expression",
          "[samasa][pratt][stage5]") {
    // Tokens: num@{0,1} plus@{1,1} num@{2,1}
    // Expected binary_expr span = hull({0,1}, {2,1}) = {0, 3}
    const auto evts = run_s5({TK5::num, TK5::plus, TK5::num});

    REQUIRE(evts.size() == 5);
    const auto& end_ev = evts[4];
    REQUIRE(end_ev.kind == event_kind::end_node);
    CHECK(end_ev.span.offset == 0);
    CHECK(end_ev.span.length == 3);  // covers "a + b"
}

TEST_CASE("pratt stage5: structured — precedence nesting: num+num^num tree shape",
          "[samasa][pratt][stage5]") {
    // num(0) +(1) num(2) ^(3) num(4)
    // Expected tree (^ has higher bp=20 than +=10):
    //   begin binary_expr (+)         ← wraps all 5 tokens
    //     tok(0) tok(1)               ← left operand (num) and operator (+)
    //     begin binary_expr (^)       ← wraps tokens 2..4
    //       tok(2) tok(3) tok(4)
    //     end binary_expr (^)
    //   end binary_expr (+)
    // Events: begin(+), tok0, tok1, begin(^), tok2, tok3, tok4, end(^), end(+)  → 9 events
    const auto evts = run_s5({TK5::num, TK5::plus, TK5::num, TK5::caret, TK5::num});

    REQUIRE(evts.size() == 9);
    CHECK(evts[0].kind == event_kind::begin_node);   // outer begin (+)
    CHECK(evts[0].syntax == PSK::binary_expr);
    CHECK(evts[1].kind == event_kind::token);         // tok(0) num
    CHECK(evts[2].kind == event_kind::token);         // tok(1) +
    CHECK(evts[3].kind == event_kind::begin_node);   // inner begin (^)
    CHECK(evts[3].syntax == PSK::binary_expr);
    CHECK(evts[4].kind == event_kind::token);         // tok(2) num
    CHECK(evts[5].kind == event_kind::token);         // tok(3) ^
    CHECK(evts[6].kind == event_kind::token);         // tok(4) num
    CHECK(evts[7].kind == event_kind::end_node);      // end (^)
    CHECK(evts[8].kind == event_kind::end_node);      // end (+)
}

TEST_CASE("pratt stage5: structured — exact spans: num+num^num hull spans",
          "[samasa][pratt][stage5]") {
    // Tokens: num@{0,1} +@{1,1} num@{2,1} ^@{3,1} num@{4,1}
    // Outer (+) end_node span = hull({0,1},{4,1}) = {0,5}
    // Inner (^) end_node span = hull({2,1},{4,1}) = {2,3}
    const auto evts = run_s5({TK5::num, TK5::plus, TK5::num, TK5::caret, TK5::num});

    REQUIRE(evts.size() == 9);
    // Inner ^ end_node at index 7
    CHECK(evts[7].kind == event_kind::end_node);
    CHECK(evts[7].span.offset == 2);
    CHECK(evts[7].span.length == 3);   // "b^c"
    // Outer + end_node at index 8
    CHECK(evts[8].kind == event_kind::end_node);
    CHECK(evts[8].span.offset == 0);
    CHECK(evts[8].span.length == 5);   // "a+b^c"
}

TEST_CASE("pratt stage5: structured — prefix '-a': correct tree and span",
          "[samasa][pratt][stage5]") {
    // minus(0) num(1)
    // Expected: begin prefix_expr, tok(0/-), tok(1/num), end prefix_expr
    // Span: hull({0,1},{1,1}) = {0,2}
    const auto evts = run_s5({TK5::minus, TK5::num});

    REQUIRE(evts.size() == 4);
    CHECK(evts[0].kind == event_kind::begin_node);
    CHECK(evts[0].syntax == PSK::prefix_expr);
    CHECK(evts[1].kind == event_kind::token);   // tok(-)
    CHECK(evts[2].kind == event_kind::token);   // tok(num)
    CHECK(evts[3].kind == event_kind::end_node);
    CHECK(evts[3].span.offset == 0);
    CHECK(evts[3].span.length == 2);   // "-a"
}

TEST_CASE("pratt stage5: structured — postfix 'a!': correct tree and span",
          "[samasa][pratt][stage5]") {
    // num(0) bang(1)
    // Expected: begin postfix_expr, tok(0/num), tok(1/!), end postfix_expr
    // Span: hull({0,1},{1,1}) = {0,2}
    const auto evts = run_s5({TK5::num, TK5::bang});

    REQUIRE(evts.size() == 4);
    CHECK(evts[0].kind == event_kind::begin_node);
    CHECK(evts[0].syntax == PSK::postfix_expr);
    CHECK(evts[1].kind == event_kind::token);   // tok(num)
    CHECK(evts[2].kind == event_kind::token);   // tok(!)
    CHECK(evts[3].kind == event_kind::end_node);
    CHECK(evts[3].span.offset == 0);
    CHECK(evts[3].span.length == 2);   // "a!"
}

TEST_CASE("pratt stage5: flat_pratt_action unchanged — same flat events as before fix",
          "[samasa][pratt][stage5]") {
    // flat_pratt_action for num+num^num must still emit exactly 5 token events,
    // no begin/end nodes — identical to pre-Stage-5 behavior.
    using FlatP = pratt_expression<Ops5, num5_rule, flat_pratt_action>;

    token_buffer<TK5>                  buf;
    std::uint32_t off = 0;
    for (TK5 k : {TK5::num, TK5::plus, TK5::num, TK5::caret, TK5::num}) {
        buf.data.push_back({k, off, 1, 0, 0}); ++off;
    }
    buf.data.push_back({TK5::eof, off, 0, 0, 0});

    token_stream<TK5>                      stream = buf.view();
    event_stream<PSK>                      events;
    lang::collecting_sink<diagnostic>      sink;
    lang::parse_tree_stats                 stats;
    parse_context<PSK, TK5>               ctx{stream, std::string_view{}, events, sink, stats};

    FlatP p{};
    auto r = p.match(ctx);
    REQUIRE(r.ok());

    // Flat: only token events, no begin/end nodes.
    const auto& all = events.all();
    CHECK(all.size() == 5);
    for (const auto& ev : all)
        CHECK(ev.kind == event_kind::token);
}

