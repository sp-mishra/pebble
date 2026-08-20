#pragma once
// =============================================================================
// tarka/term.hpp — Value-Semantic Handles + Op/Sort
//
// Namespace:  tarka
// Provides:
//   SortKind     — base and parameterized sort families
//   Sort         — 16-byte trivially-copyable non-owning sort handle
//   Op           — uint16_t operator enum (builtin [0,1000), extension >=1000)
//   op_descriptor<Op> — openly-specializable metadata (arity, symbol,
//                        is_commutative, theory_bits)
//   theory_mask  — bitmask over theory families
//   Term         — 16-byte trivially-copyable non-owning term handle
//   SatResult    — sat / unsat / unknown / deferred
//   SmtError     — error code + message
//   SmtValue     — variant result from model extraction
//   bv_value     — ≤64-bit bitvector value (bits + width)
//
// Design:
//   - No virtual, no macros. C++23.
//   - Term/Sort are non-owning views over arena-owned immutable interned nodes.
//   - The redundant hash field in each handle enables key lookups without
//     pointer chases.
//   - Op metadata lives in op_descriptor<Op>, openly specializable — same
//     pattern as vakya::emit::tag_descriptor.
//   - theory_bits in op_descriptor drives the capability-mask router.
// =============================================================================

#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

namespace tarka {
    // =========================================================================
    // SortKind
    // =========================================================================

    enum class SortKind : std::uint8_t {
        Bool = 0,
        Int,
        Real,
        String,
        BitVec, // scalar_param = width
        Array, // sort_params[0]=index, sort_params[1]=element
        Function, // sort_params[0..n-2]=domain, sort_params[n-1]=range
    };

    // =========================================================================
    // theory_mask — bitmask for theory families
    // =========================================================================

    enum class theory_family : std::uint32_t {
        core = 1u << 0,
        bv = 1u << 1,
        lra = 1u << 2,
        lia = 1u << 3,
        nra = 1u << 4,
        nia = 1u << 5,
        array = 1u << 6,
        uf = 1u << 7,
        quantifier = 1u << 8,
        all_qf = (1u << 9) - 1,
        all = ~0u,
    };

    using theory_mask = std::uint32_t;

    [[nodiscard]] constexpr theory_mask theory_bit(theory_family f) noexcept {
        return static_cast<theory_mask>(f);
    }

    // =========================================================================
    // Op
    // =========================================================================

    inline constexpr std::uint16_t kOpExtensionBase = 1000;

    enum class Op : std::uint16_t {
        // Core / Boolean
        Lit = 0, // value literal (payload = SmtValue)
        Sym, // symbolic variable (payload = name index)
        True,
        False,
        Not,
        And,
        Or,
        Xor,
        Implies,
        Ite, // if-then-else
        Eq,
        Distinct,
        // Quantifiers
        Forall,
        Exists,
        // Arithmetic (shared int/real)
        Add,
        Sub,
        Mul,
        Div,
        Mod,
        Neg,
        Lt,
        Le,
        Gt,
        Ge,
        // Bitvector
        BvAdd,
        BvSub,
        BvMul,
        BvUdiv,
        BvSdiv,
        BvUrem,
        BvSrem,
        BvNeg,
        BvAnd,
        BvOr,
        BvXor,
        BvNot,
        BvShl,
        BvLshr,
        BvAshr,
        BvUlt,
        BvUle,
        BvSlt,
        BvSle,
        BvConcat,
        BvExtract, // scalar_params: high, low
        BvZeroExt, // scalar_param: extension width
        BvSignExt,
        // Array
        Select,
        Store,
        // UF (uninterpreted function)
        Apply, // first child is func-symbol term
        // Extension band starts at kOpExtensionBase
    };

    // =========================================================================
    // op_descriptor<Op> — openly specializable metadata
    // =========================================================================

    template <Op O>
    struct op_descriptor {
        static constexpr std::uint16_t stable_id = static_cast<std::uint16_t>(O);
        static constexpr std::string_view symbol = "?";
        static constexpr int arity = -1; // -1 = variadic
        static constexpr bool is_commutative = false;
        static constexpr theory_mask theory_bits = theory_bit(theory_family::core);
    };

    // Specializations for builtin ops
    template <>
    struct op_descriptor<Op::True> {
        static constexpr std::uint16_t stable_id = static_cast<std::uint16_t>(Op::True);
        static constexpr std::string_view symbol = "true";
        static constexpr int arity = 0;
        static constexpr bool is_commutative = false;
        static constexpr theory_mask theory_bits = theory_bit(theory_family::core);
    };

