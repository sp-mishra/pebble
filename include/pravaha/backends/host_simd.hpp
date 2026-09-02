#pragma once
// ============================================================================
// backends/host_simd.hpp - CPU SIMD backend for the Pravaha hetero overlay.
//   NOT a standalone header — must be included from pravaha_hetero.hpp after
//   compute types, expr tags, NADI emitters are in scope.
//   Owns all Highway includes (hwy/highway.h, aligned_allocator, math-inl).
//   Extracted from pravaha_hetero.hpp: pravaha::backends::simd_detail and
//   pravaha::backends host_simd_backend + dispatcher helpers.
// ============================================================================

#include <hwy/highway.h>
#include <hwy/aligned_allocator.h>
#include <hwy/contrib/math/math-inl.h>  // hn::Sin/Cos/Exp/Log for math-tag SIMD

namespace pravaha::backends {
    namespace lithe = ::vakya;
}

// ============================================================================
// Part 2: simd_detail — fused element-wise / reduction kernels (Highway)
// ============================================================================

namespace pravaha::backends::simd_detail {
    namespace hn = hwy::HWY_NAMESPACE;

    // Evaluate one Lithe expression node into a Highway vector.
    // Multi-input model: `xs` holds one loaded vector per input slot. call_tag and
    // input<0> both read slot 0; input<N> reads slot N. lit_node broadcasts its value.
    template <typename D, std::size_t K, typename E>
    [[nodiscard]] HWY_INLINE hn::Vec<D>

    eval_vec(const D& d, const std::array<hn::Vec<D>, K>& xs, const E& expr) {
        using node_t = std::decay_t<E>;
        using tag = typename node_t::tag_type;
        using namespace lithe;

        if constexpr (std::is_same_v<tag, call_tag>) {
            return xs[0];
        }
        else if constexpr (pravaha::expr::input_tag_index<tag>::value) {
            return xs[pravaha::expr::input_tag_index<tag>::index];
        }
        else if constexpr (std::is_same_v<tag, pravaha::expr::lit_tag>) {
            using T = hn::TFromD<D>;
            if constexpr (requires { expr.value; })
                return hn::Set(d, static_cast<T>(expr.value));
            else
                return hn::Zero(d);
        }
        else if constexpr (std::is_same_v<tag, neg_tag>) {
            return hn::Neg(eval_vec(d, xs, std::get < 0 > (expr.children)));
        }
        else if constexpr (std::is_same_v<tag, pravaha::expr::sqrt_tag>) {
            return hn::Sqrt(eval_vec(d, xs, std::get < 0 > (expr.children)));
        }
        else if constexpr (std::is_same_v<tag, pravaha::expr::abs_tag>) {
            return hn::Abs(eval_vec(d, xs, std::get < 0 > (expr.children)));
        }
        else if constexpr (std::is_same_v<tag, pravaha::expr::exp_tag>) {
            return hn::Exp(d, eval_vec(d, xs, std::get < 0 > (expr.children)));
        }
        else if constexpr (std::is_same_v<tag, pravaha::expr::log_tag>) {
            return hn::Log(d, eval_vec(d, xs, std::get < 0 > (expr.children)));
        }
        else if constexpr (std::is_same_v<tag, pravaha::expr::sin_tag>) {
            return hn::Sin(d, eval_vec(d, xs, std::get < 0 > (expr.children)));
        }
        else if constexpr (std::is_same_v<tag, pravaha::expr::cos_tag>) {
            return hn::Cos(d, eval_vec(d, xs, std::get < 0 > (expr.children)));
        }
        else {
            static_assert(lithe::emit::tag_descriptor<tag>::arity == 2,
                          "eval_vec: binary branch reached with non-binary tag");
            auto a = eval_vec(d, xs, std::get < 0 > (expr.children));
            auto b = eval_vec(d, xs, std::get < 1 > (expr.children));
            if constexpr (std::is_same_v<tag, add_tag>) return hn::Add(a, b);
            else if constexpr (std::is_same_v<tag, sub_tag>) return hn::Sub(a, b);
            else if constexpr (std::is_same_v<tag, mul_tag>) return hn::Mul(a, b);
            else if constexpr (std::is_same_v<tag, div_tag>) return hn::Div(a, b);
            else static_assert(sizeof(E) == 0, "simd backend: unsupported tag (use fallback)");
        }
    }

