#pragma once
// ============================================================================
// backends/metal_gpu.hpp - Metal GPU backend for the Pravaha hetero overlay.
//   MSL emitter is platform-independent (no HAS_METAL_CPP guard needed).
//   Metal device + dispatch guarded by: #if defined(__APPLE__) && defined(HAS_METAL_CPP)
// ============================================================================

#include "pravaha/pravaha_hetero.hpp"  // compute types, expr tags, NADI, SIMD dispatchers
#include "containers/cache/kosha.hpp"

#include <sstream>
#include <array>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <iomanip>
#include <cassert>

namespace pravaha::backends {
    namespace lithe = ::vakya;
}

// ============================================================================
// MSL emitter — platform-independent (pure string generation, no Metal calls).
// ============================================================================

namespace pravaha::backends::metal::msl {
    // Emit a scalar MSL expression fragment for one output element.
    template <typename E>
    void emit_expr(std::ostream& os, const E& expr, std::string_view var,
                   bool indexed = false) {
        using node_t = std::decay_t<E>;
        using tag = typename node_t::tag_type;
        using namespace lithe;

        if constexpr (std::is_same_v<tag, call_tag>) {
            os << var;
            if (indexed) os << "0";
        }
        else if constexpr (pravaha::expr::input_tag_index<tag>::value) {
            os << var << pravaha::expr::input_tag_index<tag>::index;
        }
        else if constexpr (std::is_same_v<tag, pravaha::expr::lit_tag>) {
            if constexpr (requires { expr.value; }) {
                std::ostringstream lit;
                lit << std::showpoint << std::setprecision(9) << static_cast<double>(expr.value);
                os << lit.str();
            }
            else {
                os << "0.0";
            }
        }
        else if constexpr (std::is_same_v<tag, neg_tag>) {
            os << "(-(";
            emit_expr(os, std::get < 0 > (expr.children), var, indexed);
            os << "))";
        }
        else if constexpr (std::is_same_v<tag, pravaha::expr::sqrt_tag>) {
            os << "sqrt(";
            emit_expr(os, std::get < 0 > (expr.children), var, indexed);
            os << ")";
        }
        else if constexpr (std::is_same_v<tag, pravaha::expr::exp_tag>) {
            os << "exp(";
            emit_expr(os, std::get < 0 > (expr.children), var, indexed);
            os << ")";
        }
        else if constexpr (std::is_same_v<tag, pravaha::expr::log_tag>) {
            os << "log(";
            emit_expr(os, std::get < 0 > (expr.children), var, indexed);
            os << ")";
        }
        else if constexpr (std::is_same_v<tag, pravaha::expr::sin_tag>) {
            os << "sin(";
            emit_expr(os, std::get < 0 > (expr.children), var, indexed);
            os << ")";
        }
        else if constexpr (std::is_same_v<tag, pravaha::expr::cos_tag>) {
            os << "cos(";
            emit_expr(os, std::get < 0 > (expr.children), var, indexed);
            os << ")";
        }
        else if constexpr (std::is_same_v<tag, pravaha::expr::abs_tag>) {
            os << "fabs(";
            emit_expr(os, std::get < 0 > (expr.children), var, indexed);
            os << ")";
        }
        else {
            static_assert(lithe::emit::tag_descriptor<tag>::arity == 2,
                          "emit_expr: binary branch reached with non-binary tag");
            os << "(";
            emit_expr(os, std::get < 0 > (expr.children), var, indexed);
            os << ' ' << lithe::emit::tag_descriptor<tag>::symbol << ' ';
            emit_expr(os, std::get < 1 > (expr.children), var, indexed);
            os << ")";
        }
    }

    template <typename N>
    struct is_input_leaf {
        static constexpr bool value =
            pravaha::expr::input_tag_index < typename N::tag_type > ::value;
    };

    template <typename E>
    consteval bool uses_input_leaves() {
        return lithe::tree::any_tag_satisfies<E, is_input_leaf>();
    }

    // Produce a complete MSL compute kernel for element-wise: dst[i] = expr(src_N[i]).
    template <typename E>
    [[nodiscard]] std::string emit_kernel(const E& expr,
                                          compute::data_element_type elem,
                                          std::string_view kernel_name = "pravaha_kernel") {
        constexpr std::size_t K =
            pravaha::backends::simd_detail::input_slot_count<E>() == 0
                ? 1
                : pravaha::backends::simd_detail::input_slot_count<E>();
        const std::string_view t = compute::msl_scalar_name(elem);
        std::ostringstream os;
        os << "#include <metal_stdlib>\n"
            << "using namespace metal;\n"
            << "kernel void " << kernel_name << "(\n";
        for (std::size_t s = 0; s < K; ++s)
            os << "    device const " << t << "* src" << s
                << " [[buffer(" << s << ")]],\n";
        os << "    device " << t << "* dst [[buffer(" << K << ")]],\n"
            << "    constant uint& n [[buffer(" << (K + 1) << ")]],\n"
            << "    uint gid [[thread_position_in_grid]]) {\n"
            << "    if (gid >= n) return;\n";
        if constexpr (uses_input_leaves<E>()) {
            for (std::size_t s = 0; s < K; ++s)
                os << "    " << t << " x" << s << " = src" << s << "[gid];\n";
            os << "    dst[gid] = ";
            emit_expr(os, expr, "x", /*indexed=*/true);
        }
        else {
            os << "    " << t << " x = src0[gid];\n"
                << "    dst[gid] = ";
            emit_expr(os, expr, "x", /*indexed=*/false);
        }
        os << ";\n}\n";
        return os.str();
    }

    // Strided element-wise kernel (Part G4).
    template <typename E>
    [[nodiscard]] std::string emit_kernel_strided(const E& expr,
                                                  compute::data_element_type elem,
                                                  std::string_view kernel_name = "pravaha_kernel_strided") {
        constexpr std::size_t K =
            pravaha::backends::simd_detail::input_slot_count<E>() == 0
                ? 1
                : pravaha::backends::simd_detail::input_slot_count<E>();
        const std::string_view t = compute::msl_scalar_name(elem);
        std::ostringstream os;
        os << "#include <metal_stdlib>\n"
            << "using namespace metal;\n"
            << "kernel void " << kernel_name << "(\n";
        for (std::size_t s = 0; s < K; ++s)
            os << "    device const " << t << "* src" << s
                << " [[buffer(" << s << ")]],\n";
        os << "    device " << t << "* dst [[buffer(" << K << ")]],\n"
            << "    constant uint& n [[buffer(" << (K + 1) << ")]],\n";
        for (std::size_t s = 0; s < K; ++s)
            os << "    constant uint& src" << s << "_off [[buffer(" << (K + 2 + s * 2) << ")]],\n"
                << "    constant uint& src" << s << "_str [[buffer(" << (K + 3 + s * 2) << ")]],\n";
        os << "    uint gid [[thread_position_in_grid]]) {\n"
            << "    if (gid >= n) return;\n";
        if constexpr (uses_input_leaves<E>()) {
            for (std::size_t s = 0; s < K; ++s)
                os << "    " << t << " x" << s << " = src" << s
                    << "[src" << s << "_off + gid * src" << s << "_str];\n";
            os << "    dst[gid] = ";
            emit_expr(os, expr, "x", /*indexed=*/true);
        }
        else {
            os << "    " << t << " x = src0[src0_off + gid * src0_str];\n"
                << "    dst[gid] = ";
            emit_expr(os, expr, "x", /*indexed=*/false);
        }
        os << ";\n}\n";
        return os.str();
    }

