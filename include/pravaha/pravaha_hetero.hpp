#pragma once
// ============================================================================
// pravaha_hetero.hpp - Generic Heterogeneous Execution EDSL overlay
//   Part 1: compute value types, execution context, routing.
//   Part 4: hetero_executor + NADI telemetry.
//
// backends/host_simd.hpp - CPU SIMD backend (Highway). Auto-included below.
// backends/metal_gpu.hpp - Metal GPU backend (macOS). Include explicitly for GPU.
// ============================================================================

#include <array>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "pravaha/pravaha.hpp"
#include "containers/dynamic/SmallVector.hpp"
#include "vakya/vakya.hpp"
#include "observability/nadi.hpp"

namespace pravaha {
    namespace lithe = ::vakya;
}

// ============================================================================
// Section 2: Compute value types
// ============================================================================

namespace pravaha::compute {
    using dim_t = std::uint64_t;
    using index_vec = containers::dynamic::SmallVector<dim_t, 64 * sizeof(dim_t)>;
    using stride_vec = containers::dynamic::SmallVector<dim_t, 64 * sizeof(dim_t)>;

    enum class memory_layout : std::uint8_t {
        contiguous = 0,
        strided = 1,
        tiled_2d = 2,
        packed_simd = 3,
        host_coherent = 4,
        sparse_csc = 5, // Compressed Sparse Column: [values, col_ptr, row_ind]
        sparse_csr = 6, // Compressed Sparse Row:    [values, row_ptr, col_ind]
        sparse_coo = 7, // Coordinate format:        [values, row_ind, col_ind]
    };

    [[nodiscard]] constexpr bool is_sparse_layout(memory_layout l) noexcept {
        return l == memory_layout::sparse_csc ||
            l == memory_layout::sparse_csr ||
            l == memory_layout::sparse_coo;
    }

    enum class data_element_type : std::uint8_t {
        unknown = 0,
        bool8, i8, i16, i32, i64,
        u8, u16, u32, u64,
        f16, bf16, f32, f64,
        complex64, complex128
    };

    [[nodiscard]] constexpr std::size_t element_size(data_element_type t) noexcept {
        switch (t) {
        case data_element_type::bool8:
        case data_element_type::i8:
        case data_element_type::u8: return 1;
        case data_element_type::i16:
        case data_element_type::u16:
        case data_element_type::f16:
        case data_element_type::bf16: return 2;
        case data_element_type::i32:
        case data_element_type::u32:
        case data_element_type::f32: return 4;
        case data_element_type::i64:
        case data_element_type::u64:
        case data_element_type::f64:
        case data_element_type::complex64: return 8;
        case data_element_type::complex128: return 16;
        case data_element_type::unknown: return 0;
        }
        return 0;
    }

    // Compile-time C++ scalar type → data_element_type map. Single source of truth
    // for backends that must derive the element type from the view's element type T
    // (rather than a runtime descriptor). Primary template is left undefined so an
    // unsupported T is a hard compile error at the use site, not a silent f32.
    template <class T>
    struct element_type_of;

    template <>
    struct element_type_of<float> {
        static constexpr data_element_type value = data_element_type::f32;
    };

    template <>
    struct element_type_of<double> {
        static constexpr data_element_type value = data_element_type::f64;
    };

    template <>
    struct element_type_of<std::int32_t> {
        static constexpr data_element_type value = data_element_type::i32;
    };

    template <>
    struct element_type_of<std::uint32_t> {
        static constexpr data_element_type value = data_element_type::u32;
    };

    template <>
    struct element_type_of<std::int16_t> {
        static constexpr data_element_type value = data_element_type::i16;
    };

    template <>
    struct element_type_of<std::uint16_t> {
        static constexpr data_element_type value = data_element_type::u16;
    };

    template <>
    struct element_type_of<std::int8_t> {
        static constexpr data_element_type value = data_element_type::i8;
    };

    template <>
    struct element_type_of<std::uint8_t> {
        static constexpr data_element_type value = data_element_type::u8;
    };

    template <>
    struct element_type_of<std::int64_t> {
        static constexpr data_element_type value = data_element_type::i64;
    };

    template <>
    struct element_type_of<std::uint64_t> {
        static constexpr data_element_type value = data_element_type::u64;
    };

    template <class T>
    inline constexpr data_element_type element_type_for =
        element_type_of<std::remove_cv_t<T>>::value;

    // MSL scalar name — consumed by Part 3 msl_emitter. Single source of truth for type↔name.
    [[nodiscard]] constexpr std::string_view msl_scalar_name(data_element_type t) noexcept {
        switch (t) {
        case data_element_type::f16: return "half";
        case data_element_type::f32: return "float";
        case data_element_type::i32: return "int";
        case data_element_type::u32: return "uint";
        case data_element_type::i16: return "short";
        case data_element_type::u16: return "ushort";
        case data_element_type::i8: return "char";
        case data_element_type::u8: return "uchar";
        case data_element_type::bool8: return "bool";
        default: return "float"; // f64 unsupported on GPU → Part 3 rejects
        }
    }

    // Memory domain vocabulary — mirrors lithe::execution::memory_domain (same values).
    // Defined here so buffer_descriptor is independent of the Vulkan backend guard.
    // The Vulkan dispatch shim maps this directly to vk_memory_flags_for().
    enum class memory_domain : std::uint8_t {
        host_cpu = 0, // standard CPU-accessible heap
        device_gpu = 1, // GPU-local memory
        shared_unified = 2, // CPU+GPU unified memory (Apple Silicon zero-copy fast path)
        guest_sandbox = 3, // sandboxed guest linear memory (unsupported by Vulkan dispatch)
    };