    template <>
    struct op_descriptor<Op::False> {
        static constexpr std::uint16_t stable_id = static_cast<std::uint16_t>(Op::False);
        static constexpr std::string_view symbol = "false";
        static constexpr int arity = 0;
        static constexpr bool is_commutative = false;
        static constexpr theory_mask theory_bits = theory_bit(theory_family::core);
    };

    template <>
    struct op_descriptor<Op::Not> {
        static constexpr std::uint16_t stable_id = static_cast<std::uint16_t>(Op::Not);
        static constexpr std::string_view symbol = "not";
        static constexpr int arity = 1;
        static constexpr bool is_commutative = false;
        static constexpr theory_mask theory_bits = theory_bit(theory_family::core);
    };

    template <>
    struct op_descriptor<Op::And> {
        static constexpr std::uint16_t stable_id = static_cast<std::uint16_t>(Op::And);
        static constexpr std::string_view symbol = "and";
        static constexpr int arity = -1;
        static constexpr bool is_commutative = true;
        static constexpr theory_mask theory_bits = theory_bit(theory_family::core);
    };

    template <>
    struct op_descriptor<Op::Or> {
        static constexpr std::uint16_t stable_id = static_cast<std::uint16_t>(Op::Or);
        static constexpr std::string_view symbol = "or";
        static constexpr int arity = -1;
        static constexpr bool is_commutative = true;
        static constexpr theory_mask theory_bits = theory_bit(theory_family::core);
    };

    template <>
    struct op_descriptor<Op::Xor> {
        static constexpr std::uint16_t stable_id = static_cast<std::uint16_t>(Op::Xor);
        static constexpr std::string_view symbol = "xor";
        static constexpr int arity = -1;
        static constexpr bool is_commutative = true;
        static constexpr theory_mask theory_bits = theory_bit(theory_family::core);
    };

    template <>
    struct op_descriptor<Op::Implies> {
        static constexpr std::uint16_t stable_id = static_cast<std::uint16_t>(Op::Implies);
        static constexpr std::string_view symbol = "=>";
        static constexpr int arity = 2;
        static constexpr bool is_commutative = false;
        static constexpr theory_mask theory_bits = theory_bit(theory_family::core);
    };

    template <>
    struct op_descriptor<Op::Ite> {
        static constexpr std::uint16_t stable_id = static_cast<std::uint16_t>(Op::Ite);
        static constexpr std::string_view symbol = "ite";
        static constexpr int arity = 3;
        static constexpr bool is_commutative = false;
        static constexpr theory_mask theory_bits = theory_bit(theory_family::core);
    };

    template <>
    struct op_descriptor<Op::Eq> {
        static constexpr std::uint16_t stable_id = static_cast<std::uint16_t>(Op::Eq);
        static constexpr std::string_view symbol = "=";
        static constexpr int arity = 2;
        static constexpr bool is_commutative = true;
        static constexpr theory_mask theory_bits = theory_bit(theory_family::core) | theory_bit(theory_family::uf);
    };

    template <>
    struct op_descriptor<Op::Distinct> {
        static constexpr std::uint16_t stable_id = static_cast<std::uint16_t>(Op::Distinct);
        static constexpr std::string_view symbol = "distinct";
        static constexpr int arity = -1;
        static constexpr bool is_commutative = true;
        static constexpr theory_mask theory_bits = theory_bit(theory_family::core) | theory_bit(theory_family::uf);
    };

    template <>
    struct op_descriptor<Op::Add> {
        static constexpr std::uint16_t stable_id = static_cast<std::uint16_t>(Op::Add);
        static constexpr std::string_view symbol = "+";
        static constexpr int arity = -1;
        static constexpr bool is_commutative = true;
        static constexpr theory_mask theory_bits = theory_bit(theory_family::lra) | theory_bit(theory_family::lia);
    };

    template <>
    struct op_descriptor<Op::Sub> {
        static constexpr std::uint16_t stable_id = static_cast<std::uint16_t>(Op::Sub);
        static constexpr std::string_view symbol = "-";
        static constexpr int arity = -1;
        static constexpr bool is_commutative = false;
        static constexpr theory_mask theory_bits = theory_bit(theory_family::lra) | theory_bit(theory_family::lia);
    };