    [[nodiscard]] inline std::string_view reduce_init_msl(pravaha::expr::reduce_op op,
                                                          std::string_view t) {
        using ro = pravaha::expr::reduce_op;
        switch (op) {
        case ro::sum: return "0";
        case ro::max: return t == "float" ? "-INFINITY" : "-3.402823466e+38";
        case ro::min: return t == "float" ? "INFINITY" : "3.402823466e+38";
        }
        return "0";
    }

    // Emit the threadgroup reduction tree. For `sum` a Kahan-compensated add is used
    // (Metal has no f64, so compensation is the widest accuracy available on-device);
    // max/min use the plain tree. Assumes `scratch[lid]` holds each thread's value
    // and, for sum, `comp[lid]` holds its compensation term. Leaves the group result
    // in `scratch[0]`.
    inline void emit_reduce_tree_msl(std::ostream& os, pravaha::expr::reduce_op op,
                                     std::string_view t) {
        os << "    for (uint s = tgs / 2; s > 0; s >>= 1) {\n"
            << "        if (lid < s) {\n";
        if (op == pravaha::expr::reduce_op::sum) {
            // Kahan: add scratch[lid+s] (+its compensation) into scratch[lid].
            os << "            " << t << " y = scratch[lid + s] - (comp[lid] + comp[lid + s]);\n"
                << "            " << t << " tsum = scratch[lid] + y;\n"
                << "            comp[lid] = (tsum - scratch[lid]) - y;\n"
                << "            scratch[lid] = tsum;\n";
        }
        else {
            const char* comb = op == pravaha::expr::reduce_op::max
                                   ? "max(scratch[lid], scratch[lid + s])"
                                   : "min(scratch[lid], scratch[lid + s])";
            os << "            scratch[lid] = " << comb << ";\n";
        }
        os << "        }\n"
            << "        threadgroup_barrier(mem_flags::mem_threadgroup);\n"
            << "    }\n";
    }

    template <typename E, std::size_t TGSize = 256>
    [[nodiscard]] std::string emit_reduce_kernel(const E& child,
                                                 pravaha::expr::reduce_op op,
                                                 compute::data_element_type elem,
                                                 std::string_view kernel_name = "pravaha_reduce") {
        static_assert(TGSize > 0 && (TGSize & (TGSize - 1)) == 0, "TGSize must be a power of two");
        const std::string_view t = compute::msl_scalar_name(elem);
        const std::string_view init = reduce_init_msl(op, t);
        const char* comb = op == pravaha::expr::reduce_op::sum
                               ? "acc + v"
                               : op == pravaha::expr::reduce_op::max
                               ? "max(acc, v)"
                               : "min(acc, v)";
        std::ostringstream os;
        os << "#include <metal_stdlib>\n"
            << "using namespace metal;\n"
            << "kernel void " << kernel_name << "(\n"
            << "    device const " << t << "* src0 [[buffer(0)]],\n"
            << "    device " << t << "* partials [[buffer(1)]],\n"
            << "    constant uint& n [[buffer(2)]],\n"
            << "    uint gid  [[thread_position_in_grid]],\n"
            << "    uint lid  [[thread_position_in_threadgroup]],\n"
            << "    uint tgid [[threadgroup_position_in_grid]],\n"
            << "    uint tgs  [[threads_per_threadgroup]]) {\n"
            << "    threadgroup " << t << " scratch[" << TGSize << "];\n";
        if (op == pravaha::expr::reduce_op::sum)
            os << "    threadgroup " << t << " comp[" << TGSize << "];\n";
        os << "    " << t << " acc = " << init << ";\n"
            << "    if (gid < n) {\n"
            << "        " << t << (uses_input_leaves<E>()
                                       ? " x0 = src0[gid];\n"
                                       : " x = src0[gid];\n")
            << "        " << t << " v = ";
        emit_expr(os, child, "x", /*indexed=*/uses_input_leaves<E>());
        os << ";\n"
            << "        acc = " << comb << ";\n"
            << "    }\n"
            << "    scratch[lid] = acc;\n";
        if (op == pravaha::expr::reduce_op::sum)
            os << "    comp[lid] = 0;\n";
        os << "    threadgroup_barrier(mem_flags::mem_threadgroup);\n";
        emit_reduce_tree_msl(os, op, t);
        os << "    if (lid == 0) partials[tgid] = scratch[0];\n"
            << "}\n";
        return os.str();
    }

    template <typename E, std::size_t K, std::size_t TGSize = 256>
    [[nodiscard]] std::string emit_reduce_kernel_multi(const E& child,
                                                       pravaha::expr::reduce_op op,
                                                       compute::data_element_type elem,
                                                       std::string_view kernel_name = "pravaha_reduce_multi") {
        static_assert(TGSize > 0 && (TGSize & (TGSize - 1)) == 0, "TGSize must be a power of two");
        static_assert(K >= 1, "K must be at least 1");
        const std::string_view t = compute::msl_scalar_name(elem);
        const std::string_view init = reduce_init_msl(op, t);
        const char* comb = op == pravaha::expr::reduce_op::sum
                               ? "acc + v"
                               : op == pravaha::expr::reduce_op::max
                               ? "max(acc, v)"
                               : "min(acc, v)";
        std::ostringstream os;
        os << "#include <metal_stdlib>\n"
            << "using namespace metal;\n"
            << "kernel void " << kernel_name << "(\n";
        for (std::size_t s = 0; s < K; ++s)
            os << "    device const " << t << "* src" << s << " [[buffer(" << s << ")]],\n";
        os << "    device " << t << "* partials [[buffer(" << K << ")]],\n"
            << "    constant uint& n [[buffer(" << (K + 1) << ")]],\n"
            << "    uint gid  [[thread_position_in_grid]],\n"
            << "    uint lid  [[thread_position_in_threadgroup]],\n"
            << "    uint tgid [[threadgroup_position_in_grid]],\n"
            << "    uint tgs  [[threads_per_threadgroup]]) {\n"
            << "    threadgroup " << t << " scratch[" << TGSize << "];\n";
        if (op == pravaha::expr::reduce_op::sum)
            os << "    threadgroup " << t << " comp[" << TGSize << "];\n";
        os << "    " << t << " acc = " << init << ";\n"
            << "    if (gid < n) {\n";
        for (std::size_t s = 0; s < K; ++s)
            os << "        " << t << " x" << s << " = src" << s << "[gid];\n";
        os << "        " << t << " v = ";
        emit_expr(os, child, "x", /*indexed=*/true);
        os << ";\n"
            << "        acc = " << comb << ";\n"
            << "    }\n"
            << "    scratch[lid] = acc;\n";
        if (op == pravaha::expr::reduce_op::sum)
            os << "    comp[lid] = 0;\n";
        os << "    threadgroup_barrier(mem_flags::mem_threadgroup);\n";
        emit_reduce_tree_msl(os, op, t);
        os << "    if (lid == 0) partials[tgid] = scratch[0];\n"
            << "}\n";
        return os.str();
    }
} // namespace pravaha::backends::metal::msl

