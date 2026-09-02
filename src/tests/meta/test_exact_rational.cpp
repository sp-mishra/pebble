// =============================================================================
// test_exact_rational.cpp — containers::numeric::exact_rational
//
// Tests:
//   1. Fast-path arithmetic + gcd reduction.
//   2. Overflow promotion to bignum and exact result.
//   3. Demotion back to fast path after reduction.
//   4. Exact comparison (<=>) across mixed representations.
//   5. floor / ceil / to_int64 / to_int64_pair / to_string.
// =============================================================================

#include "catch_amalgamated.hpp"

#include "containers/numeric/exact_rational.hpp"

using containers::numeric::exact_rational;
using i128 = containers::numeric::i128;

TEST_CASE (



"exact_rational: fast-path arithmetic and reduction"
,
"[numeric][exact_rational]"
)
 {
    exact_rational a{1, 2};
    exact_rational b{1, 3};
    REQUIRE((a + b) == exact_rational{5, 6});
    REQUIRE((a - b) == exact_rational{1, 6});
    REQUIRE((a * b) == exact_rational{1, 6});
    REQUIRE((a / b) == exact_rational{3, 2});

    // reduction: 2/4 == 1/2
    REQUIRE(exact_rational{2, 4} == exact_rational{1, 2});
    // negative denominator normalizes to numerator sign
    REQUIRE(exact_rational{1, -2} == exact_rational{-1, 2});
    REQUIRE(exact_rational{0, 5}.is_zero());
    REQUIRE(exact_rational{0, 5}.sign() == 0);
}

TEST_CASE (



"exact_rational: overflow promotes to bignum exactly"
,
"[numeric][exact_rational]"
)
 {
    // near-i128-max integers whose product overflows 128 bits
    const i128 big = (i128{1} << 100) + 7;
    exact_rational x{big};
    exact_rational y{big};
    exact_rational p = x * y; // = big^2, does not fit in i128

    // p should be exact: p / big == big
    REQUIRE((p / exact_rational{big}) == exact_rational{big});
    REQUIRE(p.is_integer());
    REQUIRE(p.sign() == 1);

    // p + 0 unchanged; p - p == 0
    REQUIRE((p - p).is_zero());

    // string is a plain decimal (no fraction bar)
    const std::string s = p.to_string();
    REQUIRE(s.find('/') == std::string::npos);
    REQUIRE(s.size() > 30); // big^2 has ~60 decimal digits
}

TEST_CASE (



"exact_rational: demotion back to fast path"
,
"[numeric][exact_rational]"
)
 {
    const i128 big = (i128{1} << 100);
    exact_rational p = exact_rational{big} * exact_rational{big}; // bignum
    // divide back down to something small; result must demote and equal 1
    exact_rational q = p / p;
    REQUIRE(q == exact_rational{1});
    REQUIRE(q.to_int64().has_value());
    REQUIRE(*q.to_int64() == 1);
}

TEST_CASE (



"exact_rational: comparison across representations"
,
"[numeric][exact_rational]"
)
 {
    exact_rational small{3, 4};
    const i128 big = (i128{1} << 100);
    exact_rational large = exact_rational{big} * exact_rational{big};
    REQUIRE(small < large);
    REQUIRE(large > small);
    REQUIRE(( -large) < small);
    REQUIRE((small <=> small) == std::strong_ordering::equal);
    REQUIRE((exact_rational{2, 3} <=> exact_rational{3, 4}) == std::strong_ordering::less);
}

TEST_CASE (



"exact_rational: floor / ceil / conversions"
,
"[numeric][exact_rational]"
)
 {
    REQUIRE(exact_rational{7, 2}.floor() == exact_rational{3});
    REQUIRE(exact_rational{7, 2}.ceil() == exact_rational{4});
    REQUIRE(exact_rational{-7, 2}.floor() == exact_rational{-4});
    REQUIRE(exact_rational{-7, 2}.ceil() == exact_rational{-3});
    REQUIRE(exact_rational{6, 3}.floor() == exact_rational{2});
    REQUIRE(exact_rational{6, 3}.is_integer());

    REQUIRE(exact_rational{5}.to_int64() == std::optional<std::int64_t>{5});
    REQUIRE_FALSE(exact_rational{1, 2}.to_int64().has_value());

    auto pr = exact_rational{7, 2}.to_int64_pair();
    REQUIRE(pr.has_value());
    REQUIRE(pr->first == 7);
    REQUIRE(pr->second == 2);

    REQUIRE(exact_rational{1, 2}.reciprocal() == exact_rational{2});
    REQUIRE(exact_rational{-3, 4}.abs() == exact_rational{3, 4});
    REQUIRE(exact_rational{7, 2}.to_string() == "7/2");
    REQUIRE(exact_rational{-3}.to_string() == "-3");
}