    template <>
    struct op_descriptor<Op::Mul> {
        static constexpr std::uint16_t stable_id = static_cast<std::uint16_t>(Op::Mul);
        static constexpr std::string_view symbol = "*";
        static constexpr int arity = -1;
        static constexpr bool is_commutative = true;
        static constexpr theory_mask theory_bits = theory_bit(theory_family::lra) | theory_bit(theory_family::lia) |
            theory_bit(theory_family::nra) | theory_bit(theory_family::nia);
    };

    template <>
    struct op_descriptor<Op::Div> {
        static constexpr std::uint16_t stable_id = static_cast<std::uint16_t>(Op::Div);
        static constexpr std::string_view symbol = "/";
        static constexpr int arity = 2;
        static constexpr bool is_commutative = false;
        static constexpr theory_mask theory_bits = theory_bit(theory_family::lra) | theory_bit(theory_family::lia) |
            theory_bit(theory_family::nra) | theory_bit(theory_family::nia);
    };

    template <>
    struct op_descriptor<Op::Mod> {
        static constexpr std::uint16_t stable_id = static_cast<std::uint16_t>(Op::Mod);
        static constexpr std::string_view symbol = "mod";
        static constexpr int arity = 2;
        static constexpr bool is_commutative = false;
        static constexpr theory_mask theory_bits = theory_bit(theory_family::lia) | theory_bit(theory_family::nia);
    };

    template <>
    struct op_descriptor<Op::Neg> {
        static constexpr std::uint16_t stable_id = static_cast<std::uint16_t>(Op::Neg);
        static constexpr std::string_view symbol = "-";
        static constexpr int arity = 1;
        static constexpr bool is_commutative = false;
        static constexpr theory_mask theory_bits = theory_bit(theory_family::lra) | theory_bit(theory_family::lia);
    };

    template <>
    struct op_descriptor<Op::Lt> {
        static constexpr std::uint16_t stable_id = static_cast<std::uint16_t>(Op::Lt);
        static constexpr std::string_view symbol = "<";
        static constexpr int arity = 2;
        static constexpr bool is_commutative = false;
        static constexpr theory_mask theory_bits = theory_bit(theory_family::lra) | theory_bit(theory_family::lia);
    };

    template <>
    struct op_descriptor<Op::Le> {
        static constexpr std::uint16_t stable_id = static_cast<std::uint16_t>(Op::Le);
        static constexpr std::string_view symbol = "<=";
        static constexpr int arity = 2;
        static constexpr bool is_commutative = false;
        static constexpr theory_mask theory_bits = theory_bit(theory_family::lra) | theory_bit(theory_family::lia);
    };

    template <>
    struct op_descriptor<Op::Gt> {
        static constexpr std::uint16_t stable_id = static_cast<std::uint16_t>(Op::Gt);
        static constexpr std::string_view symbol = ">";
        static constexpr int arity = 2;
        static constexpr bool is_commutative = false;
        static constexpr theory_mask theory_bits = theory_bit(theory_family::lra) | theory_bit(theory_family::lia);
    };

    template <>
    struct op_descriptor<Op::Ge> {
        static constexpr std::uint16_t stable_id = static_cast<std::uint16_t>(Op::Ge);
        static constexpr std::string_view symbol = ">=";
        static constexpr int arity = 2;
        static constexpr bool is_commutative = false;
        static constexpr theory_mask theory_bits = theory_bit(theory_family::lra) | theory_bit(theory_family::lia);
    };

    template <>
    struct op_descriptor<Op::BvAdd> {
        static constexpr std::uint16_t stable_id = static_cast<std::uint16_t>(Op::BvAdd);
        static constexpr std::string_view symbol = "bvadd";
        static constexpr int arity = 2;
        static constexpr bool is_commutative = true;
        static constexpr theory_mask theory_bits = theory_bit(theory_family::bv);
    };

    template <>
    struct op_descriptor<Op::BvSub> {
        static constexpr std::uint16_t stable_id = static_cast<std::uint16_t>(Op::BvSub);
        static constexpr std::string_view symbol = "bvsub";
        static constexpr int arity = 2;
        static constexpr bool is_commutative = false;
        static constexpr theory_mask theory_bits = theory_bit(theory_family::bv);
    };

    template <>
    struct op_descriptor<Op::BvMul> {
        static constexpr std::uint16_t stable_id = static_cast<std::uint16_t>(Op::BvMul);
        static constexpr std::string_view symbol = "bvmul";
        static constexpr int arity = 2;
        static constexpr bool is_commutative = true;
        static constexpr theory_mask theory_bits = theory_bit(theory_family::bv);
    };