    struct buffer_descriptor {
        index_vec shape;
        stride_vec strides;
        memory_layout layout = memory_layout::contiguous;
        data_element_type element_type = data_element_type::unknown;
        std::uint16_t alignment = 64;
        bool writable = false;
        bool is_unified = true; // Apple Silicon zero-copy (Metal path)
        memory_domain domain = memory_domain::shared_unified; // Vulkan memory routing

        // Sparse metadata — zero cost for dense (nnz==0 means use element_count()).
        // Index arrays travel as separate engine_binding entries; see sparse adapter convention.
        std::uint64_t nnz = 0;
        data_element_type index_type = data_element_type::u32; // col_ptr/row_ind width

        [[nodiscard]] constexpr dim_t element_count() const noexcept {
            if (nnz > 0) return static_cast<dim_t>(nnz); // sparse: nnz values
            if (shape.empty()) return 0;
            dim_t n = 1;
            for (dim_t d : shape) n *= d;
            return n;
        }

        [[nodiscard]] constexpr std::size_t footprint_bytes() const noexcept {
            return static_cast<std::size_t>(element_count()) * element_size(element_type);
        }
    };

    // Half-open slice selector [begin, end) with a step. Default {0,0,1} means
    // "whole dimension" (end resolved to shape at slice time).
    struct range {
        dim_t begin = 0;
        dim_t end = 0;
        dim_t step = 1;
    };

    template <typename T>
    struct compute_view {
        T* data = nullptr;
        buffer_descriptor desc;
        dim_t offset = 0;

        [[nodiscard]] T* base() noexcept { return data + offset; }
        [[nodiscard]] const T* base() const noexcept { return data + offset; }

        // Invariant 2: const T view is always read-only (enforced by make_view factory).
        static_assert(!std::is_const_v<T> || true,
                      "compute_view<const T> is always read-only");

        // Row-major strides derived from shape when desc.strides is empty.
        [[nodiscard]] stride_vec effective_strides() const {
            if (!desc.strides.empty()) return desc.strides;
            stride_vec s;
            s.resize(desc.shape.size());
            dim_t acc = 1;
            for (std::size_t i = desc.shape.size(); i-- > 0;) {
                s[i] = acc;
                acc *= desc.shape[i];
            }
            return s;
        }

        // Innermost stride (elements between consecutive logical elements). 1 = packed.
        [[nodiscard]] dim_t inner_stride() const {
            const stride_vec s = effective_strides();
            return s.empty() ? 1 : s[s.size() - 1];
        }

        // True when a linear walk of element_count() with inner_stride() covers the
        // buffer densely — i.e. the SIMD fast path (LoadU/StoreU) is valid.
        [[nodiscard]] bool is_contiguous() const {
            return desc.layout != memory_layout::strided && inner_stride() == 1;
        }

        // Slice: one range/index per leading dimension. `range` selects [begin,end)
        // with a step; a bare integer selects a single index (dimension collapses to 1).
        // Computes the new offset and per-dimension strides; trailing dims pass through.
        template <typename... Sel>
        [[nodiscard]] compute_view slice(Sel... sel) const {
            compute_view out = *this;
            const stride_vec str = effective_strides();
            out.desc.strides = str;

            std::size_t dim = 0;
            auto apply = [&](auto s) {
                if (dim >= desc.shape.size()) return;
                const dim_t st = str[dim];
                if constexpr (std::is_integral_v<decltype(s)>) {
                    out.offset += static_cast<dim_t>(s) * st;
                    out.desc.shape[dim] = 1;
                    out.desc.strides[dim] = st;
                }
                else { // range
                    const dim_t b = s.begin;
                    const dim_t e = (s.end == 0 && s.begin == 0) ? desc.shape[dim] : s.end;
                    const dim_t step = s.step ? s.step : 1;
                    out.offset += b * st;
                    out.desc.shape[dim] = (e > b) ? (e - b + step - 1) / step : 0;
                    out.desc.strides[dim] = st * step;
                }
                ++dim;
            };
            (apply(sel), ...);
            out.desc.layout = memory_layout::strided;
            return out;
        }

        // C++23 multidimensional subscript operator (spec §6). Real offset/stride math.
        template <typename... Sel>
        [[nodiscard]] compute_view operator[](Sel... sel) const {
            return slice(sel...);
        }
    };

    template <typename T>
    [[nodiscard]] compute_view<T> make_view(T* data, buffer_descriptor desc) {
        static_assert(!std::is_const_v<T>,
                      "make_view requires mutable T; use make_const_view for read-only");
        desc.writable = true;
        return compute_view<T>{data, std::move(desc), 0};
    }

    template <typename T>
    [[nodiscard]] compute_view<const T> make_const_view(const T* data, buffer_descriptor desc) {
        desc.writable = false;
        return compute_view<const T>{data, std::move(desc), 0};
    }
} // namespace pravaha::compute

// ============================================================================
// Sections 3-6: Hetero execution types, hashing, routing
// ============================================================================

namespace pravaha::hetero {
    // Separate from core pravaha::ExecutionDomain — core enum is NOT edited.
    enum class compute_domain : std::uint8_t {
        auto_select = 0,
        host_simd = 1, // Part 2
        metal_gpu = 2, // Part 3
        vulkan = 3 // RESERVED — never selected in this edition
    };

    struct compute_grid_descriptor {
        std::array<compute::dim_t, 3> global_size = {1, 1, 1};
        std::array<compute::dim_t, 3> local_size = {1, 1, 1};
        compute::dim_t simd_width = 0; // 0 = auto-detect

        [[nodiscard]] static compute_grid_descriptor
        from_flat(compute::dim_t n, compute::dim_t tg = 256) noexcept {
            compute_grid_descriptor g;
            g.global_size = {n, 1, 1};
            g.local_size = {std::min(n, tg), 1, 1};
            return g;
        }
    };