// Convenience aliases in the parent namespace.
namespace pravaha::backends::metal {
    using msl::emit_expr;
    using msl::emit_kernel;
    using msl::emit_reduce_kernel;
} // namespace pravaha::backends::metal

// ============================================================================
// Metal device + dispatch — macOS with metal-cpp only.
// ============================================================================

#if defined(__APPLE__) && defined(HAS_METAL_CPP)

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include <cstring>

namespace pravaha::backends::metal {
    inline constexpr std::size_t kReduceTG = 256;

    // An owned command-buffer completion token.  It is deliberately explicit:
    // callers can overlap independent CPU work before calling wait(), while the
    // ordinary dispatch APIs retain their synchronous, simple semantics.
    struct metal_submission {
        MTL::CommandBuffer* command_buffer = nullptr;

        metal_submission() = default;
        explicit metal_submission(MTL::CommandBuffer* value) noexcept : command_buffer(value) {}
        metal_submission(const metal_submission&) = delete;
        metal_submission& operator=(const metal_submission&) = delete;

        metal_submission(metal_submission&& other) noexcept
            : command_buffer(std::exchange(other.command_buffer, nullptr)) {}

        metal_submission& operator=(metal_submission&& other) noexcept {
            if (this != std::addressof(other)) {
                reset();
                command_buffer = std::exchange(other.command_buffer, nullptr);
            }
            return *this;
        }

        ~metal_submission() { reset(); }

        [[nodiscard]] explicit operator bool() const noexcept { return command_buffer != nullptr; }

        [[nodiscard]] Outcome<void> wait() const {
            if (!command_buffer)
                return std::unexpected(PravahaError::make(
                    ErrorKind::ExecutorUnavailable, "metal: empty submission"));
            command_buffer->waitUntilCompleted();
            if (auto* error = command_buffer->error(); error != nullptr)
                return std::unexpected(PravahaError::make(
                    ErrorKind::InternalError, error->localizedDescription()->utf8String()));
            return {};
        }

    private:
        void reset() noexcept {
            if (command_buffer) command_buffer->release();
            command_buffer = nullptr;
        }
    };

    // Reusable shared-memory buffers for repeated host-to-device elementwise
    // dispatches.  Buffer lifetime is caller-owned, so no allocation occurs
    // after capacity is established.  A caller must wait for a submission before
    // reusing or destroying this set.
    template <typename T, std::size_t K>
    struct metal_buffer_set {
        std::array<MTL::Buffer*, K> inputs{};
        MTL::Buffer* output = nullptr;
        std::size_t capacity = 0;

        metal_buffer_set() = default;
        metal_buffer_set(const metal_buffer_set&) = delete;
        metal_buffer_set& operator=(const metal_buffer_set&) = delete;
        ~metal_buffer_set() { reset(); }

        [[nodiscard]] Outcome<void> reserve(MTL::Device* device, const std::size_t count) {
            if (count == 0) return {};
            if (count <= capacity && output != nullptr) return {};
            if (!device)
                return std::unexpected(PravahaError::make(
                    ErrorKind::ExecutorUnavailable, "metal: no device for buffer allocation"));
            const auto bytes = count * sizeof(T);
            std::array<MTL::Buffer*, K> new_inputs{};
            for (auto& buffer : new_inputs) {
                buffer = device->newBuffer(bytes, MTL::ResourceStorageModeShared);
                if (!buffer) {
                    for (auto* allocated : new_inputs) if (allocated) allocated->release();
                    return std::unexpected(PravahaError::make(
                        ErrorKind::InternalError, "metal: input buffer allocation failed"));
                }
            }
            auto* new_output = device->newBuffer(bytes, MTL::ResourceStorageModeShared);
            if (!new_output) {
                for (auto* allocated : new_inputs) allocated->release();
                return std::unexpected(PravahaError::make(
                    ErrorKind::InternalError, "metal: output buffer allocation failed"));
            }
            reset();
            inputs = new_inputs;
            output = new_output;
            capacity = count;
            return {};
        }

        [[nodiscard]] Outcome<void> upload(
            const std::array<std::span<const T>, K>& source, const std::size_t count) const {
            if (count > capacity)
                return std::unexpected(PravahaError::make(
                    ErrorKind::InternalError, "metal: buffer set capacity is too small"));
            for (std::size_t i = 0; i < K; ++i) {
                if (source[i].size() < count)
                    return std::unexpected(PravahaError::make(
                        ErrorKind::InternalError, "metal: input span is shorter than dispatch domain"));
                std::memcpy(inputs[i]->contents(), source[i].data(), count * sizeof(T));
            }
            return {};
        }

        [[nodiscard]] Outcome<void> download(const std::span<T> destination,
                                             const std::size_t count) const {
            if (count > capacity || destination.size() < count)
                return std::unexpected(PravahaError::make(
                    ErrorKind::InternalError, "metal: output span is shorter than dispatch domain"));
            std::memcpy(destination.data(), output->contents(), count * sizeof(T));
            return {};
        }

    private:
        void reset() noexcept {
            for (auto*& buffer : inputs) {
                if (buffer) buffer->release();
                buffer = nullptr;
            }
            if (output) output->release();
            output = nullptr;
            capacity = 0;
        }
    };

    struct metal_gpu_backend {
        MTL::Device* device = nullptr;
        MTL::CommandQueue* queue = nullptr;

        ~metal_gpu_backend() {
            if (queue) {
                queue->release();
                queue = nullptr;
            }
            if (device) {
                device->release();
                device = nullptr;
            }
        }

        [[nodiscard]] static metal_gpu_backend& instance() {
            static metal_gpu_backend be = [] {
                metal_gpu_backend b;
                b.device = MTL::CreateSystemDefaultDevice();
                b.queue = b.device ? b.device->newCommandQueue() : nullptr;
                return b;
            }();
            return be;
        }

        [[nodiscard]] bool available() const noexcept { return device && queue; }

        [[nodiscard]] Outcome<MTL::ComputePipelineState*>
        compile(const std::string& msl_source,
                std::string_view fn_name = "pravaha_kernel") const {
            if (!available())
                return std::unexpected(PravahaError::make(
                    ErrorKind::ExecutorUnavailable, "metal: no default device"));

            NS::Error* err = nullptr;
            NS::String* src = NS::String::string(msl_source.c_str(), NS::UTF8StringEncoding);
            MTL::Library* lib = device->newLibrary(src, nullptr, &err);
            if (!lib)
                return std::unexpected(PravahaError::make(
                    ErrorKind::InternalError,
                    err ? err->localizedDescription()->utf8String() : "metal: newLibrary failed"));

            NS::String* fname = NS::String::string(std::string(fn_name).c_str(), NS::UTF8StringEncoding);
            MTL::Function* fn = lib->newFunction(fname);
            lib->release();
            if (!fn)
                return std::unexpected(PravahaError::make(
                    ErrorKind::SymbolNotFound, "metal: kernel function not found"));

            MTL::ComputePipelineState* pso = device->newComputePipelineState(fn, &err);
            fn->release();
            if (!pso)
                return std::unexpected(PravahaError::make(
                    ErrorKind::InternalError,
                    err
                        ? err->localizedDescription()->utf8String()
                        : "metal: newComputePipelineState failed"));
            return pso;
        }