    template <>
    struct op_descriptor<Op::BvUdiv> {
        static constexpr std::uint16_t stable_id = static_cast<std::uint16_t>(Op::BvUdiv);
        static constexpr std::string_view symbol = "bvudiv";
        static constexpr int arity = 2;
        static constexpr bool is_commutative = false;
        static constexpr theory_mask theory_bits = theory_bit(theory_family::bv);
    };

    template <>
    struct op_descriptor<Op::BvSdiv> {
        static constexpr std::uint16_t stable_id = static_cast<std::uint16_t>(Op::BvSdiv);
        static constexpr std::string_view symbol = "bvsdiv";
        static constexpr int arity = 2;
        static constexpr bool is_commutative = false;
        static constexpr theory_mask theory_bits = theory_bit(theory_family::bv);
    };

    template <>
    struct op_descriptor<Op::BvUrem> {
        static constexpr std::uint16_t stable_id = static_cast<std::uint16_t>(Op::BvUrem);
        static constexpr std::string_view symbol = "bvurem";
        static constexpr int arity = 2;
        static constexpr bool is_commutative = false;
        static constexpr theory_mask theory_bits = theory_bit(theory_family::bv);
    };

    template <>
    struct op_descriptor<Op::BvSrem> {
        static constexpr std::uint16_t stable_id = static_cast<std::uint16_t>(Op::BvSrem);
        static constexpr std::string_view symbol = "bvsrem";
        static constexpr int arity = 2;
        static constexpr bool is_commutative = false;
        static constexpr theory_mask theory_bits = theory_bit(theory_family::bv);
    };

    template <>
    struct op_descriptor<Op::BvNeg> {
        static constexpr std::uint16_t stable_id = static_cast<std::uint16_t>(Op::BvNeg);
        static constexpr std::string_view symbol = "bvneg";
        static constexpr int arity = 1;
        static constexpr bool is_commutative = false;
        static constexpr theory_mask theory_bits = theory_bit(theory_family::bv);
    };

    template <>
    struct op_descriptor<Op::BvAnd> {
        static constexpr std::uint16_t stable_id = static_cast<std::uint16_t>(Op::BvAnd);
        static constexpr std::string_view symbol = "bvand";
        static constexpr int arity = 2;
        static constexpr bool is_commutative = true;
        static constexpr theory_mask theory_bits = theory_bit(theory_family::bv);
    };

    template <>
    struct op_descriptor<Op::BvOr> {
        static constexpr std::uint16_t stable_id = static_cast<std::uint16_t>(Op::BvOr);
        static constexpr std::string_view symbol = "bvor";
        static constexpr int arity = 2;
        static constexpr bool is_commutative = true;
        static constexpr theory_mask theory_bits = theory_bit(theory_family::bv);
    };

    template <>
    struct op_descriptor<Op::BvXor> {
        static constexpr std::uint16_t stable_id = static_cast<std::uint16_t>(Op::BvXor);
        static constexpr std::string_view symbol = "bvxor";
        static constexpr int arity = 2;
        static constexpr bool is_commutative = true;
        static constexpr theory_mask theory_bits = theory_bit(theory_family::bv);
    };

    template <>
    struct op_descriptor<Op::BvNot> {
        static constexpr std::uint16_t stable_id = static_cast<std::uint16_t>(Op::BvNot);
        static constexpr std::string_view symbol = "bvnot";
        static constexpr int arity = 1;
        static constexpr bool is_commutative = false;
        static constexpr theory_mask theory_bits = theory_bit(theory_family::bv);
    };

    template <>
    struct op_descriptor<Op::BvShl> {
        static constexpr std::uint16_t stable_id = static_cast<std::uint16_t>(Op::BvShl);
        static constexpr std::string_view symbol = "bvshl";
        static constexpr int arity = 2;
        static constexpr bool is_commutative = false;
        static constexpr theory_mask theory_bits = theory_bit(theory_family::bv);
    };

    template <>
    struct op_descriptor<Op::BvLshr> {
        static constexpr std::uint16_t stable_id = static_cast<std::uint16_t>(Op::BvLshr);
        static constexpr std::string_view symbol = "bvlshr";
        static constexpr int arity = 2;
        static constexpr bool is_commutative = false;
        static constexpr theory_mask theory_bits = theory_bit(theory_family::bv);
    };

    template <>
    struct op_descriptor<Op::BvAshr> {
        static constexpr std::uint16_t stable_id = static_cast<std::uint16_t>(Op::BvAshr);
        static constexpr std::string_view symbol = "bvashr";
        static constexpr int arity = 2;
        static constexpr bool is_commutative = false;
        static constexpr theory_mask theory_bits = theory_bit(theory_family::bv);
    };