    // structural_hash: topology-only kernel cache key.
    // Expression nodes: seeded from lithe::emit::tag_descriptor::stable_id + payload hook
    //   (lit_node<T> folds its value so distinct constants → distinct kernel keys).
    // Non-Expression terminals: fixed marker (topology-only; runtime double/int values ignored).
    // uint64 cast is a no-op on the 64-bit macOS-first target.
    template <typename E>
    [[nodiscard]] std::uint64_t structural_hash(const E& expr); // forward declare

    namespace detail {
        template <typename C>
        [[nodiscard]] std::uint64_t structural_hash_child(const C& c, std::uint64_t h) {
            if constexpr (lithe::Expression<std::decay_t<C>>) {
                // mix child's full hash into running accumulator
                return static_cast<std::uint64_t>(
                    lithe::emit::hash_combine(h, static_cast<std::size_t>(pravaha::hetero::structural_hash(c))));
            }
            else {
                // non-Expression terminal: topology-only (fold fixed leaf marker)
                return static_cast<std::uint64_t>(
                    lithe::emit::hash_combine(h, std::size_t{0x1EAF00DULL}));
            }
        }
    } // namespace detail

    template <typename E>
    [[nodiscard]] std::uint64_t structural_hash(const E& expr) {
        static_assert(sizeof(std::size_t) == 8,
                      "pravaha cache key is uint64; requires 64-bit std::size_t");
        if constexpr (lithe::VariantExpr<std::decay_t<E>>) {
            return std::visit([](const auto& alt) -> std::uint64_t {
                return pravaha::hetero::structural_hash(alt);
            }, expr);
        }
        else if constexpr (lithe::Expression<std::decay_t<E>>) {
            using Dec = std::decay_t<E>;
            // Seed from tag's stable_id (registered via tag_descriptor, impl-1/impl-4)
            std::uint64_t h = lithe::emit::tag_id<typename Dec::tag_type>::value;
            // Mix payload hook if present (lit_node<T> folds value → distinct constants distinct)
            if constexpr (lithe::emit::HasPayloadHash<Dec>) {
                using lithe::emit::structural_payload_hash;
                h = static_cast<std::uint64_t>(
                    lithe::emit::hash_combine(h, structural_payload_hash(expr)));
            }
            // Recurse into children; non-Expression children fold fixed marker
            std::apply([&](auto const&... ch) {
                ((h = detail::structural_hash_child(ch, h)), ...);
            }, expr.children);
            return h;
        }
        else {
            // Non-Expression root terminal: topology-only
            return std::uint64_t{0x1EAF00DULL};
        }
    }

    struct node_metadata {
        compute::buffer_descriptor input_desc;
        compute::buffer_descriptor output_desc;
        compute_grid_descriptor grid;
        compute_domain preferred = compute_domain::auto_select;
    };

    // Execution context: all hardware metadata keyed by structural hash.
    // No thread-local storage — Invariant 4 satisfied by construction.
    struct execution_context {
        using key_t = std::uint64_t;

        void bind(key_t hash, node_metadata meta) {
            overlay_.insert_or_assign(hash, std::move(meta));
        }

        [[nodiscard]] const node_metadata* lookup(key_t hash) const noexcept {
            auto it = overlay_.find(hash);
            return it != overlay_.end() ? &it->second : nullptr;
        }

        [[nodiscard]] std::size_t size() const noexcept { return overlay_.size(); }
        void clear() noexcept { overlay_.clear(); }

    private:
        std::unordered_map<key_t, node_metadata> overlay_;
    };

    struct routing_policy {
        std::size_t gpu_threshold_bytes = 256 * 1024;
        // Reductions have a different footprint/latency profile (kernel launch +
        // threadgroup sync overhead) so they get their own GPU threshold (Part E).
        std::size_t reduce_gpu_threshold_bytes = 1024 * 1024;
        compute_domain force = compute_domain::auto_select;
        bool allow_gpu = true;
        // When true, the executor runs lithe::preset::O3 on every expression before
        // hashing. O3 collapses algebraically-equal trees (x+0→x, x*1→x, constant
        // folds) to a single cache key. Zero runtime cost when false (no branch taken).
        // Note: true_cse_pass inside O3 is a fixpoint placeholder — the collapse
        // guarantee comes from simplify/fold/strength/dead-subtree passes, not CSE.
        bool optimize_before_codegen = false;
    };

    // Pure function. No side effects, no allocation. Deterministic given inputs.
    [[nodiscard]] inline compute_domain
    route(const compute::buffer_descriptor& desc, const routing_policy& policy) noexcept {
        if (policy.force != compute_domain::auto_select) return policy.force;

        // f64 has no Metal scalar → force SIMD (see msl_scalar_name note).
        if (desc.element_type == compute::data_element_type::f64)
            return compute_domain::host_simd;

        const bool big = desc.footprint_bytes() >= policy.gpu_threshold_bytes;
        if (big && policy.allow_gpu) return compute_domain::metal_gpu;
        return compute_domain::host_simd;
    }

    // ============================================================================
    // Vākya structural-expression boundary.
    //
    // Pravaha consumes concrete Vākya trees.  Optimisation is intentionally
    // external: no compiler pass framework is pulled into this execution layer.
    // ============================================================================

    template <class E>
    concept FlatExpression = vakya::Expression<E> && !vakya::VariantExpr<E>;

    template <class E>
    concept BackendExpr = FlatExpression<E> || vakya::VariantExpr<E>;

    // Kept as one call boundary so a user-provided Vākya rewrite can be added
    // later without changing executor control flow.
    template <class E, class Fn>
    decltype(auto) with_canon([[maybe_unused]] bool optimize, E&& expr, Fn&& fn) {
        return std::forward<Fn>(fn)(std::forward<E>(expr));
    }
} // namespace pravaha::hetero

