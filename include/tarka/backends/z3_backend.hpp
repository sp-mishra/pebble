#pragma once
// =============================================================================
// tarka/backends/z3_backend.hpp — Z3 static solver backend
//
// Namespace:  tarka::backend
// Guard:      #if defined(HAS_Z3) && __has_include(<z3++.h>)
//
// Provides:   z3_backend — models SmtSolverBackend + CancelableBackend
//
// Design:
//   - Owns z3::context + z3::solver.
//   - Lowers Tarka Term → z3::expr via post-order walk cached in a
//     kosha::ShardedLRUCache<uint64_t, Z3_ast> — shared-DAG subterms lower once.
//   - Cancellation wired via z3::context::interrupt() registered as stop_token
//     callback; callback is one-shot (interrupt is advisory).
//   - All capabilities declared as ALL so it is always eligible in the router
//     (Z3 is a complete backend — handles any theory within its supported set).
// =============================================================================

#include "tarka/backend.hpp"
#include "tarka/context.hpp"

#if defined(HAS_Z3) && __has_include(<z3++.h>)

#include <z3++.h>
#include "containers/cache/kosha.hpp"

#include <cassert>
#include <cstdint>
#include <expected>
#include <span>
#include <stop_token>
#include <string>

namespace tarka::backend {
    class z3_backend {
    public:
        using native_term_t = z3::expr;
        using native_sort_t = z3::sort;

        z3_backend() : solver_(ctx_) {}

        [[nodiscard]] static constexpr theory_mask capabilities() noexcept {
            return theory_bit(theory_family::all);
        }

        // ------------------------------------------------------------------
        // Sort lowering
        // ------------------------------------------------------------------

        [[nodiscard]] native_sort_t lower_sort(Sort s) {
            switch (s.kind()) {
            case SortKind::Bool: return ctx_.bool_sort();
            case SortKind::Int: return ctx_.int_sort();
            case SortKind::Real: return ctx_.real_sort();
            case SortKind::BitVec: return ctx_.bv_sort(s.scalar_param());
            case SortKind::Array: {
                auto params = s.sort_params();
                assert(params.size() == 2);
                return ctx_.array_sort(lower_sort(params[0]), lower_sort(params[1]));
            }
            default:
                throw z3::exception("z3_backend: unsupported sort kind");
            }
        }

        // ------------------------------------------------------------------
        // Term lowering — post-order with ShardedLRUCache
        // ------------------------------------------------------------------

        [[nodiscard]] native_term_t lower_term(Term t) {
            return lower_impl(t);
        }

        // ------------------------------------------------------------------
        // Solver operations
        // ------------------------------------------------------------------

        void assert_formula(Term t) {
            solver_.add(lower_term(t));
        }

        [[nodiscard]] std::expected<SatResult, SmtError> check_sat() {
            try {
                const z3::check_result r = solver_.check();
                switch (r) {
                case z3::sat: return SatResult::Sat;
                case z3::unsat: return SatResult::Unsat;
                case z3::unknown: return SatResult::Unknown;
                }
            }
            catch (const z3::exception& e) {
                return std::unexpected(SmtError{SmtError::Kind::Internal, e.msg()});
            }
            return SatResult::Unknown;
        }

        [[nodiscard]] std::expected<SmtValue, SmtError> get_value(Term t) {
            try {
                z3::model m = solver_.get_model();
                z3::expr e = lower_term(t);
                z3::expr v = m.eval(e, true);
                return z3_expr_to_smtvalue(v, t.sort());
            }
            catch (const z3::exception& e) {
                return std::unexpected(SmtError{SmtError::Kind::Internal, e.msg()});
            }
        }

        void push(std::uint32_t n) { for (std::uint32_t i = 0; i < n; ++i) solver_.push(); }
        void pop(std::uint32_t n) { solver_.pop(n); }

        void reset() noexcept {
            solver_.reset();
            lower_cache_.clear();
        }

