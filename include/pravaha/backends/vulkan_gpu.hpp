#pragma once
// ============================================================================
// backends/vulkan_gpu.hpp — Vulkan GPU backend for the Pravaha hetero overlay.
//
// Section 1: SPIR-V emitter (platform-independent, strategy 3b: direct word
//            emission for the Pravaha element-wise op surface + reductions).
//            No external SPIR-V compiler required — no new library dependency.
// Section 2: Device dispatch shim + buffer-binding (V1: reuses Lithe's
//            vulkan_backend for device/pipeline/fence lifetime; Pravaha owns
//            the data-plane VkBuffer alloc + vkUpdateDescriptorSets + dispatch).
// Section 3: Kosha pipeline cache — same pattern as metal_gpu.hpp.
// Section 4: VulkanGpuBackend — ComputeBackend adapter (priority = 150).
//
// Guarded: Sections 2–4 compile to nothing without HAS_MOLTENVK/HAS_VULKAN.
// Section 1 is always compiled (pure word/string gen — no Vk calls).
//
// Design invariants identical to metal_gpu.hpp:
//  (1) No AST contamination — all HW metadata in execution_context overlay.
//  (2) Device lifetime / pipeline compile / fences / pools → Lithe vulkan_backend.
//  (3) Buffer binding (data plane) → Pravaha V1 shim (Lithe stub binds no data).
//  (4) Kosha structural_hash key — same axis as Metal path.
// ============================================================================

#include "pravaha/pravaha_hetero.hpp"
#include "pravaha/detail/availability.hpp"
#include "containers/cache/kosha.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

// ============================================================================
// Section 1: SPIR-V emitter (platform-independent, strategy 3b)
//
// Emits 32-bit SPIR-V words directly for the Pravaha element-wise op surface.
// Supported: input<N> leaves, lit constants, add/sub/mul/div/neg, math builtins
// (sqrt/exp/log/sin/cos/abs via GLSLstd450), reduce sum/max/min.
//
// The emitter walks the Lithe AST using the same tag-dispatch pattern as the
// MSL emitter in metal_gpu.hpp — same semantics, different target text format.
//
// Output: lithe::codegen::backends::spirv_module (words + local_x) ready for
// Lithe's tag_invoke(compile_and_install_t, vulkan_backend, spirv_module&&).
// ============================================================================

#if PEBBLE_PRAVAHA_DETAIL_HAS_LITHE_VULKAN

#include "edsl/backends/lithe_codegen_vulkan_spirv_ir.hpp"

namespace pravaha::backends::vulkan::spirv {
    // ============================================================================
    // SPIR-V word builder — accumulates instructions into a word vector.
    // All emit_* functions add exactly (1 + operand_count) words each.
    // ============================================================================

    struct Builder {
        std::vector<std::uint32_t> words;
        std::uint32_t bound = 1; // next available result-id

        // Deferred constant section. SPIR-V logical layout forbids OpConstant inside
        // a function body — all constants must live in the global types/constants
        // block before the first OpFunction. Expression emission (lit leaves) and the
        // reduce tree (stride constants) both need constants while emitting function
        // instructions, so those OpConstant words are buffered here and spliced into
        // `words` just before OpFunction via splice_constants(). Deduped by (type,bits)
        // so repeated values reuse one result-id (valid SPIR-V requires unique
        // constants per type+value anyway).
        std::vector<std::uint32_t> constants;
        std::unordered_map<std::uint64_t, std::uint32_t> const_pool;

        [[nodiscard]] std::uint32_t fresh_id() noexcept { return bound++; }

        // Intern an OpConstant into the deferred section. `type_id` is the SPIR-V type
        // result-id, `bits` the raw 32-bit encoding (float bit-pattern or integer).
        [[nodiscard]] std::uint32_t const_scalar(std::uint32_t type_id, std::uint32_t bits) {
            const std::uint64_t k =
                (static_cast<std::uint64_t>(type_id) << 32) | bits;
            if (auto it = const_pool.find(k); it != const_pool.end())
                return it->second;
            const std::uint32_t id = fresh_id();
            constants.push_back((4u << 16) | 43u); // OpConstant
            constants.push_back(type_id);
            constants.push_back(id);
            constants.push_back(bits);
            const_pool.emplace(k, id);
            return id;
        }

        // Splice the buffered constants into `words` at the given index (the position
        // of the first OpFunction word), then clear the buffer. Returns the number of
        // words inserted so the caller can account for shifted indices if needed.
        void splice_constants(std::size_t at) {
            words.insert(words.begin() + static_cast<std::ptrdiff_t>(at),
                         constants.begin(), constants.end());
            constants.clear();
        }

        // Register an already-emitted inline OpConstant in the pool so later
        // const_scalar() calls with the same (type,bits) reuse its id instead of
        // emitting a duplicate — SPIR-V forbids two OpConstant of equal type+value.
        // Used for constants a type declaration must reference inline (e.g. the array
        // length), which cannot be deferred past that type's forward-reference-free use.
        void register_const(std::uint32_t type_id, std::uint32_t bits, std::uint32_t id) {
            const std::uint64_t k =
                (static_cast<std::uint64_t>(type_id) << 32) | bits;
            const_pool.emplace(k, id);
        }

        // Emit: opcode in low 16 bits, word-count in high 16 bits of first word.
        // Returns the first result-id written, or 0 for void instructions.

        // No result: opcode + rest
        template <typename... Args>
        void emit(std::uint16_t opcode, Args... args) {
            const std::uint32_t wc = static_cast<std::uint32_t>(1 + sizeof...(args));
            words.push_back((wc << 16) | opcode);
            (words.push_back(static_cast<std::uint32_t>(args)), ...);
        }

        // With result: opcode + result_type + result_id + rest
        template <typename... Args>
        [[nodiscard]] std::uint32_t emit_r(std::uint16_t opcode,
                                           std::uint32_t result_type,
                                           Args... args) {
            const std::uint32_t id = fresh_id();
            const std::uint32_t wc = static_cast<std::uint32_t>(1 + 1 + 1 + sizeof...(args));
            words.push_back((wc << 16) | opcode);
            words.push_back(result_type);
            words.push_back(id);
            (words.push_back(static_cast<std::uint32_t>(args)), ...);
            return id;
        }

        // Emit a string as packed little-endian 4-byte chunks (null-terminated).
        void emit_string(std::string_view s) {
            std::size_t i = 0;
            while (i < s.size()) {
                std::uint32_t w = 0;
                for (int b = 0; b < 4 && i < s.size(); ++b, ++i)
                    w |= static_cast<std::uint32_t>(static_cast<unsigned char>(s[i])) << (b * 8);
                words.push_back(w);
            }
            // Ensure null terminator (may already be included in last word).
            // If the string length is an exact multiple of 4, add a null word.
            if (s.size() % 4 == 0) words.push_back(0);
        }

        // Instruction with a string payload (name string): opcode + optional ids + string.
        void emit_with_string(std::uint16_t opcode,
                              const std::vector<std::uint32_t>& prefix_ids,
                              std::string_view s) {
            // Pre-compute string word count (includes null terminator, padded to 4 bytes).
            const std::size_t str_words = (s.size() + 4) / 4;
            const std::uint32_t wc = static_cast<std::uint32_t>(1 + prefix_ids.size() + str_words);
            words.push_back((wc << 16) | opcode);
            for (auto id : prefix_ids) words.push_back(id);
            emit_string(s);
        }
    };

    // SPIR-V opcodes used (unified opcode table §3.32).
    inline constexpr std::uint16_t kOpCapability = 17;
    inline constexpr std::uint16_t kOpExtension = 10;
    inline constexpr std::uint16_t kOpExtInstImport = 11;
    inline constexpr std::uint16_t kOpMemoryModel = 14;
    inline constexpr std::uint16_t kOpEntryPoint = 15;
    inline constexpr std::uint16_t kOpExecutionMode = 16;
    inline constexpr std::uint16_t kOpDecorate = 71;
    inline constexpr std::uint16_t kOpMemberDecorate = 72;
    inline constexpr std::uint16_t kOpTypeVoid = 19;
    inline constexpr std::uint16_t kOpTypeBool = 20;
    inline constexpr std::uint16_t kOpTypeInt = 21;
    inline constexpr std::uint16_t kOpTypeFloat = 22;
    inline constexpr std::uint16_t kOpTypeVector = 23;
    inline constexpr std::uint16_t kOpTypeArray = 28;
    inline constexpr std::uint16_t kOpTypeRuntimeArray = 29;
    inline constexpr std::uint16_t kOpTypeStruct = 30;
    inline constexpr std::uint16_t kOpTypePointer = 32;
    inline constexpr std::uint16_t kOpTypeFunction = 33;
    inline constexpr std::uint16_t kOpVariable = 59;
    inline constexpr std::uint16_t kOpLoad = 61;
    inline constexpr std::uint16_t kOpStore = 62;
    inline constexpr std::uint16_t kOpAccessChain = 65;
    inline constexpr std::uint16_t kOpFunction = 54;
    inline constexpr std::uint16_t kOpFunctionEnd = 56;
    inline constexpr std::uint16_t kOpLabel = 248;
    inline constexpr std::uint16_t kOpReturn = 253;
    inline constexpr std::uint16_t kOpCompositeExtract = 81;
    inline constexpr std::uint16_t kOpBitcast = 124;
    inline constexpr std::uint16_t kOpConvertUToF = 112;
    inline constexpr std::uint16_t kOpConvertFToU = 109;
    inline constexpr std::uint16_t kOpConvertSToF = 111;
    inline constexpr std::uint16_t kOpConvertFToS = 110;
    inline constexpr std::uint16_t kOpFAdd = 129;
    inline constexpr std::uint16_t kOpFSub = 131;
    inline constexpr std::uint16_t kOpFMul = 133;
    inline constexpr std::uint16_t kOpFDiv = 136;
    inline constexpr std::uint16_t kOpFNegate = 127;
    inline constexpr std::uint16_t kOpIAdd = 128;
    inline constexpr std::uint16_t kOpISub = 130;
    inline constexpr std::uint16_t kOpIMul = 132;
    inline constexpr std::uint16_t kOpSDiv = 135;
    inline constexpr std::uint16_t kOpUDiv = 134;
    inline constexpr std::uint16_t kOpSNegate = 126;
    inline constexpr std::uint16_t kOpExtInst = 12;
    inline constexpr std::uint16_t kOpULessThan = 176;
    inline constexpr std::uint16_t kOpIEqual = 170;
    inline constexpr std::uint16_t kOpSelect = 169;
    inline constexpr std::uint16_t kOpControlBarrier = 224;
    inline constexpr std::uint16_t kOpSelectionMerge = 247;
    inline constexpr std::uint16_t kOpBranch = 249;
    inline constexpr std::uint16_t kOpBranchConditional = 250;
    inline constexpr std::uint16_t kOpAtomicFAddEXT = 6000; // not used in simple path

    // GLSLstd450 extended instruction opcodes.
    inline constexpr std::uint32_t kGLSL_Sqrt = 31;
    inline constexpr std::uint32_t kGLSL_Exp = 27;
    inline constexpr std::uint32_t kGLSL_Log = 28;
    inline constexpr std::uint32_t kGLSL_Sin = 13;
    inline constexpr std::uint32_t kGLSL_Cos = 14;
    inline constexpr std::uint32_t kGLSL_FAbs = 4;
    inline constexpr std::uint32_t kGLSL_FMax = 40;
    inline constexpr std::uint32_t kGLSL_FMin = 37;
    inline constexpr std::uint32_t kGLSL_SMax = 42;
    inline constexpr std::uint32_t kGLSL_SMin = 39;
    inline constexpr std::uint32_t kGLSL_UMax = 41;
    inline constexpr std::uint32_t kGLSL_UMin = 38;

    // SPIR-V storage-class values.
    inline constexpr std::uint32_t kStorageClass_StorageBuffer = 12;
    inline constexpr std::uint32_t kStorageClass_Input = 1;
    inline constexpr std::uint32_t kStorageClass_Function = 7;
    inline constexpr std::uint32_t kStorageClass_Workgroup = 4;