    // Scalar equivalent for the loop tail. `xs` holds one scalar per input slot.
    template <typename T, std::size_t K, typename E>
    [[nodiscard]] HWY_INLINE T eval_scalar(const std::array<T, K>& xs, const E& expr) {
        using node_t = std::decay_t<E>;
        using tag = typename node_t::tag_type;
        using namespace lithe;
        if constexpr (std::is_same_v<tag, call_tag>) return xs[0];
        else if constexpr (pravaha::expr::input_tag_index<tag>::value)
            return xs[pravaha::expr::input_tag_index<tag>::index];
        else if constexpr (std::is_same_v<tag, pravaha::expr::lit_tag>) {
            if constexpr (requires { expr.value; })
                return static_cast<T>(expr.value);
            else
                return T{};
        }
        else if constexpr (std::is_same_v<tag, neg_tag>)
            return -eval_scalar(xs, std::get < 0 > (expr.children));
        else if constexpr (std::is_same_v<tag, expr::sqrt_tag>)
            return static_cast<T>(std::sqrt(eval_scalar(xs, std::get < 0 > (expr.children))));
        else if constexpr (std::is_same_v<tag, expr::exp_tag>)
            return static_cast<T>(std::exp(eval_scalar(xs, std::get < 0 > (expr.children))));
        else if constexpr (std::is_same_v<tag, expr::log_tag>)
            return static_cast<T>(std::log(eval_scalar(xs, std::get < 0 > (expr.children))));
        else if constexpr (std::is_same_v<tag, expr::sin_tag>)
            return static_cast<T>(std::sin(eval_scalar(xs, std::get < 0 > (expr.children))));
        else if constexpr (std::is_same_v<tag, expr::cos_tag>)
            return static_cast<T>(std::cos(eval_scalar(xs, std::get < 0 > (expr.children))));
        else if constexpr (std::is_same_v<tag, expr::abs_tag>)
            return static_cast<T>(std::abs(eval_scalar(xs, std::get < 0 > (expr.children))));
        else {
            static_assert(lithe::emit::tag_descriptor<tag>::arity == 2,
                          "eval_scalar: binary branch reached with non-binary tag");
            T a = eval_scalar(xs, std::get < 0 > (expr.children));
            T b = eval_scalar(xs, std::get < 1 > (expr.children));
            if constexpr (std::is_same_v<tag, add_tag>) return a + b;
            else if constexpr (std::is_same_v<tag, sub_tag>) return a - b;
            else if constexpr (std::is_same_v<tag, mul_tag>) return a * b;
            else if constexpr (std::is_same_v<tag, div_tag>) return a / b;
            else static_assert(sizeof(E) == 0, "simd backend: unsupported tag");
        }
    }

    // SIMD-supported tag set — the ONLY backend-specific part; Highway op bodies live below.
    template <typename N>
    struct simd_tag_ok {
        using tag = typename N::tag_type;
        static constexpr bool value =
            std::is_same_v<tag, lithe::add_tag> || std::is_same_v<tag, lithe::sub_tag> ||
            std::is_same_v<tag, lithe::mul_tag> || std::is_same_v<tag, lithe::div_tag> ||
            std::is_same_v<tag, lithe::neg_tag> || std::is_same_v<tag, lithe::call_tag> ||
            std::is_same_v<tag, pravaha::expr::lit_tag> ||
            std::is_same_v<tag, pravaha::expr::sqrt_tag> ||
            std::is_same_v<tag, pravaha::expr::exp_tag> ||
            std::is_same_v<tag, pravaha::expr::log_tag> ||
            std::is_same_v<tag, pravaha::expr::sin_tag> ||
            std::is_same_v<tag, pravaha::expr::cos_tag> ||
            std::is_same_v<tag, pravaha::expr::abs_tag> ||
            pravaha::expr::input_tag_index<tag>::value;
    };

    // Compile-time check: every node in the tree uses a SIMD-supported tag.
    template <typename E>
    consteval bool is_simd_capable() {
        return lithe::tree::all_tags_satisfy<E, simd_tag_ok>();
    }