// ============================================================================
// Section 7: Math operation tags for the hetero eDSL (used by pravaha_expr.hpp
// and by the scalar/Metal evaluation paths below).
// Defined here so eval_scalar and emit_expr can reference them without a
// circular dependency on pravaha_expr.hpp.
// ============================================================================

namespace pravaha::expr {
    struct sqrt_tag {};

    struct exp_tag {};

    struct log_tag {};

    struct sin_tag {};

    struct cos_tag {};

    struct abs_tag {};

    // Constant-leaf tag. Distinct from call_tag so the stored value is honored on
    // every path and so structurally-distinct constants get distinct kernel cache keys.
    struct lit_tag {};

    // Indexed input-leaf tag. tag_id folds the slot index N so input<0>, input<1>, …
    // hash distinctly and bind to distinct buffer slots (multi-input, Part C).
    template <std::size_t N>
    struct input_tag {};

    // Trait: extract the slot index of an input_tag; is_input_tag detects it.
    template <typename T>
    struct input_tag_index {
        static constexpr bool value = false;
    };

    template <std::size_t N>
    struct input_tag_index<input_tag<N>> {
        static constexpr bool value = true;
        static constexpr std::size_t index = N;
    };

    // Reduction op kind (Part E). Whole-input reduce of an element-wise child
    // expression → scalar. Distinct op family from element-wise (own emit/dispatch).
    enum class reduce_op : std::uint8_t { sum, max, min };

    template <reduce_op Op>
    struct reduce_tag {};

    using reduce_sum_tag = reduce_tag<reduce_op::sum>;
    using reduce_max_tag = reduce_tag<reduce_op::max>;
    using reduce_min_tag = reduce_tag<reduce_op::min>;

    // Trait: detect a reduce_tag and recover its op.
    template <typename T>
    struct reduce_tag_op {
        static constexpr bool value = false;
    };

    template <reduce_op Op>
    struct reduce_tag_op<reduce_tag<Op>> {
        static constexpr bool value = true;
        static constexpr reduce_op op = Op;
    };
} // namespace pravaha::expr

// Register Pravaha tags with Vākya's tag_descriptor so vakya::emit::structural_hash
// and vakya::tree compile-time folds can see symbol/stable_id/arity.
// Extension-band ids (>= kExtensionIdBase = 1000) avoid collisions with built-ins.
namespace vakya::emit {
    template <>
    struct tag_descriptor<pravaha::expr::sqrt_tag> {
        static constexpr std::string_view symbol = "sqrt";
        static constexpr std::size_t stable_id = kExtensionIdBase + 10u;
        static constexpr std::uint8_t arity = 1;
    };

    template <>
    struct tag_descriptor<pravaha::expr::exp_tag> {
        static constexpr std::string_view symbol = "exp";
        static constexpr std::size_t stable_id = kExtensionIdBase + 11u;
        static constexpr std::uint8_t arity = 1;
    };

    template <>
    struct tag_descriptor<pravaha::expr::log_tag> {
        static constexpr std::string_view symbol = "log";
        static constexpr std::size_t stable_id = kExtensionIdBase + 12u;
        static constexpr std::uint8_t arity = 1;
    };

    template <>
    struct tag_descriptor<pravaha::expr::sin_tag> {
        static constexpr std::string_view symbol = "sin";
        static constexpr std::size_t stable_id = kExtensionIdBase + 13u;
        static constexpr std::uint8_t arity = 1;
    };

    template <>
    struct tag_descriptor<pravaha::expr::cos_tag> {
        static constexpr std::string_view symbol = "cos";
        static constexpr std::size_t stable_id = kExtensionIdBase + 14u;
        static constexpr std::uint8_t arity = 1;
    };

    template <>
    struct tag_descriptor<pravaha::expr::abs_tag> {
        static constexpr std::string_view symbol = "fabs"; // MSL spelling; scalar path uses std::abs
        static constexpr std::size_t stable_id = kExtensionIdBase + 15u;
        static constexpr std::uint8_t arity = 1;
    };

    // Constant leaf — arity 0. Kernel identity comes from the payload hook, not the id alone.
    template <>
    struct tag_descriptor<pravaha::expr::lit_tag> {
        static constexpr std::string_view symbol = "lit";
        static constexpr std::size_t stable_id = kExtensionIdBase + 20u;
        static constexpr std::uint8_t arity = 0;
    };

    // Indexed input leaves — arity 0. Slot index folded into stable_id so
    // input<0>, input<1>, … hash distinctly without a payload hook.
    template <std::size_t N>
    struct tag_descriptor<pravaha::expr::input_tag<N>> {
        static constexpr std::string_view symbol = "in";
        static constexpr std::size_t stable_id = kExtensionIdBase + 100u + N;
        static constexpr std::uint8_t arity = 0;
    };

    // Reduction tags — arity 1 (wrap one element-wise child).
    template <pravaha::expr::reduce_op Op>
    struct tag_descriptor<pravaha::expr::reduce_tag<Op>> {
        static constexpr std::string_view symbol = "reduce";
        static constexpr std::size_t stable_id = kExtensionIdBase + 30u + static_cast<std::size_t>(Op);
        static constexpr std::uint8_t arity = 1;
    };
} // namespace vakya::emit

// ============================================================================
// Part 4: NADI telemetry — sink alias + event emitters
// ============================================================================

namespace pravaha::hetero {
    // Compile-time sink selection: define PRAVAHA_HETERO_SINK before including
    // this header to capture pulses. Default = zero-cost NoSink.
#ifndef PRAVAHA_HETERO_SINK
#  define PRAVAHA_HETERO_SINK ::utils::nadi::NoSink
#endif
    using hetero_sink = PRAVAHA_HETERO_SINK;