        template <typename T, std::size_t K>
        [[nodiscard]] Outcome<metal_submission> dispatch_multi_async(
            MTL::ComputePipelineState* pso,
            const metal_buffer_set<T, K>& buffers,
            const std::size_t count,
            const hetero::compute_grid_descriptor& grid) {
            if (!available() || !pso || count > buffers.capacity)
                return std::unexpected(PravahaError::make(
                    ErrorKind::ExecutorUnavailable, "metal: asynchronous dispatch is unavailable"));
            auto* command_buffer = queue->commandBuffer();
            auto* encoder = command_buffer->computeCommandEncoder();
            encoder->setComputePipelineState(pso);
            for (std::size_t i = 0; i < K; ++i) encoder->setBuffer(buffers.inputs[i], 0, i);
            encoder->setBuffer(buffers.output, 0, K);
            const auto n = static_cast<std::uint32_t>(count);
            encoder->setBytes(&n, sizeof(n), K + 1);
            const auto threads = std::min<NS::UInteger>(pso->maxTotalThreadsPerThreadgroup(),
                                                        grid.local_size[0] ? grid.local_size[0] : 256);
            encoder->dispatchThreads(MTL::Size(n, 1, 1), MTL::Size(threads, 1, 1));
            encoder->endEncoding();
            command_buffer->commit();
            return metal_submission{command_buffer};
        }

        // Device-resident variant: callers own the Metal buffers and can feed
        // one kernel's output directly into another kernel's input.  It performs
        // no host allocation and no host/device copy.
        template <std::size_t K>
        [[nodiscard]] Outcome<metal_submission> dispatch_device_multi_async(
            MTL::ComputePipelineState* pso,
            MTL::Buffer* output,
            const std::array<MTL::Buffer*, K>& inputs,
            const std::size_t count,
            const hetero::compute_grid_descriptor& grid) {
            if (!available() || !pso || !output || count == 0
                || std::ranges::any_of(inputs, [](const auto* buffer) { return buffer == nullptr; }))
                return std::unexpected(PravahaError::make(
                    ErrorKind::ExecutorUnavailable, "metal: device-resident dispatch is unavailable"));
            auto* command_buffer = queue->commandBuffer();
            auto* encoder = command_buffer->computeCommandEncoder();
            encoder->setComputePipelineState(pso);
            for (std::size_t i = 0; i < K; ++i) encoder->setBuffer(inputs[i], 0, i);
            encoder->setBuffer(output, 0, K);
            const auto n = static_cast<std::uint32_t>(count);
            encoder->setBytes(&n, sizeof(n), K + 1);
            const auto threads = std::min<NS::UInteger>(pso->maxTotalThreadsPerThreadgroup(),
                                                        grid.local_size[0] ? grid.local_size[0] : 256);
            encoder->dispatchThreads(MTL::Size(n, 1, 1), MTL::Size(threads, 1, 1));
            encoder->endEncoding();
            command_buffer->commit();
            return metal_submission{command_buffer};
        }

        template <typename T, std::size_t K>
        [[nodiscard]] Outcome<void> dispatch_multi(
            MTL::ComputePipelineState* pso,
            metal_buffer_set<T, K>& buffers,
            const std::span<T> destination,
            const std::array<std::span<const T>, K>& source,
            const hetero::compute_grid_descriptor& grid) {
            const auto count = source.front().size();
            if (count == 0) return {};
            if (destination.size() < count)
                return std::unexpected(PravahaError::make(
                    ErrorKind::InternalError, "metal: output span is shorter than dispatch domain"));
            if (const auto reserved = buffers.reserve(device, count); !reserved) return reserved;
            if (const auto uploaded = buffers.upload(source, count); !uploaded) return uploaded;
            auto submission = dispatch_multi_async(pso, buffers, count, grid);
            if (!submission) return std::unexpected(submission.error());
            if (const auto completed = submission->wait(); !completed) return completed;
            return buffers.download(destination, count);
        }

        template <typename T>
        Outcome<void> dispatch(MTL::ComputePipelineState* pso,
                               compute::compute_view<T> dst,
                               compute::compute_view<const T> src,
                               const hetero::compute_grid_descriptor& grid) {
            if (!available())
                return std::unexpected(PravahaError::make(
                    ErrorKind::ExecutorUnavailable, "metal: unavailable"));

            const std::uint32_t n = static_cast<std::uint32_t>(src.desc.element_count());
            const std::size_t bytes = static_cast<std::size_t>(n) * sizeof(T);

            const bool zero_copy = src.desc.is_unified && dst.desc.is_unified
                && is_page_aligned(src.base())
                && is_page_aligned(dst.base());

            MTL::Buffer* src_buf;
            MTL::Buffer* dst_buf;
            if (zero_copy) {
                src_buf = device->newBuffer(const_cast<T*>(src.base()), bytes,
                                            MTL::ResourceStorageModeShared, nullptr);
                dst_buf = device->newBuffer(dst.base(), bytes,
                                            MTL::ResourceStorageModeShared, nullptr);
            }
            else {
                src_buf = device->newBuffer(bytes, MTL::ResourceStorageModeShared);
                dst_buf = device->newBuffer(bytes, MTL::ResourceStorageModeShared);
                std::memcpy(src_buf->contents(), src.base(), bytes);
            }

            MTL::CommandBuffer* cb = queue->commandBuffer();
            MTL::ComputeCommandEncoder* enc = cb->computeCommandEncoder();
            enc->setComputePipelineState(pso);
            enc->setBuffer(src_buf, 0, 0);
            enc->setBuffer(dst_buf, 0, 1);
            enc->setBytes(&n, sizeof(n), 2);

            const NS::UInteger tg =
                std::min<NS::UInteger>(pso->maxTotalThreadsPerThreadgroup(),
                                       grid.local_size[0] ? grid.local_size[0] : 256);
            enc->dispatchThreads(MTL::Size(n, 1, 1), MTL::Size(tg, 1, 1));
            enc->endEncoding();
            cb->commit();
            cb->waitUntilCompleted();

            if (!zero_copy) std::memcpy(dst.base(), dst_buf->contents(), bytes);

            src_buf->release();
            dst_buf->release();
            return {};
        }