    // Compile-time: number of distinct input slots a tree references (max slot + 1).
    // call_tag counts as slot 0. lit/math/arithmetic nodes contribute nothing.
    struct slot_contrib {
        template <typename N>
        consteval std::size_t operator()() const {
            using tag = typename N::tag_type;
            if constexpr (pravaha::expr::input_tag_index<tag>::value)
                return pravaha::expr::input_tag_index<tag>::index + 1;
            else if constexpr (std::is_same_v<tag, lithe::call_tag>)
                return 1;
            else
                return 0;
        }
    };

    template <typename E>
    consteval std::size_t input_slot_count() {
        return lithe::tree::fold<E>(
            slot_contrib{},
            [](std::size_t a, std::size_t b) consteval { return a > b ? a : b; },
            std::size_t{0});
    }

    // ----------------------------------------------------------------------------
    // Reductions (Part E). Fold an element-wise child expression over all elements
    // of a single input buffer into one scalar.
    // ----------------------------------------------------------------------------

    template <pravaha::expr::reduce_op Op, typename T>
    [[nodiscard]] constexpr T reduce_identity() noexcept {
        if constexpr (Op == pravaha::expr::reduce_op::sum) return T{0};
        else if constexpr (Op == pravaha::expr::reduce_op::max) return std::numeric_limits<T>::lowest();
        else return std::numeric_limits<T>::max();
    }

    // Wide accumulator for sum reductions: f32 sums promote to f64 internally so
    // large-N reductions stay accurate (naive f32 accumulation over ~1e8 terms
    // loses everything below the running magnitude). max/min never lose precision,
    // so they accumulate in the native element type. The reduction still *returns*
    // T — only the running total is wider.
    template <pravaha::expr::reduce_op Op, typename T>
    using sum_accum_t =
    std::conditional_t<Op == pravaha::expr::reduce_op::sum && std::is_same_v<T, float>,
                       double, T>;

    template <pravaha::expr::reduce_op Op, typename T>
    [[nodiscard]] constexpr T reduce_combine(T a, T b) noexcept {
        if constexpr (Op == pravaha::expr::reduce_op::sum) return a + b;
        else if constexpr (Op == pravaha::expr::reduce_op::max) return a > b ? a : b;
        else return a < b ? a : b;
    }

    template <pravaha::expr::reduce_op Op, typename D>
    [[nodiscard]] HWY_INLINE hn::Vec<D> reduce_combine_vec(const D& d, hn::Vec<D> a, hn::Vec<D> b) {
        (void)d;
        if constexpr (Op == pravaha::expr::reduce_op::sum) return hn::Add(a, b);
        else if constexpr (Op == pravaha::expr::reduce_op::max) return hn::Max(a, b);
        else return hn::Min(a, b);
    }

    template <pravaha::expr::reduce_op Op, typename D>
    [[nodiscard]] HWY_INLINE hn::TFromD<D> reduce_horizontal(const D& d, hn::Vec<D> acc) {
        if constexpr (Op == pravaha::expr::reduce_op::sum) return hn::ReduceSum(d, acc);
        else if constexpr (Op == pravaha::expr::reduce_op::max) return hn::ReduceMax(d, acc);
        else return hn::ReduceMin(d, acc);
    }