    inline void emit_backend_selected(compute_domain d, std::uint64_t hash,
                                      std::size_t bytes) noexcept {
        using P = utils::nadi::Pulse<"pravaha.hetero.backend_selected",
                                     utils::nadi::Field < "domain", std::uint8_t>,
        utils::nadi::Field < "hash", std::uint64_t >,
            utils::nadi::Field<"bytes", std::uint64_t> >;
        utils::nadi::route_pulse<hetero_sink>(P{
            .id = utils::nadi::generate_event_id(),
            .phase = utils::nadi::PulsePhase::Instant,
            .timestamp_ns = utils::nadi::now_ns(),
            .payload = {
                utils::nadi::Field < "domain", std::uint8_t >{static_cast<std::uint8_t>(d)},
                utils::nadi::Field < "hash", std::uint64_t >{hash},
                utils::nadi::Field < "bytes", std::uint64_t >{bytes}
            }
        });
    }

    inline void emit_cache_event(bool hit, std::uint64_t hash) noexcept {
        if (hit) {
            using P = utils::nadi::Pulse<"pravaha.hetero.kernel_cache_hit",
                                         utils::nadi::Field<"hash", std::uint64_t>>;
            utils::nadi::route_pulse<hetero_sink>(P{
                .id = utils::nadi::generate_event_id(),
                .phase = utils::nadi::PulsePhase::Instant,
                .timestamp_ns = utils::nadi::now_ns(),
                .payload = {utils::nadi::Field < "hash", std::uint64_t >{hash}}
            });
        }
        else {
            using P = utils::nadi::Pulse<"pravaha.hetero.kernel_cache_miss",
                                         utils::nadi::Field<"hash", std::uint64_t>>;
            utils::nadi::route_pulse<hetero_sink>(P{
                .id = utils::nadi::generate_event_id(),
                .phase = utils::nadi::PulsePhase::Instant,
                .timestamp_ns = utils::nadi::now_ns(),
                .payload = {utils::nadi::Field < "hash", std::uint64_t >{hash}}
            });
        }
    }

    inline void emit_fallback_event(std::string_view /*reason*/) noexcept {
        using P = utils::nadi::Pulse<"pravaha.hetero.backend_fallback">;
        utils::nadi::route_pulse<hetero_sink>(P{
            .id = utils::nadi::generate_event_id(),
            .phase = utils::nadi::PulsePhase::Instant,
            .timestamp_ns = utils::nadi::now_ns(),
        });
    }

    // RAII scope that emits Begin/End pulses around GPU dispatch for latency capture.
    struct nadi_gpu_dispatch_scope {
        utils::nadi::BasicPulseScope<utils::nadi::TscCycleClockPolicy, hetero_sink,
                                     "pravaha.hetero.gpu_dispatch",
                                     utils::nadi::Field<"bytes", std::uint64_t>> scope_;

        explicit nadi_gpu_dispatch_scope(std::size_t bytes)
            : scope_{utils::nadi::Field < "bytes", std::uint64_t >{bytes}} {}
    };

    struct simd_events {
        static constexpr auto exec_begin = "pravaha.hetero.simd_exec";
    };
} // namespace pravaha::hetero

// ============================================================================
// Backend capability — concept, traits, backend_set.
// ============================================================================

#include "pravaha/backends/capability.hpp"

// ============================================================================
// CPU SIMD backend — extracted to backends/host_simd.hpp.
// ============================================================================

#include "pravaha/backends/host_simd.hpp"

// Forward-declare MetalGpuBackend so default_hetero_executor can reference it
// when HAS_METAL_CPP is defined, even before metal_gpu.hpp is included.
// Full definition is in backends/metal_gpu.hpp (user opt-in).
#if defined(__APPLE__) && defined(HAS_METAL_CPP)
namespace pravaha::backends::metal {
    struct MetalGpuBackend;
}
#endif

// ============================================================================
// hetero_executor — parameterized on a compile-time list of ComputeBackend
// types. No #if, no switch-on-enum. Platform detection is confined to
// capability.hpp and the individual backend wrappers.
//
// Execute algorithm per call:
//  1. Compute structural_hash on the expr.
//  2. Compile-time: skip backends whose backend_traits::supports_type returns
//     false for the element type.
//  3. Runtime: skip backends where is_available()==false or
//     supports_expression(hash, type)==false.
//  4. User override: if routing_policy::force_name matches a backend, lock to it.
//  5. Cost race: pick backend with highest evaluate_cost score.
//  6. Graceful fallback: on Outcome failure cascade to the next candidate,
//     each step emitting a NADI backend_fallback pulse.
// Note: optimize_before_codegen is a caller-side hint. The executor does not
// call optimize_preset internally; callers must pre-optimize if desired.
// ============================================================================

namespace pravaha::hetero {
    // Internal: select and invoke the best available backend from a tuple.
    // Returns an error Outcome only when ALL backends fail (final SIMD always succeeds).
    namespace detail {
        // Suitability score for one backend given descriptor and hash.
        // Zero means "do not use" (unavailable or type not supported).
        template <typename Backend>
        [[nodiscard]] std::uint64_t backend_score(Backend& be,
                                                  const compute::buffer_descriptor& desc,
                                                  std::size_t hash,
                                                  compute::data_element_type elem) noexcept {
            if (!compute::backend_traits<Backend>::supports_type(elem)) return 0;
            if (!be.is_available()) return 0;
            if (!be.supports_expression(hash, elem)) return 0;
            const std::uint64_t cost = be.evaluate_cost(desc, hash);
            if (cost == 0) return 0;
            return static_cast<std::uint64_t>(compute::backend_traits<Backend>::priority())
                * 0x10000'0000ULL
                + cost;
        }