    template <>
    struct op_descriptor<Op::BvUlt> {
        static constexpr std::uint16_t stable_id = static_cast<std::uint16_t>(Op::BvUlt);
        static constexpr std::string_view symbol = "bvult";
        static constexpr int arity = 2;
        static constexpr bool is_commutative = false;
        static constexpr theory_mask theory_bits = theory_bit(theory_family::bv);
    };

    template <>
    struct op_descriptor<Op::BvUle> {
        static constexpr std::uint16_t stable_id = static_cast<std::uint16_t>(Op::BvUle);
        static constexpr std::string_view symbol = "bvule";
        static constexpr int arity = 2;
        static constexpr bool is_commutative = false;
        static constexpr theory_mask theory_bits = theory_bit(theory_family::bv);
    };

    template <>
    struct op_descriptor<Op::BvSlt> {
        static constexpr std::uint16_t stable_id = static_cast<std::uint16_t>(Op::BvSlt);
        static constexpr std::string_view symbol = "bvslt";
        static constexpr int arity = 2;
        static constexpr bool is_commutative = false;
        static constexpr theory_mask theory_bits = theory_bit(theory_family::bv);
    };

    template <>
    struct op_descriptor<Op::BvSle> {
        static constexpr std::uint16_t stable_id = static_cast<std::uint16_t>(Op::BvSle);
        static constexpr std::string_view symbol = "bvsle";
        static constexpr int arity = 2;
        static constexpr bool is_commutative = false;
        static constexpr theory_mask theory_bits = theory_bit(theory_family::bv);
    };

    template <>
    struct op_descriptor<Op::BvConcat> {
        static constexpr std::uint16_t stable_id = static_cast<std::uint16_t>(Op::BvConcat);
        static constexpr std::string_view symbol = "concat";
        static constexpr int arity = 2;
        static constexpr bool is_commutative = false;
        static constexpr theory_mask theory_bits = theory_bit(theory_family::bv);
    };

    template <>
    struct op_descriptor<Op::BvExtract> {
        static constexpr std::uint16_t stable_id = static_cast<std::uint16_t>(Op::BvExtract);
        static constexpr std::string_view symbol = "extract";
        static constexpr int arity = 1;
        static constexpr bool is_commutative = false;
        static constexpr theory_mask theory_bits = theory_bit(theory_family::bv);
    };

    template <>
    struct op_descriptor<Op::BvZeroExt> {
        static constexpr std::uint16_t stable_id = static_cast<std::uint16_t>(Op::BvZeroExt);
        static constexpr std::string_view symbol = "zero_extend";
        static constexpr int arity = 1;
        static constexpr bool is_commutative = false;
        static constexpr theory_mask theory_bits = theory_bit(theory_family::bv);
    };

    template <>
    struct op_descriptor<Op::BvSignExt> {
        static constexpr std::uint16_t stable_id = static_cast<std::uint16_t>(Op::BvSignExt);
        static constexpr std::string_view symbol = "sign_extend";
        static constexpr int arity = 1;
        static constexpr bool is_commutative = false;
        static constexpr theory_mask theory_bits = theory_bit(theory_family::bv);
    };

    template <>
    struct op_descriptor<Op::Select> {
        static constexpr std::uint16_t stable_id = static_cast<std::uint16_t>(Op::Select);
        static constexpr std::string_view symbol = "select";
        static constexpr int arity = 2;
        static constexpr bool is_commutative = false;
        static constexpr theory_mask theory_bits = theory_bit(theory_family::array);
    };

    template <>
    struct op_descriptor<Op::Store> {
        static constexpr std::uint16_t stable_id = static_cast<std::uint16_t>(Op::Store);
        static constexpr std::string_view symbol = "store";
        static constexpr int arity = 3;
        static constexpr bool is_commutative = false;
        static constexpr theory_mask theory_bits = theory_bit(theory_family::array);
    };

    template <>
    struct op_descriptor<Op::Apply> {
        static constexpr std::uint16_t stable_id = static_cast<std::uint16_t>(Op::Apply);
        static constexpr std::string_view symbol = "apply";
        static constexpr int arity = -1;
        static constexpr bool is_commutative = false;
        static constexpr theory_mask theory_bits = theory_bit(theory_family::uf);
    };

    // =========================================================================
    // Runtime op metadata query (for non-constexpr contexts)
    // =========================================================================