        template <typename T, std::size_t K>
        Outcome<void> dispatch_multi(MTL::ComputePipelineState* pso,
                                     compute::compute_view<T> dst,
                                     const std::array<compute::compute_view<const T>, K>& srcs,
                                     const hetero::compute_grid_descriptor& grid) {
            if (!available())
                return std::unexpected(PravahaError::make(
                    ErrorKind::ExecutorUnavailable, "metal: unavailable"));

            const std::uint32_t n = static_cast<std::uint32_t>(srcs[0].desc.element_count());
            const std::size_t bytes = static_cast<std::size_t>(n) * sizeof(T);

            std::array<MTL::Buffer*, K> src_bufs{};
            for (std::size_t s = 0; s < K; ++s) {
                const bool zc = srcs[s].desc.is_unified && is_page_aligned(srcs[s].base());
                if (zc) {
                    src_bufs[s] = device->newBuffer(const_cast<T*>(srcs[s].base()), bytes,
                                                    MTL::ResourceStorageModeShared, nullptr);
                }
                else {
                    src_bufs[s] = device->newBuffer(bytes, MTL::ResourceStorageModeShared);
                    std::memcpy(src_bufs[s]->contents(), srcs[s].base(), bytes);
                }
            }

            const bool dst_zc = dst.desc.is_unified && is_page_aligned(dst.base());
            MTL::Buffer* dst_buf = dst_zc
                                       ? device->newBuffer(dst.base(), bytes, MTL::ResourceStorageModeShared, nullptr)
                                       : device->newBuffer(bytes, MTL::ResourceStorageModeShared);

            MTL::CommandBuffer* cb = queue->commandBuffer();
            MTL::ComputeCommandEncoder* enc = cb->computeCommandEncoder();
            enc->setComputePipelineState(pso);
            for (std::size_t s = 0; s < K; ++s) enc->setBuffer(src_bufs[s], 0, s);
            enc->setBuffer(dst_buf, 0, K);
            enc->setBytes(&n, sizeof(n), K + 1);

            const NS::UInteger tg =
                std::min<NS::UInteger>(pso->maxTotalThreadsPerThreadgroup(),
                                       grid.local_size[0] ? grid.local_size[0] : 256);
            enc->dispatchThreads(MTL::Size(n, 1, 1), MTL::Size(tg, 1, 1));
            enc->endEncoding();
            cb->commit();
            cb->waitUntilCompleted();

            if (!dst_zc) std::memcpy(dst.base(), dst_buf->contents(), bytes);

            for (std::size_t s = 0; s < K; ++s) src_bufs[s]->release();
            dst_buf->release();
            return {};
        }

        template <typename T, std::size_t K>
        Outcome<void> dispatch_strided(MTL::ComputePipelineState* pso,
                                       compute::compute_view<T> dst,
                                       const std::array<compute::compute_view<const T>, K>& srcs,
                                       const hetero::compute_grid_descriptor& grid) {
            if (!available())
                return std::unexpected(PravahaError::make(
                    ErrorKind::ExecutorUnavailable, "metal: unavailable"));

            const std::uint32_t n = static_cast<std::uint32_t>(srcs[0].desc.element_count());
            const std::size_t bytes_each = static_cast<std::size_t>(n) * sizeof(T);

            std::array<MTL::Buffer*, K> src_bufs{};
            for (std::size_t s = 0; s < K; ++s) {
                const std::size_t src_bytes = srcs[s].desc.element_count() * sizeof(T);
                const bool zc = srcs[s].desc.is_unified && is_page_aligned(srcs[s].data);
                if (zc) {
                    src_bufs[s] = device->newBuffer(const_cast<T*>(srcs[s].data), src_bytes,
                                                    MTL::ResourceStorageModeShared, nullptr);
                }
                else {
                    src_bufs[s] = device->newBuffer(src_bytes, MTL::ResourceStorageModeShared);
                    std::memcpy(src_bufs[s]->contents(), srcs[s].data, src_bytes);
                }
            }

            const bool dst_zc = dst.desc.is_unified && is_page_aligned(dst.base());
            MTL::Buffer* dst_buf = dst_zc
                                       ? device->newBuffer(dst.base(), bytes_each, MTL::ResourceStorageModeShared,
                                                           nullptr)
                                       : device->newBuffer(bytes_each, MTL::ResourceStorageModeShared);

            MTL::CommandBuffer* cb = queue->commandBuffer();
            MTL::ComputeCommandEncoder* enc = cb->computeCommandEncoder();
            enc->setComputePipelineState(pso);
            for (std::size_t s = 0; s < K; ++s) enc->setBuffer(src_bufs[s], 0, s);
            enc->setBuffer(dst_buf, 0, K);
            enc->setBytes(&n, sizeof(n), K + 1);
            for (std::size_t s = 0; s < K; ++s) {
                const std::uint32_t off = static_cast<std::uint32_t>(srcs[s].offset);
                const std::uint32_t str = static_cast<std::uint32_t>(srcs[s].inner_stride());
                enc->setBytes(&off, sizeof(off), K + 2 + s * 2);
                enc->setBytes(&str, sizeof(str), K + 3 + s * 2);
            }

            const NS::UInteger tg =
                std::min<NS::UInteger>(pso->maxTotalThreadsPerThreadgroup(),
                                       grid.local_size[0] ? grid.local_size[0] : 256);
            enc->dispatchThreads(MTL::Size(n, 1, 1), MTL::Size(tg, 1, 1));
            enc->endEncoding();
            cb->commit();
            cb->waitUntilCompleted();

            if (!dst_zc) std::memcpy(dst.base(), dst_buf->contents(), bytes_each);

            for (std::size_t s = 0; s < K; ++s) src_bufs[s]->release();
            dst_buf->release();
            return {};
        }

        template <typename T>
        Outcome<std::vector<T>> dispatch_reduce(MTL::ComputePipelineState* pso,
                                                compute::compute_view<const T> src,
                                                std::size_t tg_size) {
            if (!available())
                return std::unexpected(PravahaError::make(
                    ErrorKind::ExecutorUnavailable, "metal: unavailable"));

            const std::uint32_t n = static_cast<std::uint32_t>(src.desc.element_count());
            if (n == 0) return std::vector<T>{};
            const std::size_t bytes = static_cast<std::size_t>(n) * sizeof(T);

            const NS::UInteger tg =
                std::min<NS::UInteger>(pso->maxTotalThreadsPerThreadgroup(),
                                       std::min<NS::UInteger>(tg_size ? tg_size : kReduceTG,
                                                              kReduceTG));
            assert(tg <= static_cast<NS::UInteger>(kReduceTG) &&
                "dispatch_reduce: tg exceeds kReduceTG — kernel scratch too small");
            const std::uint32_t groups = (n + static_cast<std::uint32_t>(tg) - 1)
                / static_cast<std::uint32_t>(tg);

            const bool zc = src.desc.is_unified && is_page_aligned(src.base());
            MTL::Buffer* src_buf = zc
                                       ? device->newBuffer(const_cast<T*>(src.base()), bytes,
                                                           MTL::ResourceStorageModeShared, nullptr)
                                       : device->newBuffer(bytes, MTL::ResourceStorageModeShared);
            if (!zc) std::memcpy(src_buf->contents(), src.base(), bytes);

            MTL::Buffer* part_buf = device->newBuffer(
                static_cast<std::size_t>(groups) * sizeof(T), MTL::ResourceStorageModeShared);

            MTL::CommandBuffer* cb = queue->commandBuffer();
            MTL::ComputeCommandEncoder* enc = cb->computeCommandEncoder();
            enc->setComputePipelineState(pso);
            enc->setBuffer(src_buf, 0, 0);
            enc->setBuffer(part_buf, 0, 1);
            enc->setBytes(&n, sizeof(n), 2);
            enc->dispatchThreads(MTL::Size(static_cast<NS::UInteger>(groups) * tg, 1, 1),
                                 MTL::Size(tg, 1, 1));
            enc->endEncoding();
            cb->commit();
            cb->waitUntilCompleted();

            std::vector<T> partials(groups);
            std::memcpy(partials.data(), part_buf->contents(),
                        static_cast<std::size_t>(groups) * sizeof(T));

            src_buf->release();
            part_buf->release();
            return partials;
        }