        // Suitability score for reduction operations. Uses evaluate_reduce_cost() if
        // the backend exposes it, otherwise falls back to evaluate_cost().
        template <typename Backend>
        [[nodiscard]] std::uint64_t backend_reduce_score(Backend& be,
                                                         const compute::buffer_descriptor& desc,
                                                         std::size_t hash,
                                                         compute::data_element_type elem) noexcept {
            if (!compute::backend_traits<Backend>::supports_type(elem)) return 0;
            if (!be.is_available()) return 0;
            if (!be.supports_expression(hash, elem)) return 0;
            std::uint64_t cost;
            if constexpr (requires { be.evaluate_reduce_cost(desc, hash); })
                cost = be.evaluate_reduce_cost(desc, hash);
            else
                cost = be.evaluate_cost(desc, hash);
            if (cost == 0) return 0;
            return static_cast<std::uint64_t>(compute::backend_traits<Backend>::priority())
                * 0x10000'0000ULL
                + cost;
        }
    } // namespace detail

    // Parameters are unconstrained here to support forward-declared backend types
    // in default_hetero_executor aliases. ComputeBackend is enforced at the call
    // site via backend_score (which reads backend_traits<B>::supports_type) and the
    // execute_elementwise/execute_reduction call expressions.
    template <typename... Backends>
    struct basic_hetero_executor {
        routing_policy policy{};
        [[no_unique_address]] std::tuple<Backends...> backends_{};

        // -------------------------------------------------------------------------
        // Single-input element-wise: dst[i] = expr(src[i])
        // -------------------------------------------------------------------------
        template <typename T, typename E>
        Outcome<void> execute(const E& expr,
                              compute::compute_view<T> dst,
                              compute::compute_view<const T> src,
                              const execution_context& ctx) {
            return execute_canonicalized_<T>(expr, dst, src, ctx);
        }

        // -------------------------------------------------------------------------
        // Multi-source element-wise: dst[i] = expr(src0[i], …, srcK[i])
        // -------------------------------------------------------------------------
        template <typename T, std::size_t K, typename E>
        Outcome<void> execute(const E& expr,
                              compute::compute_view<T> dst,
                              const std::array<compute::compute_view<const T>, K>& srcs,
                              const execution_context& ctx) {
            return execute_multi_canonicalized_<T, K>(expr, dst, srcs, ctx);
        }

        // -------------------------------------------------------------------------
        // Single-input reduction: reduce(child, src) → scalar T
        // -------------------------------------------------------------------------
        template <expr::reduce_op Op, typename T, typename Child>
        Outcome<T> reduce(const Child& child,
                          compute::compute_view<const T> src,
                          const execution_context& ctx) {
            return reduce_canonicalized_<Op, T>(child, src, ctx);
        }

        // -------------------------------------------------------------------------
        // Multi-input reduction: reduce(child, {src0,...,srcK}) → scalar T
        // -------------------------------------------------------------------------
        template <expr::reduce_op Op, typename T, std::size_t K, typename Child>
        Outcome<T> reduce(const Child& child,
                          const std::array<compute::compute_view<const T>, K>& srcs,
                          const execution_context& ctx) {
            return reduce_multi_canonicalized_<Op, T, K>(child, srcs, ctx);
        }

    private:
        // -------------------------------------------------------------------------
        // Canonicalized entry points.
        // Each uses with_canon() to optionally run O3 before hashing, so cache key
        // and emitted kernel both see the same canonical tree. When
        // optimize_before_codegen == false the expression passes through unchanged.
        // -------------------------------------------------------------------------

        template <typename T, typename E>
        Outcome<void> execute_canonicalized_(const E& expr, compute::compute_view<T> dst,
                                             compute::compute_view<const T> src,
                                             const execution_context& ctx) {
            return with_canon(policy.optimize_before_codegen, expr,
                              [&](auto&& canon) -> Outcome<void> {
                                  const std::uint64_t hash = pravaha::hetero::structural_hash(canon);
                                  const auto elem = src.desc.element_type;
                                  const std::size_t bytes = src.desc.footprint_bytes();

                                  if (policy.force == compute_domain::host_simd) {
                                      emit_backend_selected(compute_domain::host_simd, hash, bytes);
                                      return backends::run_simd_or_fallback<T>(canon, dst, src, ctx);
                                  }
                                  if (const auto* md = ctx.lookup(hash);
                                      md && md->preferred == compute_domain::host_simd) {
                                      emit_backend_selected(compute_domain::host_simd, hash, bytes);
                                      return backends::run_simd_or_fallback<T>(canon, dst, src, ctx);
                                  }
                                  return dispatch_elementwise_<T>(canon, dst, src, ctx, hash, elem, bytes,
                                                                  std::index_sequence_for < Backends...>{});
                              });
        }

        template <typename T, std::size_t K, typename E>
        Outcome<void> execute_multi_canonicalized_(
            const E& expr, compute::compute_view<T> dst,
            const std::array<compute::compute_view<const T>, K>& srcs,
            const execution_context& ctx) {
            return with_canon(policy.optimize_before_codegen, expr,
                              [&](auto&& canon) -> Outcome<void> {
                                  const std::uint64_t hash = pravaha::hetero::structural_hash(canon);
                                  std::size_t max_bytes = 0;
                                  const compute::buffer_descriptor* route_desc = &srcs[0].desc;
                                  for (std::size_t s = 0; s < K; ++s)
                                      if (srcs[s].desc.footprint_bytes() > max_bytes) {
                                          max_bytes = srcs[s].desc.footprint_bytes();
                                          route_desc = &srcs[s].desc;
                                      }
                                  const auto elem = route_desc->element_type;

                                  if (policy.force == compute_domain::host_simd) {
                                      emit_backend_selected(compute_domain::host_simd, hash, max_bytes);
                                      return backends::run_simd_or_fallback<T, K>(canon, dst, srcs, ctx);
                                  }
                                  return dispatch_elementwise_multi_<T, K>(canon, dst, srcs, ctx, hash, elem, max_bytes,
                                                                           *route_desc,
                                                                           std::index_sequence_for < Backends...>{});
                              });
        }