        // ------------------------------------------------------------------
        // Cancelable solve — registers stop_token callback to interrupt Z3
        // ------------------------------------------------------------------

        [[nodiscard]] std::expected<SatResult, SmtError>
        check_sat_cancelable(Term t, std::stop_token tok) {
            assert_formula(t);
            std::stop_callback cb{tok, [this]() noexcept { ctx_.interrupt(); }};
            return check_sat();
        }

        [[nodiscard]] std::size_t lower_cache_hits() const noexcept { return cache_hits_; }

    private:
        z3::context ctx_;
        z3::solver solver_;
        kosha::ShardedLRUCache<std::uint64_t, Z3_ast> lower_cache_{4096};
        std::size_t cache_hits_ = 0;

        // ------------------------------------------------------------------
        // Recursive post-order lowering with cache
        // ------------------------------------------------------------------

        [[nodiscard]] native_term_t lower_impl(Term t) {
            const std::uint64_t h = t.hash();

            if (auto r = lower_cache_.get(h); r.has_value()) {
                ++cache_hits_;
                return z3::expr(ctx_, r.value());
            }

            z3::expr result = lower_node(t);
            [[maybe_unused]] auto _ = lower_cache_.put(h, static_cast<Z3_ast>(result));
            return result;
        }

        [[nodiscard]] native_term_t lower_node(Term t) {
            const Op op = t.op();
            auto ch = t.children();
            auto sort = t.sort();

            auto lower_ch = [&](std::size_t i) { return lower_impl(ch[i]); };

            switch (op) {
            case Op::True: return ctx_.bool_val(true);
            case Op::False: return ctx_.bool_val(false);
            case Op::Lit: return lower_lit(t);
            case Op::Sym: return lower_sym(t);

            case Op::Not: return !lower_ch(0);
            case Op::And: {
                z3::expr_vector v(ctx_);
                for (auto c : ch) v.push_back(lower_impl(c));
                return z3::mk_and(v);
            }
            case Op::Or: {
                z3::expr_vector v(ctx_);
                for (auto c : ch) v.push_back(lower_impl(c));
                return z3::mk_or(v);
            }
            case Op::Xor: return z3::expr(ctx_, Z3_mk_xor(ctx_, lower_ch(0), lower_ch(1)));
            case Op::Implies: return z3::implies(lower_ch(0), lower_ch(1));
            case Op::Ite: return z3::ite(lower_ch(0), lower_ch(1), lower_ch(2));
            case Op::Eq: return lower_ch(0) == lower_ch(1);
            case Op::Distinct: return lower_ch(0) != lower_ch(1);

            case Op::Add: {
                z3::expr r = lower_ch(0);
                for (std::size_t i = 1; i < ch.size(); ++i) r = r + lower_ch(i);
                return r;
            }
            case Op::Sub: return lower_ch(0) - lower_ch(1);
            case Op::Mul: {
                z3::expr r = lower_ch(0);
                for (std::size_t i = 1; i < ch.size(); ++i) r = r * lower_ch(i);
                return r;
            }
            case Op::Div: return lower_ch(0) / lower_ch(1);
            case Op::Mod: return z3::mod(lower_ch(0), lower_ch(1));
            case Op::Neg: return -lower_ch(0);
            case Op::Lt: return lower_ch(0) < lower_ch(1);
            case Op::Le: return lower_ch(0) <= lower_ch(1);
            case Op::Gt: return lower_ch(0) > lower_ch(1);
            case Op::Ge: return lower_ch(0) >= lower_ch(1);

            case Op::BvAdd: return lower_ch(0) + lower_ch(1);
            case Op::BvSub: return lower_ch(0) - lower_ch(1);
            case Op::BvMul: return lower_ch(0) * lower_ch(1);
            case Op::BvUdiv: return z3::udiv(lower_ch(0), lower_ch(1));
            case Op::BvSdiv: return lower_ch(0) / lower_ch(1);
            case Op::BvUrem: return z3::urem(lower_ch(0), lower_ch(1));
            case Op::BvSrem: return z3::srem(lower_ch(0), lower_ch(1));
            case Op::BvNeg: return -lower_ch(0);
            case Op::BvAnd: return lower_ch(0) & lower_ch(1);
            case Op::BvOr: return lower_ch(0) | lower_ch(1);
            case Op::BvXor: return lower_ch(0) ^ lower_ch(1);
            case Op::BvNot: return ~lower_ch(0);
            case Op::BvShl: return z3::shl(lower_ch(0), lower_ch(1));
            case Op::BvLshr: return z3::lshr(lower_ch(0), lower_ch(1));
            case Op::BvAshr: return z3::ashr(lower_ch(0), lower_ch(1));
            case Op::BvUlt: return z3::ult(lower_ch(0), lower_ch(1));
            case Op::BvUle: return z3::ule(lower_ch(0), lower_ch(1));
            case Op::BvSlt: return lower_ch(0) < lower_ch(1);
            case Op::BvSle: return lower_ch(0) <= lower_ch(1);
            case Op::BvConcat: return z3::concat(lower_ch(0), lower_ch(1));
            case Op::BvExtract: {
                // payload_hash encodes high:low — recover from Context
                // Use t.ptr()->payload_hash as bit-packed (high<<16|low)
                const std::uint64_t ph = t.ptr()->payload_hash;
                const unsigned hi = static_cast<unsigned>(ph >> 32u);
                const unsigned lo = static_cast<unsigned>(ph & 0xFFFFFFFFu);
                return lower_ch(0).extract(hi, lo);
            }
            case Op::BvZeroExt: {
                const unsigned ext = static_cast<unsigned>(t.ptr()->payload_hash);
                return z3::zext(lower_ch(0), ext);
            }
            case Op::BvSignExt: {
                const unsigned ext = static_cast<unsigned>(t.ptr()->payload_hash);
                return z3::sext(lower_ch(0), ext);
            }

            case Op::Select: return z3::select(lower_ch(0), lower_ch(1));
            case Op::Store: return z3::store(lower_ch(0), lower_ch(1), lower_ch(2));

            default:
                throw z3::exception("z3_backend: unsupported op");
            }
        }