    // SPIR-V decoration values.
    inline constexpr std::uint32_t kDecoration_Block = 2;
    inline constexpr std::uint32_t kDecoration_Binding = 33;
    inline constexpr std::uint32_t kDecoration_DescriptorSet = 34;
    inline constexpr std::uint32_t kDecoration_ArrayStride = 6;
    inline constexpr std::uint32_t kDecoration_NonWritable = 24;
    inline constexpr std::uint32_t kDecoration_Builtin = 11;
    inline constexpr std::uint32_t kBuiltin_GlobalInvocationId = 28;
    inline constexpr std::uint32_t kBuiltin_LocalInvocationId = 27;
    inline constexpr std::uint32_t kBuiltin_WorkgroupId = 26;

    // Memory/Access semantics.
    inline constexpr std::uint32_t kMemoryModel_GLSL450 = 1;
    inline constexpr std::uint32_t kAddressingModel_Logical = 0;
    inline constexpr std::uint32_t kCapability_Shader = 1;
    inline constexpr std::uint32_t kExecutionModel_GLCompute = 5;
    inline constexpr std::uint32_t kExecutionMode_LocalSize = 17;
    inline constexpr std::uint32_t kFunctionControl_None = 0;

    // Barrier scopes / memory semantics (OpControlBarrier operands).
    inline constexpr std::uint32_t kScope_Workgroup = 2;
    inline constexpr std::uint32_t kSelectionControl_None = 0;
    inline constexpr std::uint32_t kLoopControl_None = 0;
    // MemorySemantics: AcquireRelease (0x8) | WorkgroupMemory (0x100) = 0x108.
    inline constexpr std::uint32_t kMemSemantics_WorkgroupAcqRel = 0x108;

    // ============================================================================
    // Element type helpers — map Pravaha data_element_type → SPIR-V scalar type.
    // ============================================================================

    // Supported element types for Vulkan dispatch (MoltenVK-safe set).
    [[nodiscard]] constexpr bool is_vulkan_supported_type(
        compute::data_element_type t) noexcept {
        switch (t) {
        case compute::data_element_type::f32:
        case compute::data_element_type::f16:
        case compute::data_element_type::i32:
        case compute::data_element_type::u32:
            return true;
        default:
            return false;
        }
    }

    // Is this a float-class element type (uses FAdd, FMul, etc.)?
    [[nodiscard]] constexpr bool is_float_type(
        compute::data_element_type t) noexcept {
        return t == compute::data_element_type::f32 ||
            t == compute::data_element_type::f16;
    }

    // ============================================================================
    // Kernel types context — pre-built SPIR-V type + variable ids for a kernel.
    // These are shared between the expression emitter and the kernel builder.
    // ============================================================================

    struct KernelTypes {
        std::uint32_t type_void = 0;
        std::uint32_t type_bool = 0;
        std::uint32_t type_u32 = 0;
        std::uint32_t type_i32 = 0;
        std::uint32_t type_f32 = 0;
        std::uint32_t type_scalar = 0; // the compute element type (f32/i32/u32)
        std::uint32_t type_v3u32 = 0;
        std::uint32_t type_u32_ptr_in = 0;
        std::uint32_t type_fn_void = 0;
        std::uint32_t glsl_ext = 0; // OpExtInstImport result-id
        std::uint32_t const_0_u32 = 0; // OpConstant u32 0
        std::uint32_t const_1_u32 = 0; // OpConstant u32 1
        std::uint32_t const_2_u32 = 0; // OpConstant u32 2
        std::uint32_t var_gid = 0; // global_invocation_id variable id
        compute::data_element_type elem_type = compute::data_element_type::f32;
        bool is_float = true;
    };

    // ============================================================================
    // Expression evaluator — walks the Lithe AST and emits the SPIR-V expression
    // for one output lane. Returns the result-id of the scalar value.
    //
    // src_ids[k] = the loaded scalar value from source buffer k at the current gid.
    // lit_value(N) = function to get scalar for lit<N>.
    // ============================================================================

    template <typename E>
    std::uint32_t emit_spirv_expr(Builder& b,
                                  const E& expr,
                                  const KernelTypes& kt,
                                  const std::vector<std::uint32_t>& src_ids);

    // Helper for OpExtInst (GLSLstd450 unary).
    template <typename E>
    [[nodiscard]] std::uint32_t emit_glsl_unary(
        Builder& b, const KernelTypes& kt,
        const std::vector<std::uint32_t>& src_ids,
        const E& child_expr, std::uint32_t glsl_op) {
        const std::uint32_t child = emit_spirv_expr(b, child_expr, kt, src_ids);
        // Promote to f32 if needed (GLSLstd450 math ops on float only).
        const std::uint32_t fv = kt.is_float
                                     ? child
                                     : b.emit_r(kOpConvertSToF, kt.type_f32, child);
        const std::uint32_t res = b.emit_r(kOpExtInst, kt.type_f32,
                                           kt.glsl_ext, glsl_op, fv);
        // Demote back for integer types (truncate — not a common use case).
        return kt.is_float ? res : b.emit_r(kOpConvertFToS, kt.type_scalar, res);
    }

    template <typename E>
    std::uint32_t emit_spirv_expr(Builder& b,
                                  const E& expr,
                                  const KernelTypes& kt,
                                  const std::vector<std::uint32_t>& src_ids) {
        using node_t = std::decay_t<E>;
        using tag = typename node_t::tag_type;

        // ── Leaf: call_tag (single-input, slot 0) ──────────────────────────────
        if constexpr (std::is_same_v<tag, lithe::call_tag>) {
            return src_ids.empty() ? kt.const_0_u32 : src_ids[0];
        }
        // ── Leaf: input<N> (multi-input, slot N) ───────────────────────────────
        else if constexpr (pravaha::expr::input_tag_index<tag>::value) {
            constexpr std::size_t N = pravaha::expr::input_tag_index<tag>::index;
            return N < src_ids.size() ? src_ids[N] : kt.const_0_u32;
        }
        // ── Leaf: lit constant ─────────────────────────────────────────────────
        else if constexpr (std::is_same_v<tag, pravaha::expr::lit_tag>) {
            if constexpr (requires { expr.value; }) {
                if (kt.is_float) {
                    const float fv = static_cast<float>(expr.value);
                    std::uint32_t bits = 0;
                    std::memcpy(&bits, &fv, sizeof(bits));
                    return b.const_scalar(kt.type_scalar, bits);
                }
                else {
                    const std::int32_t iv = static_cast<std::int32_t>(expr.value);
                    return b.const_scalar(kt.type_scalar, static_cast<std::uint32_t>(iv));
                }
            }
            return kt.const_0_u32;
        }
        // ── Unary: neg ─────────────────────────────────────────────────────────
        else if constexpr (std::is_same_v<tag, lithe::neg_tag>) {
            const std::uint32_t child = emit_spirv_expr(
                b, std::get < 0 > (expr.children), kt, src_ids);
            if (kt.is_float)
                return b.emit_r(kOpFNegate, kt.type_scalar, child);
            else
                return b.emit_r(kOpSNegate, kt.type_scalar, child);
        }
        // ── Unary math (GLSLstd450) ────────────────────────────────────────────
        else if constexpr (std::is_same_v<tag, pravaha::expr::sqrt_tag>) {
            return emit_glsl_unary(b, kt, src_ids,
                                   std::get < 0 > (expr.children), kGLSL_Sqrt);
        }
        else if constexpr (std::is_same_v<tag, pravaha::expr::exp_tag>) {
            return emit_glsl_unary(b, kt, src_ids,
                                   std::get < 0 > (expr.children), kGLSL_Exp);
        }
        else if constexpr (std::is_same_v<tag, pravaha::expr::log_tag>) {
            return emit_glsl_unary(b, kt, src_ids,
                                   std::get < 0 > (expr.children), kGLSL_Log);
        }
        else if constexpr (std::is_same_v<tag, pravaha::expr::sin_tag>) {
            return emit_glsl_unary(b, kt, src_ids,
                                   std::get < 0 > (expr.children), kGLSL_Sin);
        }
        else if constexpr (std::is_same_v<tag, pravaha::expr::cos_tag>) {
            return emit_glsl_unary(b, kt, src_ids,
                                   std::get < 0 > (expr.children), kGLSL_Cos);
        }
        else if constexpr (std::is_same_v<tag, pravaha::expr::abs_tag>) {
            return emit_glsl_unary(b, kt, src_ids,
                                   std::get < 0 > (expr.children), kGLSL_FAbs);
        }
        // ── Binary arithmetic ─────────────────────────────────────────────────
        else {
            static_assert(lithe::emit::tag_descriptor<tag>::arity == 2,
                          "emit_spirv_expr: unhandled non-binary tag");
            const std::uint32_t lhs = emit_spirv_expr(
                b, std::get < 0 > (expr.children), kt, src_ids);
            const std::uint32_t rhs = emit_spirv_expr(
                b, std::get < 1 > (expr.children), kt, src_ids);

            // Compare first char only — all symbols are single ASCII characters.
            // Avoids ADL ambiguity with std::string_view::operator== on some libcxx.
            constexpr char sym = lithe::emit::tag_descriptor<tag>::symbol[0];

            if (kt.is_float) {
                if constexpr (sym == '+') return b.emit_r(kOpFAdd, kt.type_scalar, lhs, rhs);
                else if constexpr (sym == '-') return b.emit_r(kOpFSub, kt.type_scalar, lhs, rhs);
                else if constexpr (sym == '*') return b.emit_r(kOpFMul, kt.type_scalar, lhs, rhs);
                else if constexpr (sym == '/') return b.emit_r(kOpFDiv, kt.type_scalar, lhs, rhs);
                else return b.emit_r(kOpFAdd, kt.type_scalar, lhs, rhs); // fallback
            }
            else {
                if constexpr (sym == '+') return b.emit_r(kOpIAdd, kt.type_scalar, lhs, rhs);
                else if constexpr (sym == '-') return b.emit_r(kOpISub, kt.type_scalar, lhs, rhs);
                else if constexpr (sym == '*') return b.emit_r(kOpIMul, kt.type_scalar, lhs, rhs);
                else if constexpr (sym == '/') return b.emit_r(kOpSDiv, kt.type_scalar, lhs, rhs);
                else return b.emit_r(kOpIAdd, kt.type_scalar, lhs, rhs); // fallback
            }
        }
    }

    // ============================================================================
    // emit_kernel_spirv — shared kernel builder.
    //
    // Builds the SPIR-V header, type section, decorations, and main function.
    // expr_fn is called once inside main() to emit the expression body for slot[gid]
    // and expects src value ids already loaded.
    //
    // K = number of source buffer bindings.
    // local_x = workgroup size (default 256; matches kReduceTG).
    // ============================================================================

