// =============================================================================
// Tarka — Zero-Overhead Multi-Solver SMT Substrate
// include/tarka/native/model_validator.hpp
//
// Model Formatter & Independent SAT Witness Validator.
// Evaluates assertion terms against the extracted model to verify correctness.
// C++23, zero virtual, header-only.
// =============================================================================

#pragma once

#include "tarka/context.hpp"
#include "tarka/frontend/smt2_printer.hpp"
#include "tarka/term.hpp"

#include <cstdint>
#include <iomanip>
#include <iostream>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace tarka::native {
    struct ValidationResult {
        bool is_valid = true;
        std::vector<Term> violated_assertions;
        std::string error_message;
    };

    class model_validator {
    public:
        // Format model into SMT-LIB2 (model ...) block
        static std::string format_model(const std::unordered_map<Term, SmtValue>& model) {
            std::ostringstream ss;
            ss << "(\n";
            for (const auto& [term, val] : model) {
                if (!term.valid()) continue;
                std::string_view name = term.ctx().symbol_name(term.ptr()->payload_hash);
                if (name.empty()) continue;

                ss << "  (define-fun " << name << " () "
                   << frontend::smt2_printer::to_string(term.sort()) << " ";

                std::visit([&ss](const auto& v) {
                    using T = std::decay_t<decltype(v)>;
                    if constexpr (std::is_same_v<T, bool>) {
                        ss << (v ? "true" : "false");
                    } else if constexpr (std::is_same_v<T, bv_value>) {
                        std::uint32_t hex_digits = (v.width + 3) / 4;
                        ss << "#x" << std::hex << std::setfill('0') << std::setw(hex_digits)
                           << v.bits << std::dec << std::setfill(' ');
                    } else if constexpr (std::is_same_v<T, std::int64_t>) {
                        ss << v;
                    } else if constexpr (std::is_same_v<T, rational>) {
                        if (v.den == 1) ss << v.num << ".0";
                        else ss << "(/ " << v.num << ".0 " << v.den << ".0)";
                    } else if constexpr (std::is_same_v<T, std::string>) {
                        ss << "\"" << v << "\"";
                    }
                }, val);

                ss << ")\n";
            }
            ss << ")\n";
            return ss.str();
        }

        // Independently evaluate and validate a list of assertions against the model
        static ValidationResult validate(std::span<const Term> assertions,
                                         const std::unordered_map<Term, SmtValue>& model) {
            ValidationResult res;

            for (Term a : assertions) {
                auto val = eval_term(a, model);
                if (!val.has_value()) {
                    res.is_valid = false;
                    res.violated_assertions.push_back(a);
                    res.error_message = "Failed to evaluate term: " + frontend::smt2_printer::to_string(a);
                    return res;
                }

                if (std::holds_alternative<bool>(*val)) {
                    if (!std::get<bool>(*val)) {
                        res.is_valid = false;
                        res.violated_assertions.push_back(a);
                    }
                }
            }

            return res;
        }

    private:
        static std::optional<SmtValue> eval_term(Term t, const std::unordered_map<Term, SmtValue>& model) {
            if (!t.valid()) return true;

            if (t.op() == Op::True) return true;
            if (t.op() == Op::False) return false;

            if (t.op() == Op::Sym) {
                auto it = model.find(t);
                if (it != model.end()) return it->second;
                // Default unassigned boolean is true/false, bv is 0
                if (t.sort().kind() == SortKind::Bool) return false;
                if (t.sort().kind() == SortKind::BitVec) return bv_value{0, t.sort().scalar_param()};
                if (t.sort().kind() == SortKind::Int) return std::int64_t{0};
                if (t.sort().kind() == SortKind::Real) return rational{0, 1};
                return std::nullopt;
            }

            if (t.op() == Op::Lit) {
                if (t.sort().kind() == SortKind::BitVec) {
                    return t.ctx().bv_literal(t.ptr()->payload_hash);
                }
                if (t.sort().kind() == SortKind::Int) {
                    auto iv = t.ctx().int_literal(t.ptr()->payload_hash);
                    if (iv) return *iv;
                }
                if (t.sort().kind() == SortKind::Real) {
                    auto rv = t.ctx().real_literal(t.ptr()->payload_hash);
                    if (rv) return *rv;
                }
                return true;
            }

            auto ch = t.children();
            std::vector<SmtValue> ch_vals;
            ch_vals.reserve(ch.size());
            for (Term c : ch) {
                auto v = eval_term(c, model);
                if (!v) return std::nullopt;
                ch_vals.push_back(*v);
            }

            if (t.op() == Op::Not && ch_vals.size() == 1 && std::holds_alternative<bool>(ch_vals[0])) {
                return !std::get<bool>(ch_vals[0]);
            }

            if (t.op() == Op::And) {
                for (const auto& v : ch_vals) {
                    if (std::holds_alternative<bool>(v) && !std::get<bool>(v)) return false;
                }
                return true;
            }

            if (t.op() == Op::Or) {
                for (const auto& v : ch_vals) {
                    if (std::holds_alternative<bool>(v) && std::get<bool>(v)) return true;
                }
                return false;
            }

            if (t.op() == Op::Eq && ch_vals.size() == 2) {
                return (ch_vals[0] == ch_vals[1]);
            }

            if (t.op() == Op::Distinct && ch_vals.size() == 2) {
                return (ch_vals[0] != ch_vals[1]);
            }

            if (t.op() == Op::Ite && ch_vals.size() == 3 && std::holds_alternative<bool>(ch_vals[0])) {
                return std::get<bool>(ch_vals[0]) ? ch_vals[1] : ch_vals[2];
            }

            // BitVector arithmetic / bitwise
            if (ch_vals.size() == 2 && std::holds_alternative<bv_value>(ch_vals[0]) && std::holds_alternative<bv_value>(ch_vals[1])) {
                const auto& a = std::get<bv_value>(ch_vals[0]);
                const auto& b = std::get<bv_value>(ch_vals[1]);
                const std::uint64_t mask = (a.width == 64) ? ~0ULL : ((1ULL << a.width) - 1ULL);

                switch (t.op()) {
                    case Op::BvAdd: return bv_value{(a.bits + b.bits) & mask, a.width};
                    case Op::BvSub: return bv_value{(a.bits - b.bits) & mask, a.width};
                    case Op::BvMul: return bv_value{(a.bits * b.bits) & mask, a.width};
                    case Op::BvUdiv: return bv_value{(b.bits == 0) ? mask : ((a.bits / b.bits) & mask), a.width};
                    case Op::BvUrem: return bv_value{(b.bits == 0) ? a.bits : ((a.bits % b.bits) & mask), a.width};
                    case Op::BvAnd: return bv_value{(a.bits & b.bits) & mask, a.width};
                    case Op::BvOr:  return bv_value{(a.bits | b.bits) & mask, a.width};
                    case Op::BvXor: return bv_value{(a.bits ^ b.bits) & mask, a.width};
                    case Op::BvShl: return bv_value{(b.bits >= a.width) ? 0ULL : ((a.bits << b.bits) & mask), a.width};
                    case Op::BvLshr: return bv_value{(b.bits >= a.width) ? 0ULL : ((a.bits >> b.bits) & mask), a.width};
                    case Op::BvUlt: return (a.bits < b.bits);
                    case Op::BvUle: return (a.bits <= b.bits);
                    case Op::BvSlt: {
                        std::int64_t sa = static_cast<std::int64_t>(a.bits);
                        std::int64_t sb = static_cast<std::int64_t>(b.bits);
                        return (sa < sb);
                    }
                    default: break;
                }
            }

            if (t.op() == Op::BvNot && ch_vals.size() == 1 && std::holds_alternative<bv_value>(ch_vals[0])) {
                const auto& a = std::get<bv_value>(ch_vals[0]);
                const std::uint64_t mask = (a.width == 64) ? ~0ULL : ((1ULL << a.width) - 1ULL);
                return bv_value{(~a.bits) & mask, a.width};
            }

            return std::nullopt;
        }
    };
} // namespace tarka::native