    template <pravaha::expr::reduce_op Op, typename T, typename Child>
    [[nodiscard]] T reduce_simd(const Child& child, const T* src, std::size_t n,
                                compute::dim_t istride) {
        using A = sum_accum_t<Op, T>; // f64 for f32 sum; native T otherwise
        if (n == 0) return static_cast<T>(reduce_identity<Op, A>());

        if (istride != 1) {
            {
                const hn::ScalableTag<T> d;
                using DSigned = hn::RebindToSigned<decltype(d)>;
                using idx_t = hn::TFromD<DSigned>;
                const DSigned di;
                const std::size_t L = hn::Lanes(d);

                if (istride <= static_cast<compute::dim_t>(
                    std::numeric_limits<idx_t>::max() / 4)) {
                    [[maybe_unused]] hn::Vec<decltype(d)> vacc = hn::Set(d, reduce_identity<Op, T>());
                    A acc = reduce_identity<Op, A>();
                    std::size_t i = 0;
                    for (; i + L <= n; i += L) {
                        auto vidx = hn::Mul(hn::Iota(di, static_cast<idx_t>(i)),
                                            hn::Set(di, static_cast<idx_t>(istride)));
                        std::array<hn::Vec<decltype(d)>, 1> xs{hn::GatherIndex(d, src, vidx)};
                        auto v = eval_vec(d, xs, child);
                        if constexpr (std::is_same_v<A, T>) {
                            vacc = reduce_combine_vec<Op>(d, vacc, v);
                        }
                        else {
                            acc = reduce_combine<Op>(acc, static_cast<A>(reduce_horizontal<Op>(d, v)));
                        }
                    }
                    if constexpr (std::is_same_v<A, T>)
                        acc = static_cast<A>(reduce_horizontal<Op>(d, vacc));
                    // Strided partial tail — GatherIndex has no N-variant; scalar fallback.
                    std::array<T, 1> xs{};
                    for (; i < n; ++i) {
                        xs[0] = src[i * istride];
                        acc = reduce_combine<Op>(acc, static_cast<A>(eval_scalar(xs, child)));
                    }
                    return static_cast<T>(acc);
                }
            }
            A acc = reduce_identity<Op, A>();
            std::array<T, 1> xs{};
            for (std::size_t i = 0; i < n; ++i) {
                xs[0] = src[i * istride];
                acc = reduce_combine<Op>(acc, static_cast<A>(eval_scalar(xs, child)));
            }
            return static_cast<T>(acc);
        }

        const hn::ScalableTag<T> d;
        const std::size_t L = hn::Lanes(d);
        [[maybe_unused]] hn::Vec<decltype(d)> vacc = hn::Set(d, reduce_identity<Op, T>());
        A acc = reduce_identity<Op, A>();
        std::size_t i = 0;
        for (; i + L <= n; i += L) {
            std::array<hn::Vec<decltype(d)>, 1> xs{hn::LoadU(d, src + i)};
            auto v = eval_vec(d, xs, child);
            if constexpr (std::is_same_v<A, T>) {
                vacc = reduce_combine_vec<Op>(d, vacc, v);
            }
            else {
                acc = reduce_combine<Op>(acc, static_cast<A>(reduce_horizontal<Op>(d, v)));
            }
        }
        if constexpr (std::is_same_v<A, T>)
            acc = static_cast<A>(reduce_horizontal<Op>(d, vacc));

        // Phase 2.3: masked tail via LoadN — no scalar cleanup for contiguous path.
        const std::size_t remaining = n - i;
        if (remaining > 0) {
            std::array<hn::Vec<decltype(d)>, 1> xs{hn::LoadN(d, src + i, remaining)};
            auto v = eval_vec(d, xs, child);
            // Only the first `remaining` lanes are valid; reduce_horizontal over full
            // vector is safe for sum (neutral 0s don't change sum), but max/min need
            // scalar tail because extra lanes hold uninitialized/zero identity values.
            if constexpr (Op == pravaha::expr::reduce_op::sum) {
                acc = reduce_combine<Op>(acc, static_cast<A>(reduce_horizontal<Op>(d, v)));
            }
            else {
                std::array<T, 1> xs_s{};
                for (std::size_t j = 0; j < remaining; ++j) {
                    xs_s[0] = src[i + j];
                    acc = reduce_combine<Op>(acc, static_cast<A>(eval_scalar(xs_s, child)));
                }
            }
        }
        return static_cast<T>(acc);
    }
} // namespace pravaha::backends::simd_detail

