#pragma once
// =============================================================================
// containers/numeric/exact_rational.hpp — Exact rational arithmetic
//
// Namespace:  containers::numeric
// Provides:   exact_rational — value-semantic exact rational number
//
// Design:
//   - Two-tier representation, no macros, no virtual:
//       * fast path: signed 128-bit num/den (gcd-reduced, den > 0)
//       * slow path: sign-magnitude bignum (std::vector<uint64_t> limbs)
//   - Every arithmetic op is overflow-checked on the 128-bit path
//     (__builtin_*_overflow); on overflow the operands promote to bignum,
//     the op is redone exactly, then the result demotes back to the fast path
//     when it fits. Callers therefore never observe silent wraparound.
//   - Header-only, self-contained (no internal-library dependency), so it can
//     be reused anywhere (SMT theories, LP, geometry predicates, …).
// =============================================================================

#include <cstdint>
#include <cstdlib>
#include <compare>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace containers::numeric {
    using i128 = __int128;
    using u128 = unsigned __int128;

    namespace detail {
        // ---- 128-bit helpers -------------------------------------------------

        [[nodiscard]] inline u128 abs128(const i128 v) noexcept {
            return v < 0 ? static_cast<u128>(-(v + 1)) + 1u : static_cast<u128>(v);
        }

        [[nodiscard]] inline u128 gcd_u128(u128 a, u128 b) noexcept {
            while (b != 0) {
                const u128 t = a % b;
                a = b;
                b = t;
            }
            return a;
        }

        // ---- sign-magnitude bignum (base 2^64 limbs, little-endian) ----------
        // Minimal, only what exact_rational needs: add, sub, mul, divmod,
        // compare, gcd, and conversion to/from i128 / decimal string.

        struct big {
            int sign = 0; // -1, 0, +1
            std::vector<std::uint64_t> mag; // little-endian magnitude, no trailing zeros

            big() = default;

            explicit big(const i128 v) {
                if (v == 0) {
                    sign = 0;
                    return;
                }
                sign = v < 0 ? -1 : 1;
                u128 m = abs128(v);
                while (m != 0) {
                    mag.push_back(static_cast<std::uint64_t>(m));
                    m >>= 64;
                }
            }

            [[nodiscard]] bool is_zero() const noexcept { return sign == 0; }

            void trim() {
                while (!mag.empty() && mag.back() == 0) mag.pop_back();
                if (mag.empty()) sign = 0;
            }

            // magnitude compare: -1 / 0 / +1
            [[nodiscard]] static int mag_cmp(const std::vector<std::uint64_t>& a,
                                             const std::vector<std::uint64_t>& b) noexcept {
                if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
                for (std::size_t i = a.size(); i-- > 0;) {
                    if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
                }
                return 0;
            }

            [[nodiscard]] static std::vector<std::uint64_t> mag_add(
                const std::vector<std::uint64_t>& a, const std::vector<std::uint64_t>& b) {
                std::vector<std::uint64_t> r;
                const std::size_t n = a.size() > b.size() ? a.size() : b.size();
                r.reserve(n + 1);
                u128 carry = 0;
                for (std::size_t i = 0; i < n; ++i) {
                    u128 cur = carry;
                    if (i < a.size()) cur += a[i];
                    if (i < b.size()) cur += b[i];
                    r.push_back(static_cast<std::uint64_t>(cur));
                    carry = cur >> 64;
                }
                if (carry) r.push_back(static_cast<std::uint64_t>(carry));
                return r;
            }

            // requires mag(a) >= mag(b)
            [[nodiscard]] static std::vector<std::uint64_t> mag_sub(
                const std::vector<std::uint64_t>& a, const std::vector<std::uint64_t>& b) {
                std::vector<std::uint64_t> r;
                r.reserve(a.size());
                std::int64_t borrow = 0;
                for (std::size_t i = 0; i < a.size(); ++i) {
                    u128 cur = static_cast<u128>(a[i]) - static_cast<u128>(borrow);
                    if (i < b.size()) {
                        if (a[i] < b[i] + static_cast<std::uint64_t>(borrow)) {
                            cur = (u128{1} << 64) + a[i] - b[i] - static_cast<u128>(borrow);
                            borrow = 1;
                        }
                        else {
                            cur = static_cast<u128>(a[i]) - b[i] - static_cast<u128>(borrow);
                            borrow = 0;
                        }
                    }
                    else {
                        borrow = (a[i] < static_cast<std::uint64_t>(borrow)) ? 1 : 0;
                    }
                    r.push_back(static_cast<std::uint64_t>(cur));
                }
                while (!r.empty() && r.back() == 0) r.pop_back();
                return r;
            }

            [[nodiscard]] static std::vector<std::uint64_t> mag_mul(
                const std::vector<std::uint64_t>& a, const std::vector<std::uint64_t>& b) {
                if (a.empty() || b.empty()) return {};
                std::vector<std::uint64_t> r(a.size() + b.size(), 0);
                for (std::size_t i = 0; i < a.size(); ++i) {
                    u128 carry = 0;
                    for (std::size_t j = 0; j < b.size(); ++j) {
                        const u128 cur = static_cast<u128>(a[i]) * b[j] + r[i + j] + carry;
                        r[i + j] = static_cast<std::uint64_t>(cur);
                        carry = cur >> 64;
                    }
                    r[i + b.size()] += static_cast<std::uint64_t>(carry);
                }
                while (!r.empty() && r.back() == 0) r.pop_back();
                return r;
            }

            [[nodiscard]] static big add(const big& a, const big& b) {
                if (a.sign == 0) return b;
                if (b.sign == 0) return a;
                big r;
                if (a.sign == b.sign) {
                    r.mag = mag_add(a.mag, b.mag);
                    r.sign = a.sign;
                }
                else {
                    const int c = mag_cmp(a.mag, b.mag);
                    if (c == 0) return big{};
                    if (c > 0) {
                        r.mag = mag_sub(a.mag, b.mag);
                        r.sign = a.sign;
                    }
                    else {
                        r.mag = mag_sub(b.mag, a.mag);
                        r.sign = b.sign;
                    }
                }
                r.trim();
                return r;
            }

            [[nodiscard]] static big neg(big a) {
                a.sign = -a.sign;
                return a;
            }

            [[nodiscard]] static big sub(const big& a, const big& b) { return add(a, neg(b)); }

            [[nodiscard]] static big mul(const big& a, const big& b) {
                if (a.sign == 0 || b.sign == 0) return big{};
                big r;
                r.mag = mag_mul(a.mag, b.mag);
                r.sign = a.sign * b.sign;
                r.trim();
                return r;
            }

            // Knuth-style long division by repeated bit shift-subtract.
            // Returns {quotient, remainder}, remainder has sign of dividend.
            [[nodiscard]] static std::pair<big, big> divmod(const big& a, const big& b) {
                // b != 0 assumed
                if (mag_cmp(a.mag, b.mag) < 0) {
                    return {big{}, a};
                }
                // bit-length of a
                const std::size_t abits = a.mag.size() * 64;
                std::vector<std::uint64_t> qmag(a.mag.size(), 0);
                std::vector<std::uint64_t> rem; // running remainder magnitude
                for (std::size_t bit = abits; bit-- > 0;) {
                    // rem <<= 1
                    rem = mag_mul(rem, {2});
                    // set low bit of rem to bit `bit` of a
                    if (const std::uint64_t abit = (a.mag[bit / 64] >> (bit % 64)) & 1u) {
                        if (rem.empty()) rem.push_back(1);
                        else rem[0] |= 1u;
                    }
                    if (mag_cmp(rem, b.mag) >= 0) {
                        rem = mag_sub(rem, b.mag);
                        qmag[bit / 64] |= (std::uint64_t{1} << (bit % 64));
                    }
                }
                big q;
                q.mag = std::move(qmag);
                q.sign = a.sign * b.sign;
                q.trim();
                big r;
                r.mag = std::move(rem);
                r.sign = a.sign;
                r.trim();
                return {std::move(q), std::move(r)};
            }

            [[nodiscard]] static big gcd(big a, big b) {
                a.sign = a.is_zero() ? 0 : 1;
                b.sign = b.is_zero() ? 0 : 1;
                while (!b.is_zero()) {
                    big r = divmod(a, b).second;
                    r.sign = r.is_zero() ? 0 : 1;
                    a = std::move(b);
                    b = std::move(r);
                }
                return a;
            }

            [[nodiscard]] int cmp(const big& o) const noexcept {
                if (sign != o.sign) return sign < o.sign ? -1 : 1;
                if (sign == 0) return 0;
                const int c = mag_cmp(mag, o.mag);
                return sign > 0 ? c : -c;
            }

            [[nodiscard]] std::optional<i128> to_i128() const noexcept {
                if (sign == 0) return i128{0};
                if (mag.size() > 2) return std::nullopt;
                u128 m = 0;
                for (std::size_t i = mag.size(); i-- > 0;) m = (m << 64) | mag[i];
                // max positive i128 magnitude is 2^127 - 1; max negative is 2^127
                const u128 lim = (u128{1} << 127);
                if (sign > 0 && m >= lim) return std::nullopt;
                if (sign < 0 && m > lim) return std::nullopt;
                if (sign < 0) {
                    if (m == lim) return i128{-1} << 127; // most-negative
                    return -static_cast<i128>(m);
                }
                return static_cast<i128>(m);
            }

            [[nodiscard]] std::string to_string() const {
                if (sign == 0) return "0";
                std::string digits;
                big cur = *this;
                cur.sign = 1;
                const big ten{i128{10}};
                while (!cur.is_zero()) {
                    auto [q, r] = divmod(cur, ten);
                    const std::uint64_t d = r.mag.empty() ? 0 : r.mag[0];
                    digits.push_back(static_cast<char>('0' + d));
                    cur = std::move(q);
                    cur.sign = cur.is_zero() ? 0 : 1;
                }
                if (sign < 0) digits.push_back('-');
                return {digits.rbegin(), digits.rend()};
            }
        };
    } // namespace detail

    // =========================================================================
    // exact_rational
    // =========================================================================

    class exact_rational {
    public:
        // ---- construction ----------------------------------------------------

        exact_rational() noexcept : num_(0), den_(1), big_(false) {}

        exact_rational(const i128 v) noexcept : num_(v), den_(1), big_(false) {} // NOLINT(google-explicit-constructor)

        exact_rational(const std::int64_t n, const std::int64_t d) : num_(n), den_(d), big_(false) {
            normalize_small();
        }

        // ---- queries ---------------------------------------------------------

        [[nodiscard]] int sign() const noexcept {
            if (big_) return bnum_.sign;
            return num_ < 0 ? -1 : (num_ > 0 ? 1 : 0);
        }

        [[nodiscard]] bool is_zero() const noexcept { return sign() == 0; }

        [[nodiscard]] bool is_integer() const noexcept {
            if (big_) return bden_.cmp(detail::big{i128{1}}) == 0;
            return den_ == 1;
        }

        [[nodiscard]] exact_rational abs() const {
            exact_rational r = *this;
            if (r.sign() < 0) r = -r;
            return r;
        }

        [[nodiscard]] exact_rational reciprocal() const {
            exact_rational r = *this;
            if (!big_) {
                std::swap(r.num_, r.den_);
                r.normalize_small();
                return r;
            }
            std::swap(r.bnum_, r.bden_);
            r.normalize_big();
            return r;
        }

        // floor(n/d) as an exact_rational integer
        [[nodiscard]] exact_rational floor() const {
            if (is_integer()) return *this;
            if (!big_) {
                i128 q = num_ / den_;
                if ((num_ % den_ != 0) && ((num_ < 0) != (den_ < 0))) --q;
                return exact_rational{q};
            }
            auto [q, r] = detail::big::divmod(bnum_, bden_);
            if (!r.is_zero() && (bnum_.sign * bden_.sign < 0)) {
                q = detail::big::sub(q, detail::big{i128{1}});
            }
            return from_big_int(q);
        }

        [[nodiscard]] exact_rational ceil() const {
            if (is_integer()) return *this;
            return -((-(*this)).floor()); // ceil(x) = -floor(-x)
        }

        // exact int64 if it fits and is integral
        [[nodiscard]] std::optional<std::int64_t> to_int64() const {
            if (!is_integer()) return std::nullopt;
            if (!big_) {
                if (num_ > static_cast<i128>(INT64_MAX) || num_ < static_cast<i128>(INT64_MIN))
                    return std::nullopt;
                return static_cast<std::int64_t>(num_);
            }
            const auto v = bnum_.to_i128();
            if (!v) return std::nullopt;
            if (*v > static_cast<i128>(INT64_MAX) || *v < static_cast<i128>(INT64_MIN))
                return std::nullopt;
            return static_cast<std::int64_t>(*v);
        }

        // {num, den} as int64 pair if both fit; else nullopt (use to_string)
        [[nodiscard]] std::optional<std::pair<std::int64_t, std::int64_t>>
        to_int64_pair() const {
            if (!big_) {
                if (num_ <= static_cast<i128>(INT64_MAX) && num_ >= static_cast<i128>(INT64_MIN) &&
                    den_ <= static_cast<i128>(INT64_MAX) && den_ >= static_cast<i128>(INT64_MIN))
                    return std::pair{static_cast<std::int64_t>(num_), static_cast<std::int64_t>(den_)};
                return std::nullopt;
            }
            const auto n = bnum_.to_i128();
            const auto d = bden_.to_i128();
            if (!n || !d) return std::nullopt;
            if (*n > static_cast<i128>(INT64_MAX) || *n < static_cast<i128>(INT64_MIN) ||
                *d > static_cast<i128>(INT64_MAX) || *d < static_cast<i128>(INT64_MIN))
                return std::nullopt;
            return std::pair{static_cast<std::int64_t>(*n), static_cast<std::int64_t>(*d)};
        }

        [[nodiscard]] std::string to_string() const {
            if (!big_) {
                std::string s = i128_to_string(num_);
                if (den_ != 1) {
                    s.push_back('/');
                    s += i128_to_string(den_);
                }
                return s;
            }
            std::string s = bnum_.to_string();
            if (!is_integer()) {
                s.push_back('/');
                s += bden_.to_string();
            }
            return s;
        }

        // ---- unary -----------------------------------------------------------

        [[nodiscard]] exact_rational operator-() const {
            exact_rational r = *this;
            if (r.big_) r.bnum_.sign = -r.bnum_.sign;
            else r.num_ = -r.num_;
            return r;
        }

        // ---- arithmetic ------------------------------------------------------

        [[nodiscard]] exact_rational operator+(const exact_rational& o) const {
            if (!big_ && !o.big_) {
                i128 n2d1, n, d;
                if (i128 n1d2; !mul_ovf(num_, o.den_, n1d2) && !mul_ovf(o.num_, den_, n2d1) &&
                    !add_ovf(n1d2, n2d1, n) && !mul_ovf(den_, o.den_, d)) {
                    return exact_rational{n, d, tag_small{}};
                }
            }
            return add_big(*this, o);
        }

        [[nodiscard]] exact_rational operator-(const exact_rational& o) const {
            return *this + (-o);
        }

        [[nodiscard]] exact_rational operator*(const exact_rational& o) const {
            if (!big_ && !o.big_) {
                i128 d;
                if (i128 n; !mul_ovf(num_, o.num_, n) && !mul_ovf(den_, o.den_, d)) {
                    return exact_rational{n, d, tag_small{}};
                }
            }
            return mul_big(*this, o);
        }

        [[nodiscard]] exact_rational operator/(const exact_rational& o) const {
            return *this * o.reciprocal();
        }

        exact_rational& operator+=(const exact_rational& o) { return *this = *this + o; }
        exact_rational& operator-=(const exact_rational& o) { return *this = *this - o; }
        exact_rational& operator*=(const exact_rational& o) { return *this = *this * o; }
        exact_rational& operator/=(const exact_rational& o) { return *this = *this / o; }

        // ---- comparison ------------------------------------------------------

        [[nodiscard]] std::strong_ordering operator<=>(const exact_rational& o) const {
            const exact_rational diff = *this - o;
            const int s = diff.sign();
            if (s < 0) return std::strong_ordering::less;
            if (s > 0) return std::strong_ordering::greater;
            return std::strong_ordering::equal;
        }

        [[nodiscard]] bool operator==(const exact_rational& o) const {
            return (*this <=> o) == std::strong_ordering::equal;
        }

    private:
        struct tag_small {};

        // fast-path fields (valid when !big_)
        i128 num_;
        i128 den_; // always > 0 on fast path
        bool big_;

        // slow-path fields (valid when big_)
        detail::big bnum_;
        detail::big bden_; // always sign +1, magnitude >= 1

        exact_rational(const i128 n, const i128 d, tag_small) : num_(n), den_(d), big_(false) {
            normalize_small();
        }

        // ---- overflow-checked 128-bit primitives -----------------------------

        [[nodiscard]] static bool mul_ovf(const i128 a, const i128 b, i128& out) noexcept {
            return __builtin_mul_overflow(a, b, &out);
        }

        [[nodiscard]] static bool add_ovf(const i128 a, const i128 b, i128& out) noexcept {
            return __builtin_add_overflow(a, b, &out);
        }

        [[nodiscard]] static i128 gcd_i128(const i128 a, const i128 b) noexcept {
            const u128 x = detail::abs128(a);
            const u128 y = detail::abs128(b);
            const u128 g = detail::gcd_u128(x, y);
            return static_cast<i128>(g);
        }

        void normalize_small() noexcept {
            if (den_ == 0) return; // undefined; leave as-is (caller error)
            if (den_ < 0) {
                num_ = -num_;
                den_ = -den_;
            }
            if (num_ == 0) {
                den_ = 1;
                return;
            }
            if (const i128 g = gcd_i128(num_, den_); g > 1) {
                num_ /= g;
                den_ /= g;
            }
        }

        void normalize_big() {
            if (bden_.sign < 0) {
                bnum_.sign = -bnum_.sign;
                bden_.sign = 1;
            }
            if (bnum_.is_zero()) {
                bden_ = detail::big{i128{1}};
                return;
            }
            if (const detail::big g = detail::big::gcd(bnum_, bden_); g.cmp(detail::big{i128{1}}) > 0) {
                bnum_ = detail::big::divmod(bnum_, g).first;
                bden_ = detail::big::divmod(bden_, g).first;
            }
            try_demote();
        }

        // Demote a big value back to fast path when both parts fit i128.
        void try_demote() {
            if (!big_) return;
            const auto n = bnum_.to_i128();
            if (const auto d = bden_.to_i128(); n && d) {
                num_ = *n;
                den_ = *d;
                big_ = false;
                bnum_ = detail::big{};
                bden_ = detail::big{};
            }
        }

        [[nodiscard]] detail::big num_big() const {
            return big_ ? bnum_ : detail::big{num_};
        }

        [[nodiscard]] detail::big den_big() const {
            return big_ ? bden_ : detail::big{den_};
        }

        [[nodiscard]] static exact_rational from_big(detail::big n, detail::big d) {
            exact_rational r;
            r.big_ = true;
            r.bnum_ = std::move(n);
            r.bden_ = std::move(d);
            r.normalize_big();
            return r;
        }

        [[nodiscard]] static exact_rational from_big_int(detail::big n) {
            return from_big(std::move(n), detail::big{i128{1}});
        }

        [[nodiscard]] static exact_rational add_big(const exact_rational& a, const exact_rational& b) {
            const detail::big an = a.num_big(), ad = a.den_big();
            const detail::big bn = b.num_big(), bd = b.den_big();
            detail::big n = detail::big::add(detail::big::mul(an, bd), detail::big::mul(bn, ad));
            detail::big d = detail::big::mul(ad, bd);
            return from_big(std::move(n), std::move(d));
        }

        [[nodiscard]] static exact_rational mul_big(const exact_rational& a, const exact_rational& b) {
            detail::big n = detail::big::mul(a.num_big(), b.num_big());
            detail::big d = detail::big::mul(a.den_big(), b.den_big());
            return from_big(std::move(n), std::move(d));
        }

        [[nodiscard]] static std::string i128_to_string(const i128 v) {
            return detail::big{v}.to_string();
        }
    };
} // namespace containers::numeric