    template <std::size_t K, typename ExprFn>
    [[nodiscard]] lithe::codegen::backends::spirv_module
    build_spirv_kernel(compute::data_element_type elem,
                       std::uint32_t local_x,
                       ExprFn&& expr_fn) {
        const bool is_float = is_float_type(elem);

        Builder b;
        KernelTypes kt;
        kt.elem_type = elem;
        kt.is_float = is_float;

        // ── SPIR-V header ──────────────────────────────────────────────────────
        // magic, version(1.0), generator(0), bound(placeholder=0), schema(0)
        const std::size_t bound_index = 3; // index of "bound" word in header
        b.words.push_back(0x07230203u); // magic
        b.words.push_back(0x00010000u); // SPIR-V 1.0
        b.words.push_back(0u); // generator id (private)
        b.words.push_back(0u); // bound (patched below)
        b.words.push_back(0u); // schema

        // ── Capabilities ───────────────────────────────────────────────────────
        b.emit(kOpCapability, kCapability_Shader);

        // ── Extension (GLSLstd450) ─────────────────────────────────────────────
        kt.glsl_ext = b.fresh_id();
        // OpExtInstImport result_id "GLSL.std.450"
        {
            std::string_view ext_name = "GLSL.std.450";
            const std::size_t str_words = (ext_name.size() + 4) / 4;
            const std::uint32_t wc = static_cast<std::uint32_t>(1 + 1 + str_words);
            b.words.push_back((wc << 16) | kOpExtInstImport);
            b.words.push_back(kt.glsl_ext);
            // Write string
            std::size_t i = 0;
            while (i <= ext_name.size()) {
                std::uint32_t w = 0;
                for (int bl = 0; bl < 4 && i <= ext_name.size(); ++bl, ++i)
                    w |= static_cast<std::uint32_t>(i < ext_name.size() ? static_cast<unsigned char>(ext_name[i]) : 0u)
                        << (bl * 8);
                b.words.push_back(w);
            }
        }

        // ── Memory model ──────────────────────────────────────────────────────
        b.emit(kOpMemoryModel, kAddressingModel_Logical, kMemoryModel_GLSL450);

        // ── Forward-declare entry point id ───────────────────────────────────
        const std::uint32_t entry_id = b.fresh_id();

        // ── Type declarations ────────────────────────────────────────────────
        // OpTypeVoid: 2 words — no result_type operand, just result_id.
        kt.type_void = b.fresh_id();
        b.words.push_back((2u << 16) | kOpTypeVoid);
        b.words.push_back(kt.type_void);

        kt.type_bool = b.fresh_id();
        b.words.push_back((2u << 16) | kOpTypeBool);
        b.words.push_back(kt.type_bool);

        // u32
        kt.type_u32 = b.fresh_id();
        b.words.push_back((4u << 16) | kOpTypeInt);
        b.words.push_back(kt.type_u32);
        b.words.push_back(32u); // width
        b.words.push_back(0u); // signedness=0 (unsigned)

        // i32
        kt.type_i32 = b.fresh_id();
        b.words.push_back((4u << 16) | kOpTypeInt);
        b.words.push_back(kt.type_i32);
        b.words.push_back(32u);
        b.words.push_back(1u); // signedness=1

        // f32
        kt.type_f32 = b.fresh_id();
        b.words.push_back((3u << 16) | kOpTypeFloat);
        b.words.push_back(kt.type_f32);
        b.words.push_back(32u);

        // scalar type for this kernel
        if (elem == compute::data_element_type::f32 || elem == compute::data_element_type::f16)
            kt.type_scalar = kt.type_f32;
        else if (elem == compute::data_element_type::i32)
            kt.type_scalar = kt.type_i32;
        else
            kt.type_scalar = kt.type_u32;

        // vec3 u32 (for GlobalInvocationId)
        kt.type_v3u32 = b.fresh_id();
        b.words.push_back((4u << 16) | kOpTypeVector);
        b.words.push_back(kt.type_v3u32);
        b.words.push_back(kt.type_u32);
        b.words.push_back(3u);

        // u32 constants (index loads)
        kt.const_0_u32 = b.fresh_id();
        b.words.push_back((4u << 16) | 43u); // OpConstant
        b.words.push_back(kt.type_u32);
        b.words.push_back(kt.const_0_u32);
        b.words.push_back(0u);

        kt.const_1_u32 = b.fresh_id();
        b.words.push_back((4u << 16) | 43u);
        b.words.push_back(kt.type_u32);
        b.words.push_back(kt.const_1_u32);
        b.words.push_back(1u);

        kt.const_2_u32 = b.fresh_id();
        b.words.push_back((4u << 16) | 43u);
        b.words.push_back(kt.type_u32);
        b.words.push_back(kt.const_2_u32);
        b.words.push_back(2u);

        // Runtime array of scalar (for each buffer).
        const std::uint32_t type_rt_arr = b.fresh_id();
        b.words.push_back((3u << 16) | kOpTypeRuntimeArray);
        b.words.push_back(type_rt_arr);
        b.words.push_back(kt.type_scalar);

        // Struct wrapping the runtime array (storage buffer interface).
        const std::uint32_t type_buf_struct = b.fresh_id();
        b.words.push_back((3u << 16) | kOpTypeStruct);
        b.words.push_back(type_buf_struct);
        b.words.push_back(type_rt_arr);

        // Pointer to buf_struct (StorageBuffer storage class).
        const std::uint32_t type_ptr_buf_sb = b.fresh_id();
        b.words.push_back((4u << 16) | kOpTypePointer);
        b.words.push_back(type_ptr_buf_sb);
        b.words.push_back(kStorageClass_StorageBuffer);
        b.words.push_back(type_buf_struct);

        // ── Decorations ───────────────────────────────────────────────────────
        // ArrayStride on the runtime array (stride = element size in bytes).
        const std::uint32_t byte_stride = static_cast<std::uint32_t>(
            compute::element_size(elem));
        b.emit(kOpDecorate, type_rt_arr, kDecoration_ArrayStride, byte_stride);

        // Block decoration on the struct.
        b.emit(kOpDecorate, type_buf_struct, kDecoration_Block);

        // MemberDecorate: member 0 offset 0.
        b.emit(kOpMemberDecorate, type_buf_struct, 0u, 0u /*Offset*/, 0u);

        // Declare K source buffer variables + 1 dst buffer variable.
        std::vector<std::uint32_t> src_var_ids(K);
        for (std::size_t s = 0; s < K; ++s) {
            src_var_ids[s] = b.fresh_id();
            b.words.push_back((4u << 16) | kOpVariable);
            b.words.push_back(type_ptr_buf_sb);
            b.words.push_back(src_var_ids[s]);
            b.words.push_back(kStorageClass_StorageBuffer);
            // DescriptorSet=0, Binding=s, NonWritable (read-only source)
            b.emit(kOpDecorate, src_var_ids[s], kDecoration_DescriptorSet, 0u);
            b.emit(kOpDecorate, src_var_ids[s], kDecoration_Binding, static_cast<std::uint32_t>(s));
            b.emit(kOpDecorate, src_var_ids[s], kDecoration_NonWritable);
        }
        const std::uint32_t dst_var_id = b.fresh_id();
        b.words.push_back((4u << 16) | kOpVariable);
        b.words.push_back(type_ptr_buf_sb);
        b.words.push_back(dst_var_id);
        b.words.push_back(kStorageClass_StorageBuffer);
        b.emit(kOpDecorate, dst_var_id, kDecoration_DescriptorSet, 0u);
        b.emit(kOpDecorate, dst_var_id, kDecoration_Binding, static_cast<std::uint32_t>(K));

        // GlobalInvocationId builtin variable.
        const std::uint32_t type_ptr_v3u32_in = b.fresh_id();
        b.words.push_back((4u << 16) | kOpTypePointer);
        b.words.push_back(type_ptr_v3u32_in);
        b.words.push_back(kStorageClass_Input);
        b.words.push_back(kt.type_v3u32);

        kt.var_gid = b.fresh_id();
        b.words.push_back((4u << 16) | kOpVariable);
        b.words.push_back(type_ptr_v3u32_in);
        b.words.push_back(kt.var_gid);
        b.words.push_back(kStorageClass_Input);
        b.emit(kOpDecorate, kt.var_gid, kDecoration_Builtin, kBuiltin_GlobalInvocationId);

        // u32 pointer (for AccessChain into runtime array).
        const std::uint32_t type_ptr_scalar_sb = b.fresh_id();
        b.words.push_back((4u << 16) | kOpTypePointer);
        b.words.push_back(type_ptr_scalar_sb);
        b.words.push_back(kStorageClass_StorageBuffer);
        b.words.push_back(kt.type_scalar);

        // Function type: void()
        kt.type_fn_void = b.fresh_id();
        b.words.push_back((3u << 16) | kOpTypeFunction);
        b.words.push_back(kt.type_fn_void);
        b.words.push_back(kt.type_void);

        // ── EntryPoint ────────────────────────────────────────────────────────
        // Collect all interface variable ids.
        std::vector<std::uint32_t> interface_vars;
        for (std::size_t s = 0; s < K; ++s) interface_vars.push_back(src_var_ids[s]);
        interface_vars.push_back(dst_var_id);
        interface_vars.push_back(kt.var_gid);

        {
            std::string_view ep_name = "main";
            const std::size_t str_words = (ep_name.size() + 4) / 4;
            const std::uint32_t wc = static_cast<std::uint32_t>(
                1 + 1 + 1 + str_words + interface_vars.size());
            b.words.push_back((wc << 16) | kOpEntryPoint);
            b.words.push_back(kExecutionModel_GLCompute);
            b.words.push_back(entry_id);
            // String "main" (5 bytes including null → 2 words)
            b.words.push_back(0x6e69616du); // "main" LE
            b.words.push_back(0x00000000u); // null padding
            for (auto iv : interface_vars) b.words.push_back(iv);
        }

        // ExecutionMode LocalSize.
        b.emit(kOpExecutionMode, entry_id, kExecutionMode_LocalSize,
               local_x, 1u, 1u);

        // ── Function body ─────────────────────────────────────────────────────
        // entry function definition. Record the index so any constants interned
        // during body emission (lit leaves) can be spliced back into the global
        // section before this OpFunction — SPIR-V forbids OpConstant in a function.
        const std::size_t fn_start = b.words.size();
        b.words.push_back((5u << 16) | kOpFunction);
        b.words.push_back(kt.type_void);
        b.words.push_back(entry_id);
        b.words.push_back(kFunctionControl_None);
        b.words.push_back(kt.type_fn_void);

        const std::uint32_t label_id = b.fresh_id();
        b.words.push_back((2u << 16) | kOpLabel);
        b.words.push_back(label_id);

        // Load gid.x (u32).
        const std::uint32_t gid_v3 = b.emit_r(kOpLoad, kt.type_v3u32, kt.var_gid);
        const std::uint32_t gid_x = b.emit_r(kOpCompositeExtract, kt.type_u32, gid_v3, 0u);

        // Load source values src[gid.x] for each input buffer.
        std::vector<std::uint32_t> src_val_ids(K);
        for (std::size_t s = 0; s < K; ++s) {
            const std::uint32_t ptr = b.emit_r(kOpAccessChain, type_ptr_scalar_sb,
                                               src_var_ids[s], kt.const_0_u32, gid_x);
            src_val_ids[s] = b.emit_r(kOpLoad, kt.type_scalar, ptr);
        }

        // Delegate to caller-supplied expression or reduction body.
        const std::uint32_t result_val = expr_fn(b, kt, src_val_ids, gid_x);

        // dst[gid.x] = result_val
        const std::uint32_t dst_ptr = b.emit_r(kOpAccessChain, type_ptr_scalar_sb,
                                               dst_var_id, kt.const_0_u32, gid_x);
        b.words.push_back((3u << 16) | kOpStore);
        b.words.push_back(dst_ptr);
        b.words.push_back(result_val);

        b.words.push_back((1u << 16) | kOpReturn);
        b.words.push_back((1u << 16) | kOpFunctionEnd);

        // Splice any deferred constants (lit leaves) into the global section, then
        // patch the 'bound' field in the header (bound_index is before fn_start, so
        // the splice does not shift it).
        b.splice_constants(fn_start);
        b.words[bound_index] = b.bound;

        lithe::codegen::backends::spirv_module mod;
        mod.words = std::move(b.words);
        mod.local_x = local_x;
        mod.local_y = 1;
        mod.local_z = 1;
        return mod;
    }

    // ============================================================================
    // emit_kernel — element-wise: dst[i] = expr(src_0[i], src_1[i], ...)
    // ============================================================================

    template <typename E>
    [[nodiscard]] lithe::codegen::backends::spirv_module
    emit_kernel(const E& expr,
                compute::data_element_type elem,
                std::uint32_t local_x = 256) {
        constexpr std::size_t K =
            pravaha::backends::simd_detail::input_slot_count<E>() == 0
                ? 1
                : pravaha::backends::simd_detail::input_slot_count<E>();

        return build_spirv_kernel<K>(elem, local_x,
                                     [&](Builder& b, const KernelTypes& kt,
                                         const std::vector<std::uint32_t>& src_val_ids,
                                         std::uint32_t /*gid_x*/) -> std::uint32_t {
                                         return emit_spirv_expr(b, expr, kt, src_val_ids);
                                     });
    }

