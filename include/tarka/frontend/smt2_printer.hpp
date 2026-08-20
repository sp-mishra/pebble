// =============================================================================
// Tarka — Zero-Overhead Multi-Solver SMT Substrate
// include/tarka/frontend/smt2_printer.hpp
//
// SMT-LIB2 Serializer & Formatter.
// Serializes Tarka AST Terms, Sorts, and assertion sets into standard SMT-LIB2.
// C++23, zero virtual, header-only.
// =============================================================================

#pragma once

#include "tarka/context.hpp"
#include "tarka/term.hpp"

#include <format>
#include <iomanip>
#include <sstream>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace tarka::frontend {
    class smt2_printer {
    public:
        // Format a Sort into SMT-LIB2 syntax
        static void print_sort(Sort s, std::ostream& os) {
            if (!s.valid()) {
                os << "Bool";
                return;
            }
            switch (s.kind()) {
                case SortKind::Bool: os << "Bool"; break;
                case SortKind::Int: os << "Int"; break;
                case SortKind::Real: os << "Real"; break;
                case SortKind::String: os << "String"; break;
                case SortKind::BitVec:
                    os << "(_ BitVec " << s.scalar_param() << ")";
                    break;
                case SortKind::Array: {
                    auto params = s.sort_params();
                    os << "(Array ";
                    if (params.size() >= 1) print_sort(params[0], os);
                    else os << "Bool";
                    os << " ";
                    if (params.size() >= 2) print_sort(params[1], os);
                    else os << "Bool";
                    os << ")";
                    break;
                }
                case SortKind::Function: {
                    auto params = s.sort_params();
                    if (params.empty()) {
                        os << "Bool";
                    } else {
                        print_sort(params.back(), os);
                    }
                    break;
                }
                default: os << "Bool"; break;
            }
        }

        [[nodiscard]] static std::string to_string(Sort s) {
            std::ostringstream ss;
            print_sort(s, ss);
            return ss.str();
        }

        // Format a Term into SMT-LIB2 syntax
        static void print_term(Term t, std::ostream& os) {
            if (!t.valid()) {
                os << "true";
                return;
            }

            switch (t.op()) {
                case Op::True: os << "true"; break;
                case Op::False: os << "false"; break;
                case Op::Sym: {
                    std::string_view name = t.ctx().symbol_name(t.ptr()->payload_hash);
                    if (!name.empty()) os << name;
                    else os << "s_" << t.ptr()->payload_hash;
                    break;
                }
                case Op::Lit: {
                    if (t.sort().kind() == SortKind::BitVec) {
                        auto bv = t.ctx().bv_literal(t.ptr()->payload_hash);
                        if (bv) {
                            std::uint32_t hex_digits = (bv->width + 3) / 4;
                            os << "#x" << std::hex << std::setfill('0') << std::setw(hex_digits)
                               << bv->bits << std::dec << std::setfill(' ');
                        } else {
                            os << "#x0";
                        }
                    } else if (t.sort().kind() == SortKind::Int) {
                        auto iv = t.ctx().int_literal(t.ptr()->payload_hash);
                        if (iv) os << *iv;
                        else os << "0";
                    } else if (t.sort().kind() == SortKind::Real) {
                        auto rv = t.ctx().real_literal(t.ptr()->payload_hash);
                        if (rv) {
                            if (rv->den == 1) os << rv->num << ".0";
                            else os << "(/ " << rv->num << ".0 " << rv->den << ".0)";
                        } else {
                            os << "0.0";
                        }
                    } else {
                        os << "true";
                    }
                    break;
                }
                case Op::Not:
                    os << "(not ";
                    print_term(t.children()[0], os);
                    os << ")";
                    break;
                case Op::And:
                case Op::Or:
                case Op::Xor:
                case Op::Add:
                case Op::Sub:
                case Op::Mul:
                case Op::Div:
                case Op::Lt:
                case Op::Le:
                case Op::Gt:
                case Op::Ge:
                case Op::Eq:
                case Op::Distinct:
                case Op::BvAdd:
                case Op::BvSub:
                case Op::BvMul:
                case Op::BvUdiv:
                case Op::BvSdiv:
                case Op::BvUrem:
                case Op::BvSrem:
                case Op::BvAnd:
                case Op::BvOr:
                case Op::BvXor:
                case Op::BvNot:
                case Op::BvShl:
                case Op::BvLshr:
                case Op::BvAshr:
                case Op::BvUlt:
                case Op::BvUle:
                case Op::BvSlt:
                case Op::BvSle:
                case Op::BvConcat:
                case Op::Select:
                case Op::Store:
                case Op::Ite:
                case Op::Implies: {
                    os << "(" << op_symbol(t.op());
                    for (Term c : t.children()) {
                        os << " ";
                        print_term(c, os);
                    }
                    os << ")";
                    break;
                }
                case Op::Apply: {
                    auto ch = t.children();
                    if (!ch.empty()) {
                        os << "(";
                        print_term(ch[0], os);
                        for (std::size_t i = 1; i < ch.size(); ++i) {
                            os << " ";
                            print_term(ch[i], os);
                        }
                        os << ")";
                    } else {
                        os << "apply";
                    }
                    break;
                }
                case Op::Forall:
                case Op::Exists: {
                    auto ch = t.children();
                    if (ch.size() >= 2) {
                        os << "(" << (t.op() == Op::Forall ? "forall (" : "exists (");
                        for (std::size_t i = 0; i + 1 < ch.size(); ++i) {
                            os << "(";
                            print_term(ch[i], os);
                            os << " ";
                            print_sort(ch[i].sort(), os);
                            os << ")";
                        }
                        os << ") ";
                        print_term(ch.back(), os);
                        os << ")";
                    }
                    break;
                }
                default:
                    os << "true";
                    break;
            }
        }

        [[nodiscard]] static std::string to_string(Term t) {
            std::ostringstream ss;
            print_term(t, ss);
            return ss.str();
        }

        // Serialize a complete SMT-LIB2 benchmark script
        [[nodiscard]] static std::string to_smt2_script(std::span<const Term> assertions,
                                                        std::string_view logic = "ALL") {
            std::ostringstream ss;
            ss << "(set-logic " << logic << ")\n";

            std::unordered_set<std::uint64_t> declared_symbols;
            std::vector<Term> symbols;

            auto collect_symbols = [&](this auto const& self, Term t) -> void {
                if (!t.valid()) return;
                if (t.op() == Op::Sym) {
                    if (!declared_symbols.contains(t.hash())) {
                        declared_symbols.insert(t.hash());
                        symbols.push_back(t);
                    }
                }
                for (Term c : t.children()) self(c);
            };

            for (Term a : assertions) collect_symbols(a);

            for (Term s : symbols) {
                if (s.sort().kind() == SortKind::Function) {
                    auto params = s.sort().sort_params();
                    ss << "(declare-fun " << s.ctx().symbol_name(s.ptr()->payload_hash) << " (";
                    for (std::size_t i = 0; i + 1 < params.size(); ++i) {
                        if (i > 0) ss << " ";
                        print_sort(params[i], ss);
                    }
                    ss << ") ";
                    if (!params.empty()) print_sort(params.back(), ss);
                    else ss << "Bool";
                    ss << ")\n";
                } else {
                    ss << "(declare-const " << s.ctx().symbol_name(s.ptr()->payload_hash) << " ";
                    print_sort(s.sort(), ss);
                    ss << ")\n";
                }
            }

            for (Term a : assertions) {
                ss << "(assert ";
                print_term(a, ss);
                ss << ")\n";
            }

            ss << "(check-sat)\n";
            return ss.str();
        }

    private:
        [[nodiscard]] static std::string_view op_symbol(Op op) noexcept {
            switch (op) {
                case Op::Not: return "not";
                case Op::And: return "and";
                case Op::Or: return "or";
                case Op::Xor: return "xor";
                case Op::Implies: return "=>";
                case Op::Ite: return "ite";
                case Op::Eq: return "=";
                case Op::Distinct: return "distinct";
                case Op::BvAdd: return "bvadd";
                case Op::BvSub: return "bvsub";
                case Op::BvMul: return "bvmul";
                case Op::BvUdiv: return "bvudiv";
                case Op::BvSdiv: return "bvsdiv";
                case Op::BvUrem: return "bvurem";
                case Op::BvSrem: return "bvsrem";
                case Op::BvAnd: return "bvand";
                case Op::BvOr: return "bvor";
                case Op::BvXor: return "bvxor";
                case Op::BvNot: return "bvnot";
                case Op::BvShl: return "bvshl";
                case Op::BvLshr: return "bvlshr";
                case Op::BvAshr: return "bvashr";
                case Op::BvUlt: return "bvult";
                case Op::BvUle: return "bvule";
                case Op::BvSlt: return "bvslt";
                case Op::BvSle: return "bvsle";
                case Op::BvConcat: return "concat";
                case Op::Select: return "select";
                case Op::Store: return "store";
                case Op::Add: return "+";
                case Op::Sub: return "-";
                case Op::Mul: return "*";
                case Op::Div: return "/";
                case Op::Lt: return "<";
                case Op::Le: return "<=";
                case Op::Gt: return ">";
                case Op::Ge: return ">=";
                default: return "=";
            }
        }
    };
} // namespace tarka::frontend