        template <typename T, std::size_t K>
        Outcome<std::vector<T>> dispatch_reduce_multi(
            MTL::ComputePipelineState* pso,
            const std::array<compute::compute_view<const T>, K>& srcs,
            std::size_t tg_size) {
            if (!available())
                return std::unexpected(PravahaError::make(
                    ErrorKind::ExecutorUnavailable, "metal: unavailable"));

            const std::uint32_t n = static_cast<std::uint32_t>(srcs[0].desc.element_count());
            if (n == 0) return std::vector<T>{};
            const std::size_t bytes = static_cast<std::size_t>(n) * sizeof(T);

            const NS::UInteger tg =
                std::min<NS::UInteger>(pso->maxTotalThreadsPerThreadgroup(),
                                       std::min<NS::UInteger>(tg_size ? tg_size : kReduceTG,
                                                              kReduceTG));
            assert(tg <= static_cast<NS::UInteger>(kReduceTG) &&
                "dispatch_reduce_multi: tg exceeds kReduceTG");
            const std::uint32_t groups = (n + static_cast<std::uint32_t>(tg) - 1)
                / static_cast<std::uint32_t>(tg);

            std::array<MTL::Buffer*, K> src_bufs{};
            for (std::size_t s = 0; s < K; ++s) {
                const bool zc = srcs[s].desc.is_unified && is_page_aligned(srcs[s].base());
                if (zc) {
                    src_bufs[s] = device->newBuffer(const_cast<T*>(srcs[s].base()), bytes,
                                                    MTL::ResourceStorageModeShared, nullptr);
                }
                else {
                    src_bufs[s] = device->newBuffer(bytes, MTL::ResourceStorageModeShared);
                    std::memcpy(src_bufs[s]->contents(), srcs[s].base(), bytes);
                }
            }

            MTL::Buffer* part_buf = device->newBuffer(
                static_cast<std::size_t>(groups) * sizeof(T), MTL::ResourceStorageModeShared);

            MTL::CommandBuffer* cb = queue->commandBuffer();
            MTL::ComputeCommandEncoder* enc = cb->computeCommandEncoder();
            enc->setComputePipelineState(pso);
            for (std::size_t s = 0; s < K; ++s) enc->setBuffer(src_bufs[s], 0, s);
            enc->setBuffer(part_buf, 0, K);
            enc->setBytes(&n, sizeof(n), K + 1);
            enc->dispatchThreads(MTL::Size(static_cast<NS::UInteger>(groups) * tg, 1, 1),
                                 MTL::Size(tg, 1, 1));
            enc->endEncoding();
            cb->commit();
            cb->waitUntilCompleted();

            std::vector<T> partials(groups);
            std::memcpy(partials.data(), part_buf->contents(),
                        static_cast<std::size_t>(groups) * sizeof(T));

            for (std::size_t s = 0; s < K; ++s) src_bufs[s]->release();
            part_buf->release();
            return partials;
        }

    private:
        static bool is_page_aligned(const void* p) noexcept {
            return (reinterpret_cast<std::uintptr_t>(p) & (4096u - 1u)) == 0;
        }
    };

    // ============================================================================
    // Part 4: Kosha kernel cache
    // ============================================================================

    using pipeline_ptr = NS::SharedPtr<MTL::ComputePipelineState>;
    using pipeline_cache =
    kosha::adapter::ShardedCache<
        kosha::core::Cache<std::uint64_t, pipeline_ptr>,
        8 /*shards*/>;

    [[nodiscard]] inline pipeline_cache& kernel_cache() {
        static pipeline_cache cache{256};
        return cache;
    }

    // Source kernels (including Lithe's shared HL-MIR lowering) use the same
    // bounded Kosha cache as Pravaha expression kernels.  The caller supplies a
    // stable semantic key, so this does not make source text or Metal handles
    // part of Lithe's persistent artifact format.
    [[nodiscard]] inline Outcome<pipeline_ptr>
    get_or_compile_source(const std::uint64_t key,
                          const std::string_view msl_source,
                          const std::string_view function_name) {
        if (auto hit = kernel_cache().peek(key); hit.has_value()) {
            hetero::emit_cache_event(true, key);
            return *hit;
        }
        hetero::emit_cache_event(false, key);
        auto compiled = metal_gpu_backend::instance().compile(
            std::string(msl_source), function_name);
        if (!compiled) return std::unexpected(compiled.error());
        pipeline_ptr pipeline = NS::TransferPtr(*compiled);
        [[maybe_unused]] auto inserted = kernel_cache().put(key, pipeline);
        return pipeline;
    }

    template <typename E>
    [[nodiscard]] Outcome<pipeline_ptr>
    get_or_compile(const E& expr, compute::data_element_type elem) {
        const std::uint64_t key = hetero::structural_hash(expr);

        if (auto hit = kernel_cache().peek(key); hit.has_value()) {
            hetero::emit_cache_event(true, key);
            return *hit;
        }
        hetero::emit_cache_event(false, key);

        auto& be = metal_gpu_backend::instance();
        std::string msl = msl::emit_kernel(expr, elem);
        auto pso_r = be.compile(msl);
        if (!pso_r) return std::unexpected(pso_r.error());

        pipeline_ptr ptr = NS::TransferPtr(*pso_r);
        [[maybe_unused]] auto ins = kernel_cache().put(key, ptr);
        return ptr;
    }

    template <pravaha::expr::reduce_op Op, typename Child>
    [[nodiscard]] Outcome<pipeline_ptr>
    get_or_compile_reduce(const Child& child, compute::data_element_type elem) {
        const std::uint64_t key = (hetero::structural_hash(child) * 31u)
            ^ (static_cast<std::uint64_t>(Op) | 0x8000000000000000ULL);

        if (auto hit = kernel_cache().peek(key); hit.has_value()) {
            hetero::emit_cache_event(true, key);
            return *hit;
        }
        hetero::emit_cache_event(false, key);

        auto& be = metal_gpu_backend::instance();
        std::string msl = msl::emit_reduce_kernel(child, Op, elem);
        auto pso_r = be.compile(msl, "pravaha_reduce");
        if (!pso_r) return std::unexpected(pso_r.error());

        pipeline_ptr ptr = NS::TransferPtr(*pso_r);
        [[maybe_unused]] auto ins = kernel_cache().put(key, ptr);
        return ptr;
    }