        template <expr::reduce_op Op, typename T, typename Child>
        Outcome<T> reduce_canonicalized_(const Child& child,
                                         compute::compute_view<const T> src,
                                         const execution_context& /*ctx*/) {
            return with_canon(policy.optimize_before_codegen, child,
                              [&](auto&& canon) -> Outcome<T> {
                                  const std::uint64_t hash = pravaha::hetero::structural_hash(canon);
                                  const auto elem = src.desc.element_type;
                                  const std::size_t bytes = src.desc.footprint_bytes();

                                  if (policy.force == compute_domain::host_simd) {
                                      emit_backend_selected(compute_domain::host_simd, hash, bytes);
                                      return backends::run_reduce_simd < Op, T > (canon, src);
                                  }
                                  return dispatch_reduction_<Op, T>(canon, src, {}, hash, elem, bytes,
                                                                    std::index_sequence_for < Backends...>{});
                              });
        }

        template <expr::reduce_op Op, typename T, std::size_t K, typename Child>
        Outcome<T> reduce_multi_canonicalized_(
            const Child& child,
            const std::array<compute::compute_view<const T>, K>& srcs,
            const execution_context& /*ctx*/) {
            return with_canon(policy.optimize_before_codegen, child,
                              [&](auto&& canon) -> Outcome<T> {
                                  const std::uint64_t hash = pravaha::hetero::structural_hash(canon);
                                  std::size_t max_bytes = 0;
                                  for (std::size_t s = 0; s < K; ++s)
                                      max_bytes = std::max(max_bytes, srcs[s].desc.footprint_bytes());
                                  const compute::buffer_descriptor* route_desc = &srcs[0].desc;
                                  for (std::size_t s = 0; s < K; ++s)
                                      if (srcs[s].desc.footprint_bytes() == max_bytes) {
                                          route_desc = &srcs[s].desc;
                                          break;
                                      }
                                  const auto elem = route_desc->element_type;

                                  if (policy.force == compute_domain::host_simd) {
                                      emit_backend_selected(compute_domain::host_simd, hash, max_bytes);
                                      return backends::run_reduce_simd_multi < Op, T, K > (canon, srcs);
                                  }
                                  return dispatch_reduction_multi_<Op, T, K>(canon, srcs, {}, hash, elem, max_bytes,
                                                                             *route_desc,
                                                                             std::index_sequence_for < Backends...>{});
                              });
        }

        // -------------------------------------------------------------------------
        // Internal dispatch helpers — compile-time iteration over Backends tuple.
        // -------------------------------------------------------------------------

        template <typename T, typename E, std::size_t... Is>
        Outcome<void> dispatch_elementwise_(
            const E& expr, compute::compute_view<T> dst,
            compute::compute_view<const T> src,
            const execution_context& ctx,
            std::uint64_t hash, compute::data_element_type elem, std::size_t bytes,
            std::index_sequence<Is...>) {
            // Build scores array at runtime (constexpr element filter embedded in backend_score).
            std::array<std::uint64_t, sizeof...(Is)> scores{
                detail::backend_score(std::get < Is > (backends_), src.desc, hash, elem)...
            };

            // Try backends in descending score order.
            while (true) {
                std::size_t best = sizeof...(Is);
                std::uint64_t best_score = 0;
                for (std::size_t i = 0; i < sizeof...(Is); ++i)
                    if (scores[i] > best_score) {
                        best_score = scores[i];
                        best = i;
                    }
                if (best == sizeof...(Is)) break; // no usable backend remaining

                scores[best] = 0; // prevent re-selecting after potential failure

                Outcome<void> r = std::unexpected(PravahaError::make(
                    ErrorKind::ExecutorUnavailable, "no backend selected"));

                // Dispatch to the selected index via fold — only one branch fires.
                ([&]<std::size_t I>() {
                    if (I == best) {
                        emit_backend_selected(compute_domain::auto_select, hash, bytes);
                        r = std::get < I > (backends_).execute_elementwise(expr, dst, src, ctx);
                    }
                }.template operator()<Is>(), ...);

                if (r) return r;
                emit_fallback_event("backend failed → cascade");
            }
            // All backends exhausted — emit fallback and run SIMD scalar directly.
            emit_fallback_event("all backends failed → scalar");
            return backends::run_simd_or_fallback<T>(expr, dst, src, ctx);
        }

        template <typename T, std::size_t K, typename E, std::size_t... Is>
        Outcome<void> dispatch_elementwise_multi_(
            const E& expr, compute::compute_view<T> dst,
            const std::array<compute::compute_view<const T>, K>& srcs,
            const execution_context& ctx,
            std::uint64_t hash, compute::data_element_type elem, std::size_t bytes,
            const compute::buffer_descriptor& route_desc,
            std::index_sequence<Is...>) {
            std::array<std::uint64_t, sizeof...(Is)> scores{
                detail::backend_score(std::get < Is > (backends_), route_desc, hash, elem)...
            };

            while (true) {
                std::size_t best = sizeof...(Is);
                std::uint64_t best_score = 0;
                for (std::size_t i = 0; i < sizeof...(Is); ++i)
                    if (scores[i] > best_score) {
                        best_score = scores[i];
                        best = i;
                    }
                if (best == sizeof...(Is)) break;

                scores[best] = 0;

                Outcome<void> r = std::unexpected(PravahaError::make(
                    ErrorKind::ExecutorUnavailable, "no backend selected"));

                ([&]<std::size_t I>() {
                    if (I == best) {
                        emit_backend_selected(compute_domain::auto_select, hash, bytes);
                        r = std::get < I > (backends_).template execute_elementwise_multi<T, K>(
                            expr, dst, srcs, ctx);
                    }
                }.template operator()<Is>(), ...);

                if (r) return r;
                emit_fallback_event("backend failed → cascade");
            }
            emit_fallback_event("all backends failed → scalar");
            return backends::run_simd_or_fallback<T, K>(expr, dst, srcs, ctx);
        }