    // ============================================================================
    // emit_reduce_kernel — reduction: result = reduce_op(expr(src_N[i]))
    //
    // Strategy: each workgroup lane computes one element of the expression, then
    // does a sequential reduction across the local workgroup in shared memory.
    // This is the same two-stage approach as the Metal reduce kernel: local
    // accumulator → threadgroup barrier → workgroup partial → host folds partials.
    //
    // NOTE: SPIR-V shared-memory (Workgroup storage) requires Capability Shader +
    // Capability VulkanMemoryModel or explicit memory-barrier instructions. For
    // simplicity and MoltenVK compatibility we use a flat sequential reduction
    // via atomic load/store (no Workgroup memory). Each workgroup only has 1
    // thread active (local_x=1) and writes one partial result. The host then
    // folds N_groups partials in the dst buffer. This avoids needing
    // VulkanMemoryModel or OpControlBarrier.
    //
    // For the Pravaha element-wise/reduction surface this is acceptable: the
    // heavy lifting is the single-thread partial accumulation, identical to the
    // CPU scalar reduce path (which is also sequential within a lane).
    // ============================================================================

    template <typename E>
    [[nodiscard]] lithe::codegen::backends::spirv_module
    emit_reduce_kernel(const E& child_expr,
                       pravaha::expr::reduce_op /*op*/,
                       compute::data_element_type elem,
                       std::uint32_t tg = 1) {
        // K = input slot count for the child expression.
        constexpr std::size_t K =
            pravaha::backends::simd_detail::input_slot_count<E>() == 0
                ? 1
                : pravaha::backends::simd_detail::input_slot_count<E>();

        // One thread per workgroup. Each thread reduces a contiguous segment.
        // dst[workgroup_id] = partial result. Host folds these partials.
        return build_spirv_kernel<K>(elem, tg,
                                     [&](Builder& b, const KernelTypes& kt,
                                         const std::vector<std::uint32_t>& src_val_ids,
                                         std::uint32_t gid_x) -> std::uint32_t {
                                         // For reductions, we emit a trivially valid kernel that writes the
                                         // per-element expression result. The host-side dispatch will do
                                         // segment-based reduction via a two-pass approach:
                                         //  Pass 1: emit element-wise kernel (expr(src[i])) → tmp buf
                                         //  Pass 2: CPU fold of tmp buf
                                         // This keeps the SPIR-V valid and simple (no Workgroup memory ops).
                                         (void)gid_x;
                                         return emit_spirv_expr(b, child_expr, kt, src_val_ids);
                                     });
    }

    // ============================================================================
    // emit_reduce_tree_kernel — TRUE GPU reduction tree (ARCHITECTURE_REVIEW item 2).
    //
    // Replaces the two-pass "element-wise → CPU fold" strategy with a real
    // workgroup-local parallel reduction in Workgroup (shared) memory:
    //
    //   scratch[lid] = expr(src_k[gid])                 // each lane's contribution
    //   barrier
    //   for (stride = TG/2; stride >= 1; stride /= 2)   // unrolled tree fold
    //       if (lid < stride)
    //           scratch[lid] = op(scratch[lid], scratch[lid+stride]);
    //       barrier                                     // uniform control flow
    //   if (lid == 0) dst[wgid] = scratch[0];           // one partial per workgroup
    //
    // The host then folds the ceil(N/TG) partials (see execute_reduction) instead
    // of N values — the GPU does the O(N) work, the CPU does O(N/TG).
    //
    // Uses ONLY the Shader capability (Workgroup storage + OpControlBarrier are core
    // Shader features), so Lithe's spirv_module::validate() accepts it unchanged.
    //
    // Preconditions handled by the caller (execute_reduction):
    //   - TG is a power of two (tree fold assumes it).
    //   - The source staging buffer is PADDED to a multiple of TG with the op
    //     identity (0 for sum; the first element for max/min), so no in-shader
    //     bounds branch is needed and out-of-range lanes contribute neutrally.
    // ============================================================================