// Multi-input reduce (Part G5). K input slots, mirrors element-wise multi-input.
namespace pravaha::backends::simd_detail {
    template <pravaha::expr::reduce_op Op, typename T, std::size_t K, typename Child>
    [[nodiscard]] T reduce_simd_multi(const Child& child,
                                      const std::array<const T*, K>& srcs,
                                      const std::array<compute::dim_t, K>& istrides,
                                      std::size_t n) {
        using A = sum_accum_t<Op, T>; // f64 for f32 sum; native T otherwise
        if (n == 0) return static_cast<T>(reduce_identity<Op, A>());

        bool all_contig = true;
        for (std::size_t s = 0; s < K; ++s)
            if (istrides[s] != 1) {
                all_contig = false;
                break;
            }

        if (all_contig) {
            const hn::ScalableTag<T> d;
            const std::size_t L = hn::Lanes(d);
            [[maybe_unused]] hn::Vec<decltype(d)> vacc = hn::Set(d, reduce_identity<Op, T>());
            A acc = reduce_identity<Op, A>();
            std::size_t i = 0;
            for (; i + L <= n; i += L) {
                std::array<hn::Vec<decltype(d)>, K> xs;
                for (std::size_t s = 0; s < K; ++s) xs[s] = hn::LoadU(d, srcs[s] + i);
                auto v = eval_vec(d, xs, child);
                if constexpr (std::is_same_v<A, T>) {
                    vacc = reduce_combine_vec<Op>(d, vacc, v);
                }
                else {
                    acc = reduce_combine<Op>(acc, static_cast<A>(reduce_horizontal<Op>(d, v)));
                }
            }
            if constexpr (std::is_same_v<A, T>)
                acc = static_cast<A>(reduce_horizontal<Op>(d, vacc));

            // Phase 2.3: masked tail via LoadN (contiguous multi-reduce path).
            const std::size_t remaining = n - i;
            if (remaining > 0) {
                std::array<hn::Vec<decltype(d)>, K> xs;
                for (std::size_t s = 0; s < K; ++s)
                    xs[s] = hn::LoadN(d, srcs[s] + i, remaining);
                auto v = eval_vec(d, xs, child);
                if constexpr (Op == pravaha::expr::reduce_op::sum) {
                    acc = reduce_combine<Op>(acc, static_cast<A>(reduce_horizontal<Op>(d, v)));
                }
                else {
                    std::array<T, K> xs_s{};
                    for (std::size_t j = 0; j < remaining; ++j) {
                        for (std::size_t s = 0; s < K; ++s) xs_s[s] = srcs[s][i + j];
                        acc = reduce_combine<Op>(acc, static_cast<A>(eval_scalar(xs_s, child)));
                    }
                }
            }
            return static_cast<T>(acc);
        }

        A acc = reduce_identity<Op, A>();
        std::array<T, K> xs{};
        for (std::size_t i = 0; i < n; ++i) {
            for (std::size_t s = 0; s < K; ++s) xs[s] = srcs[s][i * istrides[s]];
            acc = reduce_combine<Op>(acc, static_cast<A>(eval_scalar(xs, child)));
        }
        return static_cast<T>(acc);
    }
} // namespace pravaha::backends::simd_detail

// ============================================================================
// host_simd_backend and dispatcher helpers
// ============================================================================

namespace pravaha::backends {
    namespace hn = hwy::HWY_NAMESPACE;