        [[nodiscard]] native_term_t lower_lit(Term t) {
            const Sort s = t.sort();
            const std::uint64_t ph = t.ptr()->payload_hash;

            if (s.kind() == SortKind::Bool)
                return ctx_.bool_val(ph != 0);

            if (s.kind() == SortKind::BitVec) {
                if (auto bv = t.ctx().bv_literal(ph))
                    return ctx_.bv_val(static_cast<std::uint64_t>(bv->bits), s.scalar_param());
            }

            if (s.kind() == SortKind::Int)
                return ctx_.int_val(static_cast<long long>(ph));

            throw z3::exception("z3_backend: unsupported literal sort");
        }

        [[nodiscard]] native_term_t lower_sym(Term t) {
            const std::string_view name = t.ctx().symbol_name(t.ptr()->payload_hash);
            return ctx_.constant(std::string{name}.c_str(), lower_sort(t.sort()));
        }

        [[nodiscard]] static SmtValue z3_expr_to_smtvalue(const z3::expr& e, Sort sort) {
            if (sort.kind() == SortKind::Bool)
                return e.is_true();

            if (sort.kind() == SortKind::BitVec) {
                std::uint64_t bits = e.get_numeral_uint64();
                return bv_value{bits, sort.scalar_param()};
            }

            if (sort.kind() == SortKind::Int) {
                return static_cast<std::int64_t>(e.get_numeral_int64());
            }

            // fallback
            return std::string{e.to_string()};
        }
    };

    static_assert(SmtSolverBackend<z3_backend>);
    static_assert(CancelableBackend<z3_backend>);
} // namespace tarka::backend

#endif // HAS_Z3