    template <std::size_t K, typename E>
    [[nodiscard]] lithe::codegen::backends::spirv_module
    build_reduce_tree_kernel(const E& child_expr,
                             compute::data_element_type elem,
                             pravaha::expr::reduce_op op,
                             std::uint32_t tg) {
        const bool is_float = is_float_type(elem);

        Builder b;
        KernelTypes kt;
        kt.elem_type = elem;
        kt.is_float = is_float;

        // ── Header ──────────────────────────────────────────────────────────────
        const std::size_t bound_index = 3;
        b.words.push_back(0x07230203u); // magic
        b.words.push_back(0x00010000u); // SPIR-V 1.0
        b.words.push_back(0u); // generator
        b.words.push_back(0u); // bound (patched)
        b.words.push_back(0u); // schema

        b.emit(kOpCapability, kCapability_Shader);

        // GLSLstd450 import (SMax/SMin/etc. for integer max/min).
        kt.glsl_ext = b.fresh_id();
        {
            std::string_view ext_name = "GLSL.std.450";
            const std::size_t str_words = (ext_name.size() + 4) / 4;
            const std::uint32_t wc = static_cast<std::uint32_t>(1 + 1 + str_words);
            b.words.push_back((wc << 16) | kOpExtInstImport);
            b.words.push_back(kt.glsl_ext);
            std::size_t i = 0;
            while (i <= ext_name.size()) {
                std::uint32_t w = 0;
                for (int bl = 0; bl < 4 && i <= ext_name.size(); ++bl, ++i)
                    w |= static_cast<std::uint32_t>(i < ext_name.size()
                                                        ? static_cast<unsigned char>(ext_name[i])
                                                        : 0u) << (bl * 8);
                b.words.push_back(w);
            }
        }

        b.emit(kOpMemoryModel, kAddressingModel_Logical, kMemoryModel_GLSL450);

        const std::uint32_t entry_id = b.fresh_id();

        // ── Types ────────────────────────────────────────────────────────────────
        kt.type_void = b.fresh_id();
        b.words.push_back((2u << 16) | kOpTypeVoid);
        b.words.push_back(kt.type_void);

        kt.type_bool = b.fresh_id();
        b.words.push_back((2u << 16) | kOpTypeBool);
        b.words.push_back(kt.type_bool);

        kt.type_u32 = b.fresh_id();
        b.words.push_back((4u << 16) | kOpTypeInt);
        b.words.push_back(kt.type_u32);
        b.words.push_back(32u);
        b.words.push_back(0u);

        kt.type_i32 = b.fresh_id();
        b.words.push_back((4u << 16) | kOpTypeInt);
        b.words.push_back(kt.type_i32);
        b.words.push_back(32u);
        b.words.push_back(1u);

        kt.type_f32 = b.fresh_id();
        b.words.push_back((3u << 16) | kOpTypeFloat);
        b.words.push_back(kt.type_f32);
        b.words.push_back(32u);

        if (elem == compute::data_element_type::f32 || elem == compute::data_element_type::f16)
            kt.type_scalar = kt.type_f32;
        else if (elem == compute::data_element_type::i32)
            kt.type_scalar = kt.type_i32;
        else
            kt.type_scalar = kt.type_u32;

        kt.type_v3u32 = b.fresh_id();
        b.words.push_back((4u << 16) | kOpTypeVector);
        b.words.push_back(kt.type_v3u32);
        b.words.push_back(kt.type_u32);
        b.words.push_back(3u);

        // u32 constants. Emitted inline (the workgroup array type below references
        // const_tg, which must precede that type declaration). Registered in the
        // builder's constant pool so deferred stride constants dedupe against them.
        auto const_u32 = [&](std::uint32_t v) {
            const std::uint32_t id = b.fresh_id();
            b.words.push_back((4u << 16) | 43u); // OpConstant
            b.words.push_back(kt.type_u32);
            b.words.push_back(id);
            b.words.push_back(v);
            b.register_const(kt.type_u32, v, id);
            return id;
        };
        kt.const_0_u32 = const_u32(0u);
        kt.const_1_u32 = const_u32(1u);
        kt.const_2_u32 = const_u32(2u);
        const std::uint32_t const_tg = const_u32(tg);

        // OpControlBarrier operands (Execution scope, Memory scope, Memory semantics)
        // are <id> references to u32 OpConstant values — NOT immediate literals.
        // Passing literals makes SPIRV-Cross cast a literal as a constant id → the
        // MoltenVK "SPIR-V to MSL conversion error: Bad cast". const_2_u32 already
        // encodes the Workgroup scope (2); add the memory-semantics constant.
        const std::uint32_t const_wg_scope = kt.const_2_u32; // Workgroup = 2
        const std::uint32_t const_mem_sem = const_u32(kMemSemantics_WorkgroupAcqRel);

        // Runtime array + storage-buffer struct + pointers (same layout as elementwise).
        const std::uint32_t byte_stride = static_cast<std::uint32_t>(compute::element_size(elem));
        const std::uint32_t type_rt_arr = b.fresh_id();
        b.words.push_back((3u << 16) | kOpTypeRuntimeArray);
        b.words.push_back(type_rt_arr);
        b.words.push_back(kt.type_scalar);

        const std::uint32_t type_buf_struct = b.fresh_id();
        b.words.push_back((3u << 16) | kOpTypeStruct);
        b.words.push_back(type_buf_struct);
        b.words.push_back(type_rt_arr);

        const std::uint32_t type_ptr_buf_sb = b.fresh_id();
        b.words.push_back((4u << 16) | kOpTypePointer);
        b.words.push_back(type_ptr_buf_sb);
        b.words.push_back(kStorageClass_StorageBuffer);
        b.words.push_back(type_buf_struct);

        b.emit(kOpDecorate, type_rt_arr, kDecoration_ArrayStride, byte_stride);
        b.emit(kOpDecorate, type_buf_struct, kDecoration_Block);
        b.emit(kOpMemberDecorate, type_buf_struct, 0u, 0u /*Offset*/, 0u);

        // Source buffers (read-only) + dst buffer.
        std::array<std::uint32_t, K> src_var_ids{};
        for (std::size_t s = 0; s < K; ++s) {
            src_var_ids[s] = b.fresh_id();
            b.words.push_back((4u << 16) | kOpVariable);
            b.words.push_back(type_ptr_buf_sb);
            b.words.push_back(src_var_ids[s]);
            b.words.push_back(kStorageClass_StorageBuffer);
            b.emit(kOpDecorate, src_var_ids[s], kDecoration_DescriptorSet, 0u);
            b.emit(kOpDecorate, src_var_ids[s], kDecoration_Binding, static_cast<std::uint32_t>(s));
            b.emit(kOpDecorate, src_var_ids[s], kDecoration_NonWritable);
        }
        const std::uint32_t dst_var_id = b.fresh_id();
        b.words.push_back((4u << 16) | kOpVariable);
        b.words.push_back(type_ptr_buf_sb);
        b.words.push_back(dst_var_id);
        b.words.push_back(kStorageClass_StorageBuffer);
        b.emit(kOpDecorate, dst_var_id, kDecoration_DescriptorSet, 0u);
        b.emit(kOpDecorate, dst_var_id, kDecoration_Binding, static_cast<std::uint32_t>(K));

        // Workgroup shared array: scratch[tg] of scalar.
        const std::uint32_t type_wg_arr = b.fresh_id();
        b.words.push_back((4u << 16) | kOpTypeArray);
        b.words.push_back(type_wg_arr);
        b.words.push_back(kt.type_scalar);
        b.words.push_back(const_tg);

        const std::uint32_t type_ptr_wg_arr = b.fresh_id();
        b.words.push_back((4u << 16) | kOpTypePointer);
        b.words.push_back(type_ptr_wg_arr);
        b.words.push_back(kStorageClass_Workgroup);
        b.words.push_back(type_wg_arr);

        const std::uint32_t scratch_var = b.fresh_id();
        b.words.push_back((4u << 16) | kOpVariable);
        b.words.push_back(type_ptr_wg_arr);
        b.words.push_back(scratch_var);
        b.words.push_back(kStorageClass_Workgroup);

        const std::uint32_t type_ptr_scalar_wg = b.fresh_id();
        b.words.push_back((4u << 16) | kOpTypePointer);
        b.words.push_back(type_ptr_scalar_wg);
        b.words.push_back(kStorageClass_Workgroup);
        b.words.push_back(kt.type_scalar);

        // Builtin input vars: GlobalInvocationId, LocalInvocationId, WorkgroupId.
        const std::uint32_t type_ptr_v3u32_in = b.fresh_id();
        b.words.push_back((4u << 16) | kOpTypePointer);
        b.words.push_back(type_ptr_v3u32_in);
        b.words.push_back(kStorageClass_Input);
        b.words.push_back(kt.type_v3u32);

        auto builtin_var = [&](std::uint32_t builtin) {
            const std::uint32_t v = b.fresh_id();
            b.words.push_back((4u << 16) | kOpVariable);
            b.words.push_back(type_ptr_v3u32_in);
            b.words.push_back(v);
            b.words.push_back(kStorageClass_Input);
            b.emit(kOpDecorate, v, kDecoration_Builtin, builtin);
            return v;
        };
        const std::uint32_t var_gid = builtin_var(kBuiltin_GlobalInvocationId);
        const std::uint32_t var_lid = builtin_var(kBuiltin_LocalInvocationId);
        const std::uint32_t var_wgid = builtin_var(kBuiltin_WorkgroupId);

        // Storage-buffer scalar pointer (AccessChain into runtime arrays).
        const std::uint32_t type_ptr_scalar_sb = b.fresh_id();
        b.words.push_back((4u << 16) | kOpTypePointer);
        b.words.push_back(type_ptr_scalar_sb);
        b.words.push_back(kStorageClass_StorageBuffer);
        b.words.push_back(kt.type_scalar);

        kt.type_fn_void = b.fresh_id();
        b.words.push_back((3u << 16) | kOpTypeFunction);
        b.words.push_back(kt.type_fn_void);
        b.words.push_back(kt.type_void);

        // ── EntryPoint ────────────────────────────────────────────────────────
        // SPIR-V 1.0–1.3: OpEntryPoint interface list must only contain Input/Output
        // storage-class variables.  Workgroup (scratch_var) and StorageBuffer vars
        // must NOT be listed; MoltenVK SPIRV-Cross raises "Bad cast" otherwise.
        std::vector<std::uint32_t> interface_vars;
        interface_vars.push_back(var_gid);
        interface_vars.push_back(var_lid);
        interface_vars.push_back(var_wgid);
        {
            std::string_view ep = "main";
            const std::size_t str_words = (ep.size() + 4) / 4;
            const std::uint32_t wc = static_cast<std::uint32_t>(
                1 + 1 + 1 + str_words + interface_vars.size());
            b.words.push_back((wc << 16) | kOpEntryPoint);
            b.words.push_back(kExecutionModel_GLCompute);
            b.words.push_back(entry_id);
            b.words.push_back(0x6e69616du); // "main"
            b.words.push_back(0x00000000u);
            for (auto iv : interface_vars) b.words.push_back(iv);
        }
        b.emit(kOpExecutionMode, entry_id, kExecutionMode_LocalSize, tg, 1u, 1u);

        // ── Function ─────────────────────────────────────────────────────────────
        // Record the OpFunction index: stride constants interned during the tree fold
        // are spliced into the global section here (SPIR-V forbids OpConstant in a
        // function body).
        const std::size_t fn_start = b.words.size();
        b.words.push_back((5u << 16) | kOpFunction);
        b.words.push_back(kt.type_void);
        b.words.push_back(entry_id);
        b.words.push_back(kFunctionControl_None);
        b.words.push_back(kt.type_fn_void);

        const std::uint32_t entry_label = b.fresh_id();
        b.words.push_back((2u << 16) | kOpLabel);
        b.words.push_back(entry_label);

        // gid.x / lid.x / wgid.x
        const std::uint32_t gid_v3 = b.emit_r(kOpLoad, kt.type_v3u32, var_gid);
        const std::uint32_t gid_x = b.emit_r(kOpCompositeExtract, kt.type_u32, gid_v3, 0u);
        const std::uint32_t lid_v3 = b.emit_r(kOpLoad, kt.type_v3u32, var_lid);
        const std::uint32_t lid_x = b.emit_r(kOpCompositeExtract, kt.type_u32, lid_v3, 0u);
        const std::uint32_t wg_v3 = b.emit_r(kOpLoad, kt.type_v3u32, var_wgid);
        const std::uint32_t wg_x = b.emit_r(kOpCompositeExtract, kt.type_u32, wg_v3, 0u);

        // src_k[gid] → expr contribution.
        std::vector<std::uint32_t> src_vals(K);
        for (std::size_t s = 0; s < K; ++s) {
            const std::uint32_t ptr = b.emit_r(kOpAccessChain, type_ptr_scalar_sb,
                                               src_var_ids[s], kt.const_0_u32, gid_x);
            src_vals[s] = b.emit_r(kOpLoad, kt.type_scalar, ptr);
        }

        auto barrier = [&] {
            b.emit(kOpControlBarrier, const_wg_scope, const_wg_scope, const_mem_sem);
        };
        auto scratch_ptr = [&](std::uint32_t idx) {
            return b.emit_r(kOpAccessChain, type_ptr_scalar_wg, scratch_var, idx);
        };
        auto combine = [&](std::uint32_t a, std::uint32_t c) -> std::uint32_t {
            if (op == pravaha::expr::reduce_op::sum) {
                return is_float
                           ? b.emit_r(kOpFAdd, kt.type_scalar, a, c)
                           : b.emit_r(kOpIAdd, kt.type_scalar, a, c);
            }
            std::uint32_t glsl_op = 0;
            if (op == pravaha::expr::reduce_op::max)
                glsl_op = is_float
                              ? kGLSL_FMax
                              : (elem == compute::data_element_type::i32 ? kGLSL_SMax : kGLSL_UMax);
            else
                glsl_op = is_float
                              ? kGLSL_FMin
                              : (elem == compute::data_element_type::i32 ? kGLSL_SMin : kGLSL_UMin);
            return b.emit_r(kOpExtInst, kt.type_scalar, kt.glsl_ext, glsl_op, a, c);
        };

        // scratch[lid] = expr(src_0[gid], src_1[gid], ...) — the full child
        // expression, evaluated lane-wise exactly as the element-wise kernel does,
        // so sum/max/min fold the transformed values, not the raw loads.
        const std::uint32_t contribution =
            emit_spirv_expr(b, child_expr, kt, src_vals);
        {
            const std::uint32_t p = scratch_ptr(lid_x);
            b.words.push_back((3u << 16) | kOpStore);
            b.words.push_back(p);
            b.words.push_back(contribution);
        }
        barrier();

        // Unrolled tree fold: stride = tg/2, tg/4, ... 1.
        for (std::uint32_t stride = tg / 2; stride >= 1; stride /= 2) {
            // Deferred: OpConstant cannot appear inside the function body; intern it
            // into the global constant section (spliced in before OpFunction below).
            const std::uint32_t c_stride = b.const_scalar(kt.type_u32, stride);
            const std::uint32_t cond = b.emit_r(kOpULessThan, kt.type_bool, lid_x, c_stride);

            const std::uint32_t then_label = b.fresh_id();
            const std::uint32_t merge_label = b.fresh_id();

            b.emit(kOpSelectionMerge, merge_label, kSelectionControl_None);
            b.emit(kOpBranchConditional, cond, then_label, merge_label);

            // then: scratch[lid] = combine(scratch[lid], scratch[lid+stride])
            b.words.push_back((2u << 16) | kOpLabel);
            b.words.push_back(then_label);
            const std::uint32_t idx_hi = b.emit_r(kOpIAdd, kt.type_u32, lid_x, c_stride);
            const std::uint32_t p_lo = scratch_ptr(lid_x);
            const std::uint32_t v_lo = b.emit_r(kOpLoad, kt.type_scalar, p_lo);
            const std::uint32_t p_hi = scratch_ptr(idx_hi);
            const std::uint32_t v_hi = b.emit_r(kOpLoad, kt.type_scalar, p_hi);
            const std::uint32_t merged = combine(v_lo, v_hi);
            const std::uint32_t p_lo2 = scratch_ptr(lid_x);
            b.words.push_back((3u << 16) | kOpStore);
            b.words.push_back(p_lo2);
            b.words.push_back(merged);
            b.emit(kOpBranch, merge_label);

            // merge: all lanes converge here, then barrier in uniform control flow.
            b.words.push_back((2u << 16) | kOpLabel);
            b.words.push_back(merge_label);
            barrier();

            if (stride == 1) break; // avoid unsigned wrap on stride /= 2
        }

        // if (lid == 0) dst[wgid] = scratch[0];
        {
            const std::uint32_t is_zero = b.emit_r(kOpIEqual, kt.type_bool, lid_x, kt.const_0_u32);
            const std::uint32_t then_label = b.fresh_id();
            const std::uint32_t merge_label = b.fresh_id();
            b.emit(kOpSelectionMerge, merge_label, kSelectionControl_None);
            b.emit(kOpBranchConditional, is_zero, then_label, merge_label);

            b.words.push_back((2u << 16) | kOpLabel);
            b.words.push_back(then_label);
            const std::uint32_t sp0 = scratch_ptr(kt.const_0_u32);
            const std::uint32_t v0 = b.emit_r(kOpLoad, kt.type_scalar, sp0);
            const std::uint32_t dptr = b.emit_r(kOpAccessChain, type_ptr_scalar_sb,
                                                dst_var_id, kt.const_0_u32, wg_x);
            b.words.push_back((3u << 16) | kOpStore);
            b.words.push_back(dptr);
            b.words.push_back(v0);
            b.emit(kOpBranch, merge_label);

            b.words.push_back((2u << 16) | kOpLabel);
            b.words.push_back(merge_label);
        }

        b.words.push_back((1u << 16) | kOpReturn);
        b.words.push_back((1u << 16) | kOpFunctionEnd);

        // Splice deferred stride constants into the global section before OpFunction
        // (bound_index precedes fn_start, so it is not shifted), then patch bound.
        b.splice_constants(fn_start);
        b.words[bound_index] = b.bound;

        lithe::codegen::backends::spirv_module mod;
        mod.words = std::move(b.words);
        mod.local_x = tg;
        mod.local_y = 1;
        mod.local_z = 1;
        return mod;
    }

    // emit_reduce_tree — infer K from the child expression's input slot count.
    template <typename E>
    [[nodiscard]] lithe::codegen::backends::spirv_module
    emit_reduce_tree(const E& child_expr,
                     pravaha::expr::reduce_op op,
                     compute::data_element_type elem,
                     std::uint32_t tg = 256) {
        constexpr std::size_t K =
            pravaha::backends::simd_detail::input_slot_count<E>() == 0
                ? 1
                : pravaha::backends::simd_detail::input_slot_count<E>();
        return build_reduce_tree_kernel<K>(child_expr, elem, op, tg);
    }
} // namespace pravaha::backends::vulkan::spirv

// ============================================================================
// Section 2–4: Device dispatch shim + cache + VulkanGpuBackend adapter.
// Guarded by LITHE_VULKAN_BACKEND_AVAILABLE.
// ============================================================================

#include "edsl/backends/lithe_codegen_vulkan.hpp"  // defines LITHE_VULKAN_BACKEND_AVAILABLE

#if LITHE_VULKAN_BACKEND_AVAILABLE

#include <cstdlib>
#include <bit>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>

namespace pravaha::backends::vulkan {
    // ============================================================================
    // Section 2: Injectable process-wide device provider + buffer-binding shim.
    //
    // Lithe's vulkan_backend owns: VkContext, pipeline, fence registry.
    // Pravaha owns: VkBuffer alloc (src+dst), vkUpdateDescriptorSets, dispatch.
    //
    // The device is owned by a lazily-constructed, thread-safe, injectable
    // device_provider (defined after the buffer helpers, so it can also own the
    // staging_pool). device() is the thin accessor used by every dispatch path.
    // ============================================================================

    [[nodiscard]] lithe::codegen::backends::vulkan_backend& device() noexcept;

    // Find a compatible memory type index for the given property flags.
    [[nodiscard]] inline std::optional<std::uint32_t>
    find_memory_type(VkPhysicalDevice phys, VkMemoryPropertyFlags flags) noexcept {
        VkPhysicalDeviceMemoryProperties mp{};
        vkGetPhysicalDeviceMemoryProperties(phys, &mp);
        for (std::uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
            if ((mp.memoryTypes[i].propertyFlags & flags) == flags)
                return i;
        }
        return std::nullopt;
    }

    // RAII Vulkan buffer (device memory + VkBuffer pair).
    struct VkBufferAlloc {
        VkDevice dev = VK_NULL_HANDLE;
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        std::size_t size = 0;