    struct host_simd_backend {
        // Multi-input element-wise: dst[i] = expr(src0[i], src1[i], …), i in [0, n).
        template <typename T, std::size_t K, typename E>
        Outcome<void> execute(const E& expr,
                              compute::compute_view<T> dst,
                              const std::array<compute::compute_view<const T>, K>& srcs,
                              const hetero::execution_context& /*ctx*/) {
            static_assert(std::is_floating_point_v<T>, "simd v0.1: float element types only");
            static_assert(K >= 1, "simd: expression must reference at least one input slot");

            const std::size_t n = static_cast<std::size_t>(srcs[0].desc.element_count());
            for (std::size_t s = 0; s < K; ++s)
                if (srcs[s].desc.element_count() < srcs[0].desc.element_count())
                    return std::unexpected(PravahaError::make(
                        ErrorKind::InvalidArgument, "simd: input slot shorter than slot 0"));
            if (dst.desc.element_count() < n)
                return std::unexpected(PravahaError::make(
                    ErrorKind::InvalidArgument, "simd: dst smaller than src"));

            utils::nadi::BasicPulseScope<utils::nadi::SteadyClockPolicy, hetero::hetero_sink,
                                         "pravaha.hetero.simd_exec",
                                         utils::nadi::Field<"n", std::uint64_t>> _scope{
                utils::nadi::Field < "n", std::uint64_t >{static_cast<std::uint64_t>(n)}
            };

            std::array<const T*, K> in{};
            for (std::size_t s = 0; s < K; ++s) in[s] = srcs[s].base();
            T* out = dst.base();

            bool all_contig = dst.is_contiguous();
            for (std::size_t s = 0; s < K; ++s) all_contig = all_contig && srcs[s].is_contiguous();

            if (!all_contig) {
                std::array<compute::dim_t, K> istr{};
                for (std::size_t s = 0; s < K; ++s) istr[s] = srcs[s].inner_stride();
                const compute::dim_t ostr = dst.inner_stride();

                using DSigned = hn::RebindToSigned<hn::ScalableTag<T>>;
                using idx_t = hn::TFromD<DSigned>;
                const auto overflow_bound = static_cast<compute::dim_t>(
                    std::numeric_limits<idx_t>::max() / 4);

                // Vectorized path: scatter/gather available when all strides fit idx_t.
                // Read path uses GatherIndex; write path uses StoreU (contiguous dst) or
                // ScatterIndex (strided dst). Phase 2.2: scatter write now vectorized.
                const bool fits_idx = [&]() -> bool {
                    if (ostr > overflow_bound) return false;
                    for (std::size_t s = 0; s < K; ++s)
                        if (istr[s] > overflow_bound) return false;
                    return true;
                }();

                if (fits_idx) {
                    const hn::ScalableTag<T> d;
                    const DSigned di;
                    const std::size_t L = hn::Lanes(d);
                    std::size_t i = 0;

                    // Vector body
                    for (; i + L <= n; i += L) {
                        auto iota = hn::Iota(di, static_cast<idx_t>(i));
                        std::array<hn::Vec<decltype(d)>, K> xs;
                        for (std::size_t s = 0; s < K; ++s) {
                            auto vidx = hn::Mul(iota, hn::Set(di, static_cast<idx_t>(istr[s])));
                            xs[s] = hn::GatherIndex(d, in[s], vidx);
                        }
                        auto result = simd_detail::eval_vec(d, xs, expr);
                        if (ostr == 1) {
                            hn::StoreU(result, d, out + i);
                        }
                        else {
                            // Phase 2.2: vectorized scatter write
                            auto oidx = hn::Mul(iota, hn::Set(di, static_cast<idx_t>(ostr)));
                            hn::ScatterIndex(result, d, out, oidx);
                        }
                    }

                    // Phase 2.3: masked tail via LoadN/ScatterIndex — no scalar cleanup.
                    const std::size_t remaining = n - i;
                    if (remaining > 0) {
                        auto iota = hn::Iota(di, static_cast<idx_t>(i));
                        std::array<hn::Vec<decltype(d)>, K> xs;
                        for (std::size_t s = 0; s < K; ++s) {
                            auto vidx = hn::Mul(iota, hn::Set(di, static_cast<idx_t>(istr[s])));
                            // LoadN reads remaining valid elements, rest undefined (unused)
                            xs[s] = hn::GatherIndex(d, in[s], vidx); // only [0..remaining-1] used
                            // Mask to remaining elements for reads via scalar fallback is needed
                            // because GatherIndex has no N-variant; use scalar for partial gather.
                        }
                        // Scalar tail for the strided partial — only remaining elements.
                        std::array<T, K> xss{};
                        for (std::size_t j = 0; j < remaining; ++j) {
                            for (std::size_t s = 0; s < K; ++s)
                                xss[s] = in[s][(i + j) * istr[s]];
                            out[(i + j) * ostr] = simd_detail::eval_scalar(xss, expr);
                        }
                    }
                }
                else {
                    // Stride overflow fallback — pure scalar.
                    std::array<T, K> xs{};
                    for (std::size_t i = 0; i < n; ++i) {
                        for (std::size_t s = 0; s < K; ++s) xs[s] = in[s][i * istr[s]];
                        out[i * ostr] = simd_detail::eval_scalar(xs, expr);
                    }
                }
                return {};
            }

            // Contiguous path — all inputs and output are packed.
            const hn::ScalableTag<T> d;
            const std::size_t L = hn::Lanes(d);
            std::size_t i = 0;

            // Vector body
            for (; i + L <= n; i += L) {
                std::array<hn::Vec<decltype(d)>, K> xs;
                for (std::size_t s = 0; s < K; ++s) xs[s] = hn::LoadU(d, in[s] + i);
                hn::StoreU(simd_detail::eval_vec(d, xs, expr), d, out + i);
            }

            // Phase 2.3: Highway masked tail — replaces scalar cleanup loop.
            const std::size_t remaining = n - i;
            if (remaining > 0) {
                std::array<hn::Vec<decltype(d)>, K> xs;
                for (std::size_t s = 0; s < K; ++s)
                    xs[s] = hn::LoadN(d, in[s] + i, remaining);
                hn::StoreN(simd_detail::eval_vec(d, xs, expr), d, out + i, remaining);
            }
            return {};
        }