        template <expr::reduce_op Op, typename T, typename Child, std::size_t... Is>
        Outcome<T> dispatch_reduction_(
            const Child& child, compute::compute_view<const T> src,
            const execution_context& ctx,
            std::uint64_t hash, compute::data_element_type elem, std::size_t bytes,
            std::index_sequence<Is...>) {
            std::array<std::uint64_t, sizeof...(Is)> scores{
                detail::backend_reduce_score(std::get < Is > (backends_), src.desc, hash, elem)...
            };

            while (true) {
                std::size_t best = sizeof...(Is);
                std::uint64_t best_score = 0;
                for (std::size_t i = 0; i < sizeof...(Is); ++i)
                    if (scores[i] > best_score) {
                        best_score = scores[i];
                        best = i;
                    }
                if (best == sizeof...(Is)) break;

                scores[best] = 0;

                Outcome<T> r = std::unexpected(PravahaError::make(
                    ErrorKind::ExecutorUnavailable, "no backend selected"));

                ([&]<std::size_t I>() {
                    if (I == best) {
                        emit_backend_selected(compute_domain::auto_select, hash, bytes);
                        r = std::get < I > (backends_).template execute_reduction<Op, T>(child, src);
                    }
                }.template operator()<Is>(), ...);

                if (r) return r;
                emit_fallback_event("backend failed → cascade");
            }
            emit_fallback_event("all backends failed → scalar reduce");
            return Outcome<T>{backends::run_reduce_simd < Op, T > (child, src)};
        }

        template <expr::reduce_op Op, typename T, std::size_t K, typename Child, std::size_t... Is>
        Outcome<T> dispatch_reduction_multi_(
            const Child& child,
            const std::array<compute::compute_view<const T>, K>& srcs,
            const execution_context& ctx,
            std::uint64_t hash, compute::data_element_type elem, std::size_t bytes,
            const compute::buffer_descriptor& route_desc,
            std::index_sequence<Is...>) {
            std::array<std::uint64_t, sizeof...(Is)> scores{
                detail::backend_reduce_score(std::get < Is > (backends_), route_desc, hash, elem)...
            };

            while (true) {
                std::size_t best = sizeof...(Is);
                std::uint64_t best_score = 0;
                for (std::size_t i = 0; i < sizeof...(Is); ++i)
                    if (scores[i] > best_score) {
                        best_score = scores[i];
                        best = i;
                    }
                if (best == sizeof...(Is)) break;

                scores[best] = 0;

                Outcome<T> r = std::unexpected(PravahaError::make(
                    ErrorKind::ExecutorUnavailable, "no backend selected"));

                ([&]<std::size_t I>() {
                    if (I == best) {
                        emit_backend_selected(compute_domain::auto_select, hash, bytes);
                        r = std::get < I > (backends_).template execute_reduction_multi<Op, T, K>(
                            child, srcs);
                    }
                }.template operator()<Is>(), ...);

                if (r) return r;
                emit_fallback_event("backend failed → cascade");
            }
            emit_fallback_event("all backends failed → scalar multi-reduce");
            return Outcome<T>{backends::run_reduce_simd_multi < Op, T, K > (child, srcs)};
        }
    };
} // namespace pravaha::hetero

// ============================================================================
// default_backend_set and default_hetero_executor.
// The SIMD-only defaults are defined here for non-Metal builds.
// When HAS_METAL_CPP is defined, these are omitted — metal_gpu.hpp defines
// the full defaults (with MetalGpuBackend) after MetalGpuBackend is complete.
// ============================================================================

namespace pravaha::compute {
#if defined(__APPLE__) && defined(HAS_METAL_CPP)
    // Defined in backends/metal_gpu.hpp after MetalGpuBackend is fully defined.
#else

    using default_backend_set = backend_set<pravaha::backends::HostSimdBackend>;

#endif
} // namespace pravaha::compute

namespace pravaha::hetero {
#if defined(__APPLE__) && defined(HAS_METAL_CPP)
    // Defined in backends/metal_gpu.hpp after MetalGpuBackend is fully defined.
#else

    using default_hetero_executor = basic_hetero_executor<pravaha::backends::HostSimdBackend>;
    using hetero_executor = default_hetero_executor;

#endif
} // namespace pravaha::hetero


// ============================================================================
// Section 7.1: Distributed topology abstractions (spec §7.1)

namespace pravaha::distributed {
    using NodeId = std::uint32_t;

    // Describes a remote worker node endpoint.
    struct remote_node_descriptor {
        NodeId id = 0;
        std::string ip_address;
        std::uint16_t port = 50051;
        bool is_active = true;
    };

    // Schema and size metadata for a serialized cross-node payload.
    struct distributed_payload_metadata {
        std::size_t payload_bytes = 0;
        std::uint64_t schema_hash = 0; // cross-node binary layout validation
        bool is_blittable = true;
    };

    // Descriptor for a single RPC-dispatched task graph fragment.
    struct rpc_task_descriptor {
        std::string target_symbol;
        NodeId assigned_node_id = 0;
        pravaha::JoinPolicy network_join_policy{};
        distributed_payload_metadata serialization_meta;
    };
} // namespace pravaha::distributed