        VkBufferAlloc() = default;
        VkBufferAlloc(const VkBufferAlloc&) = delete;
        VkBufferAlloc& operator=(const VkBufferAlloc&) = delete;

        VkBufferAlloc(VkBufferAlloc&& o) noexcept
            : dev(o.dev), buffer(o.buffer), memory(o.memory), size(o.size) {
            o.dev = VK_NULL_HANDLE;
            o.buffer = VK_NULL_HANDLE;
            o.memory = VK_NULL_HANDLE;
            o.size = 0;
        }

        VkBufferAlloc& operator=(VkBufferAlloc&& o) noexcept {
            if (this != &o) {
                destroy();
                dev = o.dev;
                buffer = o.buffer;
                memory = o.memory;
                size = o.size;
                o.dev = VK_NULL_HANDLE;
                o.buffer = VK_NULL_HANDLE;
                o.memory = VK_NULL_HANDLE;
                o.size = 0;
            }
            return *this;
        }

        ~VkBufferAlloc() { destroy(); }

        [[nodiscard]] bool valid() const noexcept {
            return buffer != VK_NULL_HANDLE && memory != VK_NULL_HANDLE;
        }

        void* map() const noexcept {
            void* ptr = nullptr;
            vkMapMemory(dev, memory, 0, size, 0, &ptr);
            return ptr;
        }

        void unmap() const noexcept { vkUnmapMemory(dev, memory); }

    private:
        void destroy() noexcept {
            if (dev == VK_NULL_HANDLE) return;
            if (buffer != VK_NULL_HANDLE) vkDestroyBuffer(dev, buffer, nullptr);
            if (memory != VK_NULL_HANDLE) vkFreeMemory(dev, memory, nullptr);
            buffer = VK_NULL_HANDLE;
            memory = VK_NULL_HANDLE;
        }
    };

    // Allocate a host-visible+coherent VkBuffer (for staging path).
    [[nodiscard]] inline std::optional<VkBufferAlloc>
    alloc_vk_buffer(VkDevice dev, VkPhysicalDevice phys,
                    std::size_t bytes, VkBufferUsageFlags usage) noexcept {
        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = bytes;
        bci.usage = usage;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VkBufferAlloc alloc;
        alloc.dev = dev;
        alloc.size = bytes;
        if (vkCreateBuffer(dev, &bci, nullptr, &alloc.buffer) != VK_SUCCESS)
            return std::nullopt;

        VkMemoryRequirements mr{};
        vkGetBufferMemoryRequirements(dev, alloc.buffer, &mr);

        const VkMemoryPropertyFlags flags =
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        auto mt = find_memory_type(phys, flags);
        if (!mt) {
            vkDestroyBuffer(dev, alloc.buffer, nullptr);
            alloc.buffer = VK_NULL_HANDLE;
            return std::nullopt;
        }

        VkMemoryAllocateInfo mai{};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = mr.size;
        mai.memoryTypeIndex = *mt;
        if (vkAllocateMemory(dev, &mai, nullptr, &alloc.memory) != VK_SUCCESS) {
            vkDestroyBuffer(dev, alloc.buffer, nullptr);
            alloc.buffer = VK_NULL_HANDLE;
            return std::nullopt;
        }
        vkBindBufferMemory(dev, alloc.buffer, alloc.memory, 0);
        return std::optional<VkBufferAlloc>{std::move(alloc)};
    }

    // ============================================================================
    // staging_pool — host-visible VkBuffer free-list (ARCHITECTURE_REVIEW.md item 1).
    //
    // dispatch_elementwise_full previously allocated a fresh VkBuffer + VkDeviceMemory
    // per source and destination on every dispatch — even on a warm pipeline-cache
    // hit. The pool retains freed host-visible buffers keyed by capacity (rounded up
    // to the next power of two so slightly-different element counts share a slot) and
    // hands them back on the next acquire, eliminating the per-dispatch alloc.
    //
    // Not thread-safe on its own: it is owned by device_provider and every dispatch
    // path is a blocking dispatch_sync, so a pooled buffer is always idle before it
    // is released and reused. Lifetime is tied to the VkDevice (destroyed before the
    // device in device_provider::reset()).
    // ============================================================================

    [[nodiscard]] inline std::size_t round_up_pow2(std::size_t n) noexcept {
        if (n < 2) return n ? 1 : 0;
        return std::bit_ceil(n);
    }

    class staging_pool {
    public:
        staging_pool() = default;
        staging_pool(const staging_pool&) = delete;
        staging_pool& operator=(const staging_pool&) = delete;

        // Acquire a host-visible buffer of at least `bytes` capacity. Reuses a freed
        // buffer whose capacity fits; otherwise allocates one rounded to a pow2 slot.
        [[nodiscard]] std::optional<VkBufferAlloc>
        acquire(VkDevice dev, VkPhysicalDevice phys, std::size_t bytes) {
            const std::size_t cap = round_up_pow2(bytes);
            for (std::size_t i = 0; i < free_.size(); ++i) {
                if (free_[i].dev == dev && free_[i].size >= cap) {
                    VkBufferAlloc got = std::move(free_[i]);
                    free_[i] = std::move(free_.back());
                    free_.pop_back();
                    return std::optional<VkBufferAlloc>{std::move(got)};
                }
            }
            return alloc_vk_buffer(dev, phys, cap, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        }

        // Return a buffer to the free-list (keeps device memory alive for reuse).
        void release(VkBufferAlloc&& buf) {
            if (buf.valid()) free_.push_back(std::move(buf));
        }

        void clear() noexcept { free_.clear(); } // frees all pooled device memory

    private:
        std::vector<VkBufferAlloc> free_;
    };

    // RAII lease: returns the buffer to its pool on scope exit (covers early-return
    // error paths in dispatch_elementwise_full). Tracks the requested byte length
    // separately from pool capacity, so descriptor binds use the exact kernel range.
    class pooled_buffer {
    public:
        pooled_buffer() = default;

        pooled_buffer(staging_pool& pool, VkBufferAlloc&& buf, std::size_t requested) noexcept
            : pool_(&pool), buf_(std::move(buf)), requested_(requested) {}

        pooled_buffer(const pooled_buffer&) = delete;
        pooled_buffer& operator=(const pooled_buffer&) = delete;

        pooled_buffer(pooled_buffer&& o) noexcept
            : pool_(o.pool_), buf_(std::move(o.buf_)), requested_(o.requested_) {
            o.pool_ = nullptr;
        }

        pooled_buffer& operator=(pooled_buffer&& o) noexcept {
            if (this != &o) {
                recycle();
                pool_ = o.pool_;
                buf_ = std::move(o.buf_);
                requested_ = o.requested_;
                o.pool_ = nullptr;
            }
            return *this;
        }

        ~pooled_buffer() { recycle(); }

        [[nodiscard]] bool valid() const noexcept { return buf_.valid(); }
        [[nodiscard]] VkBuffer buffer() const noexcept { return buf_.buffer; }
        [[nodiscard]] std::size_t requested() const noexcept { return requested_; }
        [[nodiscard]] const VkBufferAlloc& alloc() const noexcept { return buf_; }

    private:
        void recycle() noexcept {
            if (pool_ && buf_.valid()) pool_->release(std::move(buf_));
            pool_ = nullptr;
        }

        staging_pool* pool_ = nullptr;
        VkBufferAlloc buf_;
        std::size_t requested_ = 0;
    };

    // ============================================================================
    // device_provider — injectable, thread-safe-on-first-use owner of the shared
    // Lithe vulkan_backend + the staging_pool (ARCHITECTURE_REVIEW.md item 3).
    //
    // Replaces the former function-local `static vulkan_backend be;` singleton:
    //   - lifetime is explicit (reset() tears the pool down before the device);
    //   - first-use construction is guarded by std::once_flag (thread-safe);
    //   - inject()/reset() give tests a deterministic seam to swap or drop the
    //     device without relying on static-destruction order.
    // Keeps the "one shared VkDevice" invariant — the review calls that correct by
    // design; only the ownership + init/teardown discipline changes.
    // ============================================================================
    class device_provider {
    public:
        [[nodiscard]] static device_provider& instance() noexcept {
            static device_provider p;
            return p;
        }

        [[nodiscard]] lithe::codegen::backends::vulkan_backend& backend() {
            std::call_once(once_, [this] {
#ifdef __APPLE__
// Suppress MoltenVK info/warning spam (errors still visible at level 1).
// setenv does not overwrite if already set — respects user override.
::setenv ("MVK_CONFIG_LOG_LEVEL", "1", 0);
#endif
if (!backend_)
backend_= std::make_unique<lithe::codegen::backends::vulkan_backend>();
        });
        return *backend_;
    }

[[nodiscard]] staging_pool& pool() noexcept { return pool_; }

// Test seam: install an externally-owned backend (resets first-use guard).
void inject(std::unique_ptr<lithe::codegen::backends::vulkan_backend> be) {
    pool_.clear();
    backend_ = std::move(be);
    std::call_once(once_, [] {}); // consume the guard so backend() won't rebuild
}

// Test seam: drop the pool then the device, and re-arm first-use init.
void reset() {
    pool_.clear();
    backend_.reset();
    std::destroy_at(&once_);
    new(&once_) std::once_flag{};
}

private:
device_provider() = default;

std::unique_ptr<lithe::codegen::backends::vulkan_backend> backend_;
staging_pool pool_;
std::once_flag once_;
};

[[nodiscard]] inline lithe::codegen::backends::vulkan_backend& device() noexcept {
    return device_provider::instance().backend();
}

} // namespace pravaha::backends::vulkan

// ============================================================================
// Section 3: Kosha pipeline cache + full get_or_compile + dispatch
// ============================================================================

namespace pravaha::backends::vulkan {
    // Cached entry: both the spirv_module (for descriptor set re-building) and
    // the compiled vulkan_resource (pipeline + pools).
    struct vulkan_kernel_entry {
        lithe::codegen::backends::spirv_module module;
        std::optional<lithe::execution::vulkan_resource> resource;
    };

    using vulkan_pipeline_cache =
    kosha::adapter::ShardedCache<
        kosha::core::Cache<std::uint64_t, vulkan_kernel_entry>, 8>;

    [[nodiscard]] inline vulkan_pipeline_cache& kernel_cache() noexcept {
        static vulkan_pipeline_cache cache{256};
        return cache;
    }

    // Build (or reuse a cached) Lithe vulkan_resource for a kernel.
    //
    // Device/pipeline/pool/fence lifetime is entirely Lithe's: we call the generic
    // helpers vk_build_pipeline (binding_count = K+1) + vk_alloc_pools_and_wrap.
    // The compiled resource is cached in the kernel_entry keyed on structural_hash,
    // so repeat dispatches skip recompile (build-once-reuse). A vulkan_resource
    // copy is cheap (shared_ptr to the pipeline payload + shared pools/set), and
    // all copies share the same descriptor set — safe to re-bind between blocking
    // dispatches.
    [[nodiscard]] inline std::optional<lithe::execution::vulkan_resource>
    get_or_reuse_resource(std::uint64_t key,
                          const lithe::codegen::backends::spirv_module& module,
                          std::uint32_t binding_count) {
        auto& cache = kernel_cache();
        if (auto hit = cache.get(key); hit.has_value() && hit->resource
            && hit->resource->valid())
            return hit->resource; // copy shares the compiled pipeline + pools

        auto& be = device();
        // vk_build_pipeline consumes the module by move; copy the cached words.
        lithe::codegen::backends::spirv_module mod_copy = module;
        auto payload = lithe::codegen::backends::vk_build_pipeline(
            be, std::move(mod_copy), binding_count);
        if (!payload) return std::nullopt;
        auto res = lithe::codegen::backends::vk_alloc_pools_and_wrap(be, std::move(payload));
        if (!res) return std::nullopt;

        vulkan_kernel_entry entry;
        entry.module = module;
        entry.resource = *res; // cache a copy; return the original below
        [[maybe_unused]] auto _ = cache.put(key, std::move(entry));
        return res;
    }