        // Single-input convenience forwarder (slot 0 only).
        template <typename T, typename E>
        Outcome<void> execute(const E& expr,
                              compute::compute_view<T> dst,
                              compute::compute_view<const T> src,
                              const hetero::execution_context& ctx) {
            std::array<compute::compute_view<const T>, 1> one{src};
            return execute<T, 1>(expr, dst, one, ctx);
        }
    };

    // Dispatch: SIMD-capable trees use host_simd_backend; others fall back to scalar.
    // SIMD backend supports floating-point only; integral T always takes scalar path.
    template <typename T, typename E>
    Outcome<void> run_simd_or_fallback(const E& expr,
                                       compute::compute_view<T> dst,
                                       compute::compute_view<const T> src,
                                       const hetero::execution_context& ctx) {
        if constexpr (std::is_floating_point_v<T>&& simd_detail::is_simd_capable<E>()) {
            host_simd_backend be;
            return be.execute(expr, dst, src, ctx);
        }
        else {
            hetero::emit_fallback_event("unsupported tag on SIMD path");
            const std::size_t n = static_cast<std::size_t>(src.desc.element_count());
            const T* in = src.base();
            std::array<T, 1> xs{};
            for (std::size_t i = 0; i < n; ++i) {
                xs[0] = in[i];
                dst.base()[i] = simd_detail::eval_scalar(xs, expr);
            }
            return {};
        }
    }

    // Multi-source dispatch.
    // SIMD backend supports floating-point only; integral T always takes scalar path.
    template <typename T, std::size_t K, typename E>
    Outcome<void> run_simd_or_fallback(const E& expr,
                                       compute::compute_view<T> dst,
                                       const std::array<compute::compute_view<const T>, K>& srcs,
                                       const hetero::execution_context& ctx) {
        if constexpr (std::is_floating_point_v<T>&& simd_detail::is_simd_capable<E>()) {
            host_simd_backend be;
            return be.template execute<T, K>(expr, dst, srcs, ctx);
        }
        else {
            hetero::emit_fallback_event("unsupported tag on SIMD path");
            const std::size_t n = static_cast<std::size_t>(srcs[0].desc.element_count());
            std::array<const T*, K> in{};
            for (std::size_t s = 0; s < K; ++s) in[s] = srcs[s].base();
            std::array<T, K> xs{};
            for (std::size_t i = 0; i < n; ++i) {
                for (std::size_t s = 0; s < K; ++s) xs[s] = in[s][i];
                dst.base()[i] = simd_detail::eval_scalar(xs, expr);
            }
            return {};
        }
    }

    // Multi-input reduce dispatcher (Part G5).
    template <pravaha::expr::reduce_op Op, typename T, std::size_t K, typename Child>
    [[nodiscard]] T run_reduce_simd_multi(const Child& child,
                                          const std::array<compute::compute_view<const T>, K>& srcs) {
        const std::size_t n = static_cast<std::size_t>(srcs[0].desc.element_count());
        std::array<const T*, K> in{};
        std::array<compute::dim_t, K> istrides{};
        for (std::size_t s = 0; s < K; ++s) {
            in[s] = srcs[s].base();
            istrides[s] = srcs[s].inner_stride();
        }
        if constexpr (simd_detail::is_simd_capable<Child>()) {
            return simd_detail::reduce_simd_multi<Op, T, K>(child, in, istrides, n);
        }
        else {
            hetero::emit_fallback_event("unsupported tag on multi-reduce SIMD path");
            using A = simd_detail::sum_accum_t<Op, T>;
            A acc = simd_detail::reduce_identity<Op, A>();
            std::array<T, K> xs{};
            for (std::size_t i = 0; i < n; ++i) {
                for (std::size_t s = 0; s < K; ++s) xs[s] = in[s][i * istrides[s]];
                acc = simd_detail::reduce_combine<Op>(acc, static_cast<A>(simd_detail::eval_scalar(xs, child)));
            }
            return static_cast<T>(acc);
        }
    }