    struct op_info {
        std::uint16_t stable_id;
        std::string_view symbol;
        int arity;
        bool is_commutative;
        theory_mask theory_bits;
    };

    [[nodiscard]] inline op_info get_op_info(Op o) noexcept; // defined after Sort/Term forward decls

    // =========================================================================
    // Forward declarations (full defs in context.hpp)
    // =========================================================================

    struct TermImpl;
    struct SortImpl;
    class Context;

    // =========================================================================
    // Sort — 16-byte non-owning handle
    // =========================================================================

    struct Sort {
        const SortImpl* ptr_ = nullptr;
        std::uint64_t hash_ = 0;

        [[nodiscard]] bool valid() const noexcept { return ptr_ != nullptr; }
        [[nodiscard]] std::uint64_t hash() const noexcept { return hash_; }
        [[nodiscard]] const SortImpl* ptr() const noexcept { return ptr_; }

        [[nodiscard]] bool operator==(const Sort& o) const noexcept {
            return ptr_ == o.ptr_;
        }

        [[nodiscard]] SortKind kind() const noexcept;
        [[nodiscard]] std::uint32_t scalar_param() const noexcept;
        [[nodiscard]] std::span<const Sort> sort_params() const noexcept;
    };

    static_assert(sizeof(Sort) == 16);
    static_assert(std::is_trivially_copyable_v<Sort>);

    // =========================================================================
    // bv_value — bitvector model value
    // =========================================================================

    struct bv_value {
        std::uint64_t bits = 0;
        std::uint32_t width = 0;

        [[nodiscard]] bool operator==(const bv_value&) const noexcept = default;
    };

    // rational — numerator/denominator
    struct rational {
        std::int64_t num = 0;
        std::int64_t den = 1;
        [[nodiscard]] bool operator==(const rational&) const noexcept = default;
    };

    // SmtValue — model-extraction result
    using SmtValue = std::variant<
        bool,
        bv_value,
        std::int64_t,
        rational,
        std::string
    >;

    // =========================================================================
    // SatResult / SmtError
    // =========================================================================

    enum class SatResult : std::uint8_t { Sat, Unsat, Unknown, Deferred };

    struct SmtError {
        enum class Kind : std::uint8_t { Internal, Unsupported, Timeout, ResourceLimit };

        Kind kind = Kind::Internal;
        std::string message;
    };

    // =========================================================================
    // Term — 16-byte non-owning handle
    // =========================================================================

    struct Term {
        const TermImpl* ptr_ = nullptr;
        std::uint64_t hash_ = 0;

        [[nodiscard]] bool valid() const noexcept { return ptr_ != nullptr; }
        [[nodiscard]] std::uint64_t hash() const noexcept { return hash_; }
        [[nodiscard]] const TermImpl* ptr() const noexcept { return ptr_; }

        [[nodiscard]] Op op() const noexcept;
        [[nodiscard]] Sort sort() const noexcept;
        [[nodiscard]] std::span<const Term> children() const noexcept;
        [[nodiscard]] Context& ctx() const noexcept;

        // Expression building — recover Context from node, so handles stay 16B/trivial
        [[nodiscard]] Term operator&&(Term rhs) const;
        [[nodiscard]] Term operator||(Term rhs) const;
        [[nodiscard]] Term operator!() const;
        [[nodiscard]] Term operator==(Term rhs) const;
        [[nodiscard]] Term operator!=(Term rhs) const;
        [[nodiscard]] Term operator+(Term rhs) const;
        [[nodiscard]] Term operator-(Term rhs) const;
        [[nodiscard]] Term operator*(Term rhs) const;
        [[nodiscard]] Term operator<(Term rhs) const;
        [[nodiscard]] Term operator<=(Term rhs) const;
        [[nodiscard]] Term operator>(Term rhs) const;
        [[nodiscard]] Term operator>=(Term rhs) const;
    };

    static_assert(sizeof(Term) == 16);
    static_assert(std::is_trivially_copyable_v<Term>);
} // namespace tarka

// std::hash specializations
namespace std {
    template <>
    struct hash<tarka::Sort> {
        [[nodiscard]] std::size_t operator()(const tarka::Sort& s) const noexcept {
            return static_cast<std::size_t>(s.hash_);
        }
    };

    template <>
    struct hash<tarka::Term> {
        [[nodiscard]] std::size_t operator()(const tarka::Term& t) const noexcept {
            return static_cast<std::size_t>(t.hash_);
        }
    };
} // namespace std