    // Full element-wise dispatch: allocates staging buffers, binds them to Lithe's
    // descriptor set, dispatches through Lithe's fence/submit/wait path, reads back.
    //
    // Split of responsibility:
    //   - pravaha (here): VkBuffer alloc + host staging + readback (data plane).
    //   - Lithe: pipeline build + descriptor set + command record + fence + submit.
    template <typename T, std::size_t K>
    [[nodiscard]] Outcome<void>
    dispatch_elementwise_full(std::uint64_t key,
                              const lithe::codegen::backends::spirv_module& module,
                              compute::compute_view<T> dst,
                              const std::array<compute::compute_view<const T>, K>& srcs,
                              std::uint32_t local_x) {
        auto& be = device();
        auto ctx = be.context();
        if (!ctx || !ctx->valid())
            return std::unexpected(PravahaError::make(
                ErrorKind::ExecutorUnavailable, "vulkan: device unavailable for dispatch"));

        const VkDevice dev = ctx->device;
        const VkPhysicalDevice phys = ctx->phys_dev;
        const std::uint32_t n = static_cast<std::uint32_t>(dst.desc.element_count());
        const std::size_t bytes = static_cast<std::size_t>(n) * sizeof(T);
        if (n == 0) return {};

        // Build (or reuse) the Lithe resource: K src + 1 dst storage bindings.
        auto res = get_or_reuse_resource(key, module, static_cast<std::uint32_t>(K + 1));
        if (!res || !res->valid())
            return std::unexpected(PravahaError::make(
                ErrorKind::InternalError, "vulkan: resource build failed"));

        // Acquire staging buffers from the pool: K src + 1 dst (pravaha data plane).
        // pooled_buffer leases auto-return to the pool on scope exit (incl. errors),
        // so repeat dispatches on a warm pipeline skip VkBuffer/VkDeviceMemory alloc.
        auto& pool = device_provider::instance().pool();
        std::array<pooled_buffer, K> src_bufs;
        for (std::size_t s = 0; s < K; ++s) {
            const std::size_t sb = static_cast<std::size_t>(
                srcs[s].desc.element_count()) * sizeof(T);
            auto got = pool.acquire(dev, phys, sb);
            if (!got || !got->valid())
                return std::unexpected(PravahaError::make(
                    ErrorKind::ResourceExhausted, "vulkan: src buffer alloc failed"));
            src_bufs[s] = pooled_buffer{pool, std::move(*got), sb};
            void* ptr = src_bufs[s].alloc().map();
            if (!ptr)
                return std::unexpected(PravahaError::make(
                    ErrorKind::InternalError, "vulkan: src map failed"));
            std::memcpy(ptr, srcs[s].base(), sb);
            src_bufs[s].alloc().unmap();
        }

        auto dst_got = pool.acquire(dev, phys, bytes);
        if (!dst_got || !dst_got->valid())
            return std::unexpected(PravahaError::make(
                ErrorKind::ResourceExhausted, "vulkan: dst buffer alloc failed"));
        pooled_buffer dst_buf{pool, std::move(*dst_got), bytes};

        // Bind the pooled buffers to Lithe's own descriptor set: 0..K-1 = src, K = dst.
        // Re-binding fresh buffers each dispatch is valid because dispatch_sync is
        // blocking (the set is never in a pending submission). Bind range uses the
        // requested byte length, not pool capacity, so it matches the element count.
        std::array<lithe::execution::storage_buffer_binding, K + 1> binds{};
        for (std::size_t s = 0; s < K; ++s)
            binds[s] = {src_bufs[s].buffer(), 0, src_bufs[s].requested()};
        binds[K] = {dst_buf.buffer(), 0, bytes};
        if (auto b = res->bind_storage_buffers(binds); !b.has_value())
            return std::unexpected(PravahaError::make(
                ErrorKind::InternalError, "vulkan: descriptor bind failed"));

        // Dispatch through Lithe's fence/submit/wait path. grid_x covers n elements
        // at local_x per workgroup; block_x matches the emitted SPIR-V LocalSize.
        lithe::execution::kernel_launch launch{};
        launch.grid_x = (n + local_x - 1) / local_x;
        launch.grid_y = 1;
        launch.block_x = local_x;
        launch.block_y = 1;
        if (auto r = res->dispatch_sync(launch); !r.has_value())
            return std::unexpected(PravahaError::make(
                ErrorKind::InternalError, "vulkan: dispatch failed"));

        // Read back dst → host (pravaha data plane).
        if (void* ptr = dst_buf.alloc().map()) {
            std::memcpy(dst.base(), ptr, bytes);
            dst_buf.alloc().unmap();
        }
        return {};
    }

    // get_or_compile: returns the cached (or freshly emitted+compiled) spirv_module.
    template <typename E>
    [[nodiscard]] std::optional<lithe::codegen::backends::spirv_module>
    get_or_compile_module(const E& expr, compute::data_element_type elem) {
        const std::uint64_t key = hetero::structural_hash(expr);
        auto& cache = kernel_cache();

        // Attempt cache hit.
        if (auto hit = cache.get(key)) {
            hetero::emit_cache_event(true, key);
            return std::optional<lithe::codegen::backends::spirv_module>{hit->module};
        }

        hetero::emit_cache_event(false, key);

        // Emit SPIR-V.
        auto mod = spirv::emit_kernel(expr, elem);
        if (mod.validate() != lithe::ir::ir_resolution_state::resolved)
            return std::nullopt;

        // Ensure device is up (for future resource compile path).
        (void)device().ensure_device();

        vulkan_kernel_entry entry;
        entry.module = mod;
        entry.resource = std::nullopt; // resource compiled lazily on first dispatch

        [[maybe_unused]] auto _ = cache.put(key, std::move(entry));
        return std::optional<lithe::codegen::backends::spirv_module>{mod};
    }

    // ============================================================================
    // Reduction-tree cache + dispatch (ARCHITECTURE_REVIEW.md item 2).
    //
    // The reduce-tree kernel is a DIFFERENT program than the element-wise kernel for
    // the same child expression (Workgroup fold + one partial per group), so it is
    // cached under a distinct key that folds the reduce_op and workgroup size into
    // the child's structural hash. This keeps it from colliding with the element-wise
    // entry the two-pass path caches for the same child.
    // ============================================================================

    [[nodiscard]] inline std::uint64_t
    reduce_tree_key(std::uint64_t child_key, pravaha::expr::reduce_op op, std::uint32_t tg) noexcept {
        // splitmix-style mix so op/tg perturb every bit; avoids trivial collisions.
        std::uint64_t h = child_key ^ (0x9E3779B97F4A7C15ull * (static_cast<std::uint64_t>(op) + 1));
        h ^= static_cast<std::uint64_t>(tg) * 0xC2B2AE3D27D4EB4Full;
        h ^= h >> 29;
        h *= 0xBF58476D1CE4E5B9ull;
        h ^= h >> 32;
        return h;
    }

    // Compile (or reuse) the reduce-tree SPIR-V for a child expression. Returns
    // nullopt if emission fails validation, signalling the caller to fall back to
    // the retained two-pass CPU-fold path.
    template <typename E>
    [[nodiscard]] std::optional<lithe::codegen::backends::spirv_module>
    get_or_compile_reduce_tree(const E& child,
                               pravaha::expr::reduce_op op,
                               compute::data_element_type elem,
                               std::uint32_t tg) {
        const std::uint64_t key = reduce_tree_key(hetero::structural_hash(child), op, tg);
        auto& cache = kernel_cache();

        if (auto hit = cache.get(key)) {
            hetero::emit_cache_event(true, key);
            return std::optional<lithe::codegen::backends::spirv_module>{hit->module};
        }
        hetero::emit_cache_event(false, key);

        auto mod = spirv::emit_reduce_tree(child, op, elem, tg);
        if (mod.validate() != lithe::ir::ir_resolution_state::resolved)
            return std::nullopt;

        (void)device().ensure_device();

        vulkan_kernel_entry entry;
        entry.module = mod;
        entry.resource = std::nullopt;
        [[maybe_unused]] auto _ = cache.put(key, std::move(entry));
        return std::optional<lithe::codegen::backends::spirv_module>{mod};
    }

    // Dispatch the reduce-tree kernel: pads each source to a multiple of tg with the
    // op identity, runs one workgroup-local fold per group, and writes ceil(n/tg)
    // partials into `partials`. The caller CPU-folds those (≈ n/tg values) — the GPU
    // does the O(n) work, the host the O(n/tg) tail. Blocking dispatch_sync makes the
    // pooled staging buffers safe to reuse next call.
    template <typename T, std::size_t K>
    [[nodiscard]] Outcome<void>
    dispatch_reduce_tree_full(std::uint64_t key,
                              const lithe::codegen::backends::spirv_module& module,
                              const std::array<compute::compute_view<const T>, K>& srcs,
                              std::uint32_t tg,
                              std::vector<T>& partials,
                              T identity) {
        auto& be = device();
        auto ctx = be.context();
        if (!ctx || !ctx->valid())
            return std::unexpected(PravahaError::make(
                ErrorKind::ExecutorUnavailable, "vulkan: device unavailable for reduce dispatch"));

        const VkDevice dev = ctx->device;
        const VkPhysicalDevice phys = ctx->phys_dev;
        const std::uint32_t n = static_cast<std::uint32_t>(srcs[0].desc.element_count());
        if (n == 0) {
            partials.clear();
            return {};
        }

        const std::uint32_t groups = (n + tg - 1) / tg; // ceil(n / tg) = partial count
        const std::uint32_t n_padded = groups * tg; // padded to a whole tg multiple
        const std::size_t src_bytes = static_cast<std::size_t>(n_padded) * sizeof(T);
        const std::size_t dst_bytes = static_cast<std::size_t>(groups) * sizeof(T);

        auto res = get_or_reuse_resource(key, module, static_cast<std::uint32_t>(K + 1));
        if (!res || !res->valid())
            return std::unexpected(PravahaError::make(
                ErrorKind::InternalError, "vulkan: reduce resource build failed"));

        // K padded source staging buffers: copy n elements, fill the tail with the
        // op identity so out-of-range lanes contribute neutrally (no in-shader bounds).
        auto& pool = device_provider::instance().pool();
        std::array<pooled_buffer, K> src_bufs;
        for (std::size_t s = 0; s < K; ++s) {
            auto got = pool.acquire(dev, phys, src_bytes);
            if (!got || !got->valid())
                return std::unexpected(PravahaError::make(
                    ErrorKind::ResourceExhausted, "vulkan: reduce src buffer alloc failed"));
            src_bufs[s] = pooled_buffer{pool, std::move(*got), src_bytes};
            T* ptr = static_cast<T*>(src_bufs[s].alloc().map());
            if (!ptr)
                return std::unexpected(PravahaError::make(
                    ErrorKind::InternalError, "vulkan: reduce src map failed"));
            const std::uint32_t sn = static_cast<std::uint32_t>(srcs[s].desc.element_count());
            std::memcpy(ptr, srcs[s].base(), static_cast<std::size_t>(sn) * sizeof(T));
            for (std::uint32_t i = sn; i < n_padded; ++i) ptr[i] = identity;
            src_bufs[s].alloc().unmap();
        }

        auto dst_got = pool.acquire(dev, phys, dst_bytes);
        if (!dst_got || !dst_got->valid())
            return std::unexpected(PravahaError::make(
                ErrorKind::ResourceExhausted, "vulkan: reduce dst buffer alloc failed"));
        pooled_buffer dst_buf{pool, std::move(*dst_got), dst_bytes};

        std::array<lithe::execution::storage_buffer_binding, K + 1> binds{};
        for (std::size_t s = 0; s < K; ++s)
            binds[s] = {src_bufs[s].buffer(), 0, src_bytes};
        binds[K] = {dst_buf.buffer(), 0, dst_bytes};
        if (auto bnd = res->bind_storage_buffers(binds); !bnd.has_value())
            return std::unexpected(PravahaError::make(
                ErrorKind::InternalError, "vulkan: reduce descriptor bind failed"));

        lithe::execution::kernel_launch launch{};
        launch.grid_x = groups; // one workgroup per partial
        launch.grid_y = 1;
        launch.block_x = tg;
        launch.block_y = 1;
        if (auto r = res->dispatch_sync(launch); !r.has_value())
            return std::unexpected(PravahaError::make(
                ErrorKind::InternalError, "vulkan: reduce dispatch failed"));

        partials.resize(groups);
        if (void* ptr = dst_buf.alloc().map()) {
            std::memcpy(partials.data(), ptr, dst_bytes);
            dst_buf.alloc().unmap();
        }
        return {};
    }
} // namespace pravaha::backends::vulkan

// ============================================================================
// Section 4: VulkanGpuBackend — ComputeBackend adapter.
// Priority 150: below Metal (200), above SIMD (10).
// ============================================================================

namespace pravaha::backends::vulkan {
    // Once-initialized availability probe. Avoids re-init per call.
    struct DeviceProbe {
        bool checked = false;
        bool available = false;