    template <pravaha::expr::reduce_op Op, std::size_t K, typename Child>
    [[nodiscard]] Outcome<pipeline_ptr>
    get_or_compile_reduce_multi(const Child& child, compute::data_element_type elem) {
        const std::uint64_t key = (hetero::structural_hash(child) * 37u)
            ^ (static_cast<std::uint64_t>(Op) | 0xC000000000000000ULL)
            ^ (static_cast<std::uint64_t>(K) << 48);

        if (auto hit = kernel_cache().peek(key); hit.has_value()) {
            hetero::emit_cache_event(true, key);
            return *hit;
        }
        hetero::emit_cache_event(false, key);

        auto& be = metal_gpu_backend::instance();
        std::string msl_src = msl::emit_reduce_kernel_multi<Child, K>(child, Op, elem,
                                                                      "pravaha_reduce_multi");
        auto pso_r = be.compile(msl_src, "pravaha_reduce_multi");
        if (!pso_r) return std::unexpected(pso_r.error());

        pipeline_ptr ptr = NS::TransferPtr(*pso_r);
        [[maybe_unused]] auto ins = kernel_cache().put(key, ptr);
        return ptr;
    }

    template <typename E>
    [[nodiscard]] Outcome<pipeline_ptr>
    get_or_compile_strided(const E& expr, compute::data_element_type elem) {
        const std::uint64_t key = (hetero::structural_hash(expr) * 29u)
            ^ 0x4000000000000000ULL;

        if (auto hit = kernel_cache().peek(key); hit.has_value()) {
            hetero::emit_cache_event(true, key);
            return *hit;
        }
        hetero::emit_cache_event(false, key);

        auto& be = metal_gpu_backend::instance();
        std::string msl_src = msl::emit_kernel_strided(expr, elem, "pravaha_kernel_strided");
        auto pso_r = be.compile(msl_src, "pravaha_kernel_strided");
        if (!pso_r) return std::unexpected(pso_r.error());

        pipeline_ptr ptr = NS::TransferPtr(*pso_r);
        [[maybe_unused]] auto ins = kernel_cache().put(key, ptr);
        return ptr;
    }

    template <typename T, typename E>
    Outcome<void> run_gpu_uncached(const E& expr,
                                   compute::compute_view<T> dst,
                                   compute::compute_view<const T> src) {
        auto& be = metal_gpu_backend::instance();
        if (!be.available())
            return std::unexpected(PravahaError::make(
                ErrorKind::ExecutorUnavailable, "metal: no GPU"));

        std::string msl_src = msl::emit_kernel(expr, src.desc.element_type);
        auto pso_r = be.compile(msl_src);
        if (!pso_r) return std::unexpected(pso_r.error());

        auto grid = hetero::compute_grid_descriptor::from_flat(src.desc.element_count());
        auto r = be.dispatch(*pso_r, dst, src, grid);
        (*pso_r)->release();
        return r;
    }
} // namespace pravaha::backends::metal

// ============================================================================
// hetero_execute_gpu_impl — body of hetero_executor GPU path.
// ============================================================================

namespace pravaha::hetero {
    template <typename T, typename E>
    Outcome<void> hetero_execute_gpu_impl(const E& expr,
                                          compute::compute_view<T> dst,
                                          compute::compute_view<const T> src) {
        auto& be = backends::metal::metal_gpu_backend::instance();
        if (!be.available()) {
            emit_fallback_event("no GPU device → SIMD");
            execution_context empty;
            return backends::run_simd_or_fallback<T>(expr, dst, src, empty);
        }
        if (!src.is_contiguous() || !dst.is_contiguous()) {
            if (dst.is_contiguous() && src.desc.element_count() > 0) {
                auto pso_s = backends::metal::get_or_compile_strided(expr, src.desc.element_type);
                if (pso_s) {
                    std::array<compute::compute_view<const T>, 1> srcs{src};
                    auto grid = compute_grid_descriptor::from_flat(src.desc.element_count());
                    nadi_gpu_dispatch_scope _guard{src.desc.footprint_bytes()};
                    return be.template dispatch_strided<T, 1>((*pso_s).get(), dst, srcs, grid);
                }
            }
            emit_fallback_event("strided view → SIMD");
            execution_context empty;
            return backends::run_simd_or_fallback<T>(expr, dst, src, empty);
        }
        auto pso = backends::metal::get_or_compile(expr, src.desc.element_type);
        if (!pso) {
            emit_fallback_event("gpu compile failed → SIMD");
            execution_context empty;
            return backends::run_simd_or_fallback<T>(expr, dst, src, empty);
        }
        auto grid = compute_grid_descriptor::from_flat(src.desc.element_count());
        nadi_gpu_dispatch_scope _guard{src.desc.footprint_bytes()};
        return be.dispatch((*pso).get(), dst, src, grid);
    }

    template <typename T, std::size_t K, typename E>
    Outcome<void> hetero_execute_gpu_impl_multi(
        const E& expr,
        compute::compute_view<T> dst,
        const std::array<compute::compute_view<const T>, K>& srcs) {
        auto& be = backends::metal::metal_gpu_backend::instance();
        if (!be.available()) {
            emit_fallback_event("no GPU device → SIMD");
            execution_context empty;
            return backends::run_simd_or_fallback<T, K>(expr, dst, srcs, empty);
        }
        bool all_contig = dst.is_contiguous();
        for (std::size_t s = 0; s < K; ++s) all_contig = all_contig && srcs[s].is_contiguous();
        if (!all_contig) {
            if (dst.is_contiguous() && srcs[0].desc.element_count() > 0) {
                auto pso_s = backends::metal::get_or_compile_strided(expr, srcs[0].desc.element_type);
                if (pso_s) {
                    auto grid = compute_grid_descriptor::from_flat(srcs[0].desc.element_count());
                    nadi_gpu_dispatch_scope _guard{srcs[0].desc.footprint_bytes()};
                    return be.template dispatch_strided<T, K>((*pso_s).get(), dst, srcs, grid);
                }
            }
            emit_fallback_event("strided view → SIMD");
            execution_context empty;
            return backends::run_simd_or_fallback<T, K>(expr, dst, srcs, empty);
        }
        auto pso = backends::metal::get_or_compile(expr, srcs[0].desc.element_type);
        if (!pso) {
            emit_fallback_event("gpu compile failed → SIMD");
            execution_context empty;
            return backends::run_simd_or_fallback<T, K>(expr, dst, srcs, empty);
        }
        auto grid = compute_grid_descriptor::from_flat(srcs[0].desc.element_count());
        nadi_gpu_dispatch_scope _guard{srcs[0].desc.footprint_bytes()};
        return be.template dispatch_multi<T, K>((*pso).get(), dst, srcs, grid);
    }