    // Reduce dispatcher (SIMD/scalar).
    template <pravaha::expr::reduce_op Op, typename T, typename Child>
    [[nodiscard]] T run_reduce_simd(const Child& child, compute::compute_view<const T> src) {
        const std::size_t n = static_cast<std::size_t>(src.desc.element_count());
        const compute::dim_t istr = src.inner_stride();
        if constexpr (simd_detail::is_simd_capable<Child>()) {
            return simd_detail::reduce_simd<Op, T>(child, src.base(), n, istr);
        }
        else {
            hetero::emit_fallback_event("unsupported tag on reduce SIMD path");
            using A = simd_detail::sum_accum_t<Op, T>;
            A acc = simd_detail::reduce_identity<Op, A>();
            const T* p = src.base();
            std::array<T, 1> xs{};
            for (std::size_t i = 0; i < n; ++i) {
                xs[0] = p[i * istr];
                acc = simd_detail::reduce_combine<Op>(acc, static_cast<A>(simd_detail::eval_scalar(xs, child)));
            }
            return static_cast<T>(acc);
        }
    }

    // ============================================================================
    // HostSimdBackend — ComputeBackend wrapper around host_simd_backend.
    // Always available (no platform guard). Low priority — serves as the
    // guaranteed software fallback in the executor's backend cascade.
    // ============================================================================

    struct HostSimdBackend {
        [[nodiscard]] static constexpr compute::backend_metadata static_metadata() noexcept {
            return {.name = "host_simd", .hardware_priority = 10};
        }

        [[nodiscard]] bool is_available() const noexcept { return true; }

        [[nodiscard]] static constexpr bool supports_type(compute::data_element_type) noexcept {
            return true;
        }

        [[nodiscard]] bool supports_expression(std::size_t /*hash*/,
                                               compute::data_element_type /*type*/) const noexcept {
            return true;
        }

        // Cost: proportional to footprint (no JIT overhead, no driver latency).
        [[nodiscard]] std::uint64_t evaluate_cost(const compute::buffer_descriptor& desc,
                                                  std::size_t /*hash*/) const noexcept {
            return desc.footprint_bytes();
        }

        template <typename T, lithe::Expression E>
        Outcome<void> execute_elementwise(const E& expr,
                                          compute::compute_view<T> dst,
                                          compute::compute_view<const T> src,
                                          const hetero::execution_context& ctx) {
            return run_simd_or_fallback<T>(expr, dst, src, ctx);
        }

        template <typename T, std::size_t K, lithe::Expression E>
        Outcome<void> execute_elementwise_multi(const E& expr,
                                                compute::compute_view<T> dst,
                                                const std::array<compute::compute_view<const T>, K>& srcs,
                                                const hetero::execution_context& ctx) {
            return run_simd_or_fallback<T, K>(expr, dst, srcs, ctx);
        }

        template <pravaha::expr::reduce_op Op, typename T, lithe::Expression Child>
        Outcome<T> execute_reduction(const Child& child,
                                     compute::compute_view<const T> src) {
            return Outcome<T>{run_reduce_simd<Op, T>(child, src)};
        }

        template <pravaha::expr::reduce_op Op, typename T, std::size_t K, lithe::Expression Child>
        Outcome<T> execute_reduction_multi(const Child& child,
                                           const std::array<compute::compute_view<const T>, K>& srcs) {
            return Outcome<T>{run_reduce_simd_multi<Op, T, K>(child, srcs)};
        }
    };
} // namespace pravaha::backends