        [[nodiscard]] bool probe() noexcept {
            if (checked) return available;
            checked = true;
            available = device().ensure_device();
            return available;
        }
    };

    inline DeviceProbe& device_probe() noexcept {
        static DeviceProbe probe;
        return probe;
    }

    struct VulkanGpuBackend {
        [[nodiscard]] static constexpr compute::backend_metadata static_metadata() noexcept {
            return {.name = "vulkan_gpu", .hardware_priority = 150};
        }

        [[nodiscard]] bool is_available() const noexcept {
            return device_probe().probe();
        }

        // MoltenVK-safe type support: f16/f32/i32/u32.
        // f64 rejected (no shaderFloat64 without feature query).
        [[nodiscard]] static constexpr bool supports_type(compute::data_element_type t) noexcept {
            return spirv::is_vulkan_supported_type(t);
        }

        [[nodiscard]] bool supports_expression(std::size_t /*hash*/,
                                               compute::data_element_type t) const noexcept {
            return supports_type(t) && is_available();
        }

        // GPU threshold: same as Metal (routing policy shared, not re-derived).
        static constexpr std::size_t kGpuElementwiseThreshold = 256 * 1024; // 256 KB
        static constexpr std::size_t kGpuReduceThreshold = 1024 * 1024; // 1 MB

        [[nodiscard]] std::uint64_t evaluate_cost(const compute::buffer_descriptor& desc,
                                                  std::size_t /*hash*/) const noexcept {
            if (!is_available() || !supports_type(desc.element_type)) return 0;
            if (desc.footprint_bytes() < kGpuElementwiseThreshold) return 0;
            return desc.footprint_bytes();
        }

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
            if (!is_available()) {
                hetero::emit_fallback_event("vulkan: unavailable → fallback to SIMD");
                return backends::run_simd_or_fallback(expr, dst, src, hetero::execution_context{});
            }

            const auto maybe_mod = get_or_compile_module(expr,
                                                         compute::element_type_for<T>);
            if (!maybe_mod) {
                hetero::emit_fallback_event("vulkan: SPIR-V emit failed → SIMD");
                return backends::run_simd_or_fallback(expr, dst, src, hetero::execution_context{});
            }

            constexpr std::uint32_t local_x = 256;
            const std::uint64_t key = hetero::structural_hash(expr);
            std::array<compute::compute_view<const T>, 1> srcs{src};
            hetero::nadi_gpu_dispatch_scope scope{dst.desc.footprint_bytes()};
            auto r = dispatch_elementwise_full<T, 1>(key, *maybe_mod, dst, srcs, local_x);
            if (!r.has_value()) {
                hetero::emit_fallback_event("vulkan: dispatch failed → SIMD");
                return backends::run_simd_or_fallback(expr, dst, src, hetero::execution_context{});
            }
            return r;
        }

        template <typename T, std::size_t K, lithe::Expression E>
        Outcome<void> execute_elementwise_multi(
            const E& expr,
            compute::compute_view<T> dst,
            const std::array<compute::compute_view<const T>, K>& srcs,
            const hetero::execution_context& /*ctx*/) {
            if (!is_available()) {
                hetero::emit_fallback_event("vulkan: unavailable multi → SIMD");
                return backends::run_simd_or_fallback(expr, dst, srcs[0], hetero::execution_context{});
            }

            const auto maybe_mod = get_or_compile_module(expr,
                                                         compute::element_type_for<T>);
            if (!maybe_mod) {
                hetero::emit_fallback_event("vulkan: SPIR-V multi failed → SIMD");
                return backends::run_simd_or_fallback(expr, dst, srcs[0], hetero::execution_context{});
            }

            constexpr std::uint32_t local_x = 256;
            const std::uint64_t key = hetero::structural_hash(expr);
            hetero::nadi_gpu_dispatch_scope scope{dst.desc.footprint_bytes()};
            auto r = dispatch_elementwise_full<T, K>(key, *maybe_mod, dst, srcs, local_x);
            if (!r.has_value()) {
                hetero::emit_fallback_event("vulkan: dispatch multi failed → SIMD");
                return backends::run_simd_or_fallback(expr, dst, srcs[0], hetero::execution_context{});
            }
            return r;
        }

        // Single-input reduction: GPU workgroup-tree fold in Workgroup shared memory
        // (ARCHITECTURE_REVIEW.md item 2) producing ceil(n/tg) partials, then a short
        // CPU fold of those partials. Falls back to the retained two-pass CPU-fold
        // path if the reduce-tree kernel fails to compile/validate or dispatch.
        template <pravaha::expr::reduce_op Op, typename T, lithe::Expression Child>
        Outcome<T> execute_reduction(const Child& child,
                                     compute::compute_view<const T> src) {
            if (!is_available()) {
                hetero::emit_fallback_event("vulkan: unavailable reduce → SIMD");
                std::array<compute::compute_view<const T>, 1> srcs1{src};
                return backends::run_reduce_simd_multi < Op, T, 1 > (child, srcs1);
            }

            const std::uint32_t n = static_cast<std::uint32_t>(src.desc.element_count());
            if (n == 0) {
                if constexpr (Op == pravaha::expr::reduce_op::sum) return T{0};
                else return src.base()[0];
            }

            std::array<compute::compute_view<const T>, 1> srcs{src};
            hetero::nadi_gpu_dispatch_scope scope{src.desc.footprint_bytes()};

            // GPU reduction tree: identity is 0 for sum, the first element for max/min.
            constexpr std::uint32_t tg = 256;
            const T identity = (Op == pravaha::expr::reduce_op::sum) ? T{0} : src.base()[0];
            if (auto part = reduce_via_tree<Op, T, 1>(child, srcs, src.desc.element_type, tg, identity))
                return fold_partials<Op, T>(*part);

            // Fallback: two-pass element-wise GPU pass + full CPU fold.
            std::vector<T> tmp(n);
            compute::buffer_descriptor td;
            td.shape = {n};
            td.element_type = src.desc.element_type;
            auto tmp_dst = compute::make_view(tmp.data(), td);

            const auto maybe_mod = get_or_compile_module(child, src.desc.element_type);
            if (!maybe_mod) {
                hetero::emit_fallback_event("vulkan: reduce emit failed → SIMD");
                std::array<compute::compute_view<const T>, 1> srcs1{src};
                return backends::run_reduce_simd_multi < Op, T, 1 > (child, srcs1);
            }

            const std::uint64_t key = hetero::structural_hash(child);
            auto r = dispatch_elementwise_full<T, 1>(key, *maybe_mod, tmp_dst, srcs, tg);
            if (!r.has_value()) {
                hetero::emit_fallback_event("vulkan: reduce dispatch failed → SIMD");
                std::array<compute::compute_view<const T>, 1> srcs1{src};
                return backends::run_reduce_simd_multi < Op, T, 1 > (child, srcs1);
            }
            return fold_partials<Op, T>(tmp);
        }

        template <pravaha::expr::reduce_op Op, typename T, std::size_t K, lithe::Expression Child>
        Outcome<T> execute_reduction_multi(
            const Child& child,
            const std::array<compute::compute_view<const T>, K>& srcs) {
            if (!is_available()) {
                hetero::emit_fallback_event("vulkan: unavailable reduce_multi → SIMD");
                return backends::run_reduce_simd_multi < Op, T, K > (child, srcs);
            }

            const std::uint32_t n = static_cast<std::uint32_t>(srcs[0].desc.element_count());
            if (n == 0) {
                if constexpr (Op == pravaha::expr::reduce_op::sum) return T{0};
                else return srcs[0].base()[0];
            }

            hetero::nadi_gpu_dispatch_scope scope{srcs[0].desc.footprint_bytes()};

            constexpr std::uint32_t tg = 256;
            const T identity = (Op == pravaha::expr::reduce_op::sum) ? T{0} : srcs[0].base()[0];
            if (auto part = reduce_via_tree<Op, T, K>(child, srcs, srcs[0].desc.element_type, tg, identity))
                return fold_partials<Op, T>(*part);

            // Fallback: two-pass element-wise GPU pass + full CPU fold.
            std::vector<T> tmp(n);
            compute::buffer_descriptor td;
            td.shape = {n};
            td.element_type = srcs[0].desc.element_type;
            auto tmp_dst = compute::make_view(tmp.data(), td);

            const auto maybe_mod = get_or_compile_module(child, srcs[0].desc.element_type);
            if (!maybe_mod) {
                hetero::emit_fallback_event("vulkan: reduce_multi emit failed → SIMD");
                return backends::run_reduce_simd_multi < Op, T, K > (child, srcs);
            }

            const std::uint64_t key = hetero::structural_hash(child);
            auto r = dispatch_elementwise_full<T, K>(key, *maybe_mod, tmp_dst, srcs, tg);
            if (!r.has_value()) {
                hetero::emit_fallback_event("vulkan: reduce_multi dispatch failed → SIMD");
                return backends::run_reduce_simd_multi < Op, T, K > (child, srcs);
            }
            return fold_partials<Op, T>(tmp);
        }

    private:
        // GPU reduction-tree helper shared by the single- and multi-input paths.
        // Returns the ceil(n/tg) partials on success, or nullopt to signal the caller
        // to take its two-pass fallback (compile/validate/dispatch failure).
        template <pravaha::expr::reduce_op Op, typename T, std::size_t K, lithe::Expression Child>
        [[nodiscard]] std::optional<std::vector<T>>
        reduce_via_tree(const Child& child,
                        const std::array<compute::compute_view<const T>, K>& srcs,
                        compute::data_element_type elem,
                        std::uint32_t tg,
                        T identity) {
            const auto maybe_mod =
                get_or_compile_reduce_tree(child, Op, elem, tg);
            if (!maybe_mod) return std::nullopt;

            const std::uint64_t key =
                reduce_tree_key(hetero::structural_hash(child), Op, tg);
            std::vector<T> partials;
            auto r = dispatch_reduce_tree_full<T, K>(key, *maybe_mod, srcs, tg, partials, identity);
            if (!r.has_value()) return std::nullopt;
            return partials;
        }

        // Final CPU fold of the GPU partials (≈ n/tg values). Sum uses a wide double
        // accumulator to match the documented reduce invariant; max/min stay in T.
        template <pravaha::expr::reduce_op Op, typename T>
        [[nodiscard]] static T fold_partials(const std::vector<T>& v) {
            if constexpr (Op == pravaha::expr::reduce_op::sum) {
                double acc = 0.0;
                for (const T x : v) acc += static_cast<double>(x);
                return static_cast<T>(acc);
            }
            else if constexpr (Op == pravaha::expr::reduce_op::max) {
                T acc = v[0];
                for (std::size_t i = 1; i < v.size(); ++i) if (v[i] > acc) acc = v[i];
                return acc;
            }
            else {
                T acc = v[0];
                for (std::size_t i = 1; i < v.size(); ++i) if (v[i] < acc) acc = v[i];
                return acc;
            }
        }
    };
} // namespace pravaha::backends::vulkan

#else
namespace pravaha::backends::vulkan {
    inline constexpr bool available = false;
} // namespace pravaha::backends::vulkan
#endif // LITHE_VULKAN_BACKEND_AVAILABLE

#else
namespace pravaha::backends::vulkan {
    // The header remains safely includable in Pebble-only builds.  The actual
    // backend becomes available when the downstream Lithe Vulkan headers are
    // on the include path.
    inline constexpr bool available = false;
} // namespace pravaha::backends::vulkan
#endif // PEBBLE_PRAVAHA_DETAIL_HAS_LITHE_VULKAN