    template <expr::reduce_op Op, typename T, typename Child>
    Outcome<T> hetero_reduce_gpu_impl(const Child& child,
                                      compute::compute_view<const T> src) {
        auto& be = backends::metal::metal_gpu_backend::instance();
        if (!be.available()) {
            emit_fallback_event("no GPU device → SIMD reduce");
            return backends::run_reduce_simd < Op, T > (child, src);
        }
        if (!src.is_contiguous()) {
            emit_fallback_event("strided view → SIMD reduce");
            return backends::run_reduce_simd < Op, T > (child, src);
        }
        auto pso = backends::metal::get_or_compile_reduce<Op>(child, src.desc.element_type);
        if (!pso) {
            emit_fallback_event("gpu reduce compile failed → SIMD");
            return backends::run_reduce_simd < Op, T > (child, src);
        }

        nadi_gpu_dispatch_scope _guard{src.desc.footprint_bytes()};
        auto partials = be.template dispatch_reduce<T>((*pso).get(), src, backends::metal::kReduceTG);
        if (!partials) return std::unexpected(partials.error());

        // Fold threadgroup partials in a wide accumulator (f64 for f32 sum) — there
        // can be ~N/256 partials, so an f32 fold here would lose precision at large N.
        using A = backends::simd_detail::sum_accum_t<Op, T>;
        A acc = backends::simd_detail::reduce_identity<Op, A>();
        for (T p : *partials) acc = backends::simd_detail::reduce_combine < Op > (acc, static_cast<A>(p));
        return static_cast<T>(acc);
    }

    template <expr::reduce_op Op, typename T, std::size_t K, typename Child>
    Outcome<T> hetero_reduce_gpu_impl_multi(
        const Child& child,
        const std::array<compute::compute_view<const T>, K>& srcs) {
        auto& be = backends::metal::metal_gpu_backend::instance();
        if (!be.available()) {
            emit_fallback_event("no GPU device → SIMD multi-reduce");
            return backends::run_reduce_simd_multi < Op, T, K > (child, srcs);
        }
        bool all_contig = true;
        for (std::size_t s = 0; s < K; ++s)
            if (!srcs[s].is_contiguous()) {
                all_contig = false;
                break;
            }
        if (!all_contig) {
            emit_fallback_event("strided view → SIMD multi-reduce");
            return backends::run_reduce_simd_multi < Op, T, K > (child, srcs);
        }
        auto pso = backends::metal::get_or_compile_reduce_multi<Op, K>(child, srcs[0].desc.element_type);
        if (!pso) {
            emit_fallback_event("gpu multi-reduce compile failed → SIMD");
            return backends::run_reduce_simd_multi < Op, T, K > (child, srcs);
        }

        nadi_gpu_dispatch_scope _guard{srcs[0].desc.footprint_bytes()};
        auto partials = be.template dispatch_reduce_multi<T, K>((*pso).get(), srcs,
                                                                backends::metal::kReduceTG);
        if (!partials) return std::unexpected(partials.error());

        using A = backends::simd_detail::sum_accum_t<Op, T>;
        A acc = backends::simd_detail::reduce_identity<Op, A>();
        for (T p : *partials) acc = backends::simd_detail::reduce_combine < Op > (acc, static_cast<A>(p));
        return static_cast<T>(acc);
    }
} // namespace pravaha::hetero

// ============================================================================
// MetalGpuBackend — ComputeBackend wrapper around the Metal dispatch path.
// Guard stays here — is_available() returns false at runtime when no GPU.
// High priority (200) so the cost model prefers it over SIMD for large buffers.
// f64 is rejected via supports_type() since Metal has no float64 scalar.
// ============================================================================

namespace pravaha::backends::metal {
    struct MetalGpuBackend {
        [[nodiscard]] static constexpr compute::backend_metadata static_metadata() noexcept {
            return {.name = "metal_gpu", .hardware_priority = 200};
        }

        [[nodiscard]] bool is_available() const noexcept {
            return metal_gpu_backend::instance().available();
        }

        // f64 has no Metal scalar (see msl_scalar_name note in pravaha_hetero.hpp).
        [[nodiscard]] static constexpr bool supports_type(compute::data_element_type t) noexcept {
            return t != compute::data_element_type::f64 &&
                t != compute::data_element_type::complex128;
        }

        [[nodiscard]] bool supports_expression(std::size_t /*hash*/,
                                               compute::data_element_type t) const noexcept {
            return supports_type(t) && is_available();
        }

        // GPU is only worthwhile above these footprint thresholds (mirrors routing_policy
        // defaults). Below threshold evaluate_cost returns 0 → SIMD wins the cost race.
        static constexpr std::size_t kGpuElementwiseThreshold = 256 * 1024; // 256 KB
        static constexpr std::size_t kGpuReduceThreshold = 1024 * 1024; // 1 MB

        [[nodiscard]] std::uint64_t evaluate_cost(const compute::buffer_descriptor& desc,
                                                  std::size_t /*hash*/) const noexcept {
            if (!is_available() || !supports_type(desc.element_type)) return 0;
            if (desc.footprint_bytes() < kGpuElementwiseThreshold) return 0;
            return desc.footprint_bytes();
        }

        // Reductions have higher GPU break-even due to kernel launch + threadgroup sync.
        [[nodiscard]] std::uint64_t evaluate_reduce_cost(const compute::buffer_descriptor& desc,
                                                         std::size_t /*hash*/) const noexcept {
            if (!is_available() || !supports_type(desc.element_type)) return 0;
            if (desc.footprint_bytes() < kGpuReduceThreshold) return 0;
            return desc.footprint_bytes();
        }

        template <typename T, lithe::Expression E>
        Outcome<void> execute_elementwise(const E& expr,
                                          compute::compute_view<T> dst,
                                          compute::compute_view<const T> src,
                                          const hetero::execution_context& /*ctx*/) {
            return hetero::hetero_execute_gpu_impl<T>(expr, dst, src);
        }

        template <typename T, std::size_t K, lithe::Expression E>
        Outcome<void> execute_elementwise_multi(const E& expr,
                                                compute::compute_view<T> dst,
                                                const std::array<compute::compute_view<const T>, K>& srcs,
                                                const hetero::execution_context& /*ctx*/) {
            return hetero::hetero_execute_gpu_impl_multi<T, K>(expr, dst, srcs);
        }

        template <pravaha::expr::reduce_op Op, typename T, lithe::Expression Child>
        Outcome<T> execute_reduction(const Child& child,
                                     compute::compute_view<const T> src) {
            return hetero::hetero_reduce_gpu_impl<Op, T>(child, src);
        }

        template <pravaha::expr::reduce_op Op, typename T, std::size_t K, lithe::Expression Child>
        Outcome<T> execute_reduction_multi(const Child& child,
                                           const std::array<compute::compute_view<const T>, K>& srcs) {
            return hetero::hetero_reduce_gpu_impl_multi<Op, T, K>(child, srcs);
        }
    };
} // namespace pravaha::backends::metal

// ============================================================================
// Default backend set and executor — Metal + SIMD.
// Defined here (not in pravaha_hetero.hpp) so MetalGpuBackend is fully defined
// before it appears in the template argument list.
// Vulkan is a separate opt-in backend; it is not a prerequisite for Metal.
// ============================================================================

namespace pravaha::compute {
    using default_backend_set =
    backend_set<pravaha::backends::metal::MetalGpuBackend,
                pravaha::backends::HostSimdBackend>;
} // namespace pravaha::compute

namespace pravaha::hetero {
    using default_hetero_executor =
    basic_hetero_executor<pravaha::backends::metal::MetalGpuBackend,
                          pravaha::backends::HostSimdBackend>;
    using hetero_executor = default_hetero_executor;
} // namespace pravaha::hetero

#endif // __APPLE__ && HAS_METAL_CPP
