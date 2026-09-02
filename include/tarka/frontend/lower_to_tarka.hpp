#pragma once

#include "tarka/frontend/ir.hpp"
#include "tarka/tarka.hpp"

#include <expected>
#include <unordered_map>

namespace tarka::frontend {
    struct lowering_result {
        std::vector<Term> assertions;
        std::expected<SatResult, SmtError> last_result{SatResult::Deferred};
    };

    namespace detail {
        inline std::expected<Sort, std::string> lower_sort(const ir::script& s, Context& ctx, ir::node_id id) {
            const auto& n = s.nodes[id];
            switch (n.kind) {
            case ir::kind::sort_bool: return ctx.bool_sort();
            case ir::kind::sort_int: return ctx.int_sort();
            case ir::kind::sort_real: return ctx.real_sort();
            case ir::kind::sort_string: return ctx.string_sort();
            case ir::kind::sort_bitvec: return ctx.bv_sort(std::get<std::uint32_t>(n.ext.value));
            case ir::kind::sort_array: {
                auto c = s.nodes.children(id);
                if (c.size() != 2) return std::unexpected("array sort needs two arguments");
                auto a = lower_sort(s, ctx, c[0]);
                auto b = lower_sort(s, ctx, c[1]);
                if (!a || !b) return std::unexpected("invalid array sort");
                return ctx.array_sort(*a, *b);
            }
            default: return std::unexpected("not a sort");
            }
        }

        inline std::optional<Op> op(std::string_view n) {
            static constexpr std::pair<std::string_view, Op> table[] = {
                {"not", Op::Not}, {"and", Op::And}, {"or", Op::Or}, {"xor", Op::Xor}, {"=>", Op::Implies},
                {"ite", Op::Ite}, {"=", Op::Eq}, {"distinct", Op::Distinct}, {"+", Op::Add}, {"-", Op::Sub},
                {"*", Op::Mul}, {"/", Op::Div}, {"mod", Op::Mod}, {"<", Op::Lt}, {"<=", Op::Le}, {">", Op::Gt},
                {">=", Op::Ge}, {"bvadd", Op::BvAdd}, {"bvsub", Op::BvSub}, {"bvmul", Op::BvMul}, {"bvand", Op::BvAnd},
                {"bvor", Op::BvOr}, {"bvxor", Op::BvXor}, {"select", Op::Select}, {"store", Op::Store}
            };
            for (auto [sp, value] : table) if (sp == n) return value;
            return std::nullopt;
        }
    }

    template <SmtSolverBackend... Backends>
    [[nodiscard]] inline std::expected<lowering_result, std::string>
    lower_to_tarka(const ir::script& script, Context& ctx, RouterEngine<Backends...>& solver) {
        std::unordered_map<lang::symbol_id, Term> symbols;
        std::function < std::expected<Term, std::string>(ir::node_id) > term;
        term = [&](ir::node_id id) -> std::expected<Term, std::string> {
            const auto& n = script.nodes[id];
            const auto kids = script.nodes.children(id);
            if (n.kind == ir::kind::symbol) {
                if (auto it = symbols.find(n.name); it != symbols.end()) return it->second;
                return std::unexpected("unknown symbol: " + std::string(script.symbols.get(n.name)));
            }
            if (n.kind == ir::kind::bool_literal) return ctx.make_bool(std::get<std::uint32_t>(n.ext.value) != 0);
            if (n.kind == ir::kind::int_literal) return ctx.make_int(std::get<std::int64_t>(n.ext.value),
                                                                     ctx.int_sort());
            if (n.kind == ir::kind::bv_literal) {
                auto v = std::get<ir::bit_vector>(n.ext.value);
                return ctx.make_value(v.bits, ctx.bv_sort(v.width));
            }
            if (n.kind != ir::kind::application) return std::unexpected("node is not an SMT term");
            std::vector<Term> args;
            for (auto kid : kids) {
                auto a = term(kid);
                if (!a)return std::unexpected(a.error());
                args.push_back(*a);
            }
            const auto spelling = script.symbols.get(n.name);
            if (auto builtin = detail::op(spelling)) {
                Sort result = ctx.bool_sort();
                if (*builtin == Op::Add || *builtin == Op::Sub || *builtin == Op::Mul || *builtin == Op::Div || *builtin
                    == Op::Mod || *builtin == Op::BvAdd || *builtin == Op::BvSub || *builtin == Op::BvMul || *builtin ==
                    Op::BvAnd || *builtin == Op::BvOr || *builtin == Op::BvXor || *builtin == Op::Store) result =
                    args.empty() ? ctx.bool_sort() : args.front().sort();
                return ctx.make_term(*builtin, result, args);
            }
            auto it = symbols.find(n.name);
            if (it == symbols.end()) return std::unexpected("unknown function: " + std::string(spelling));
            std::vector<Term> all{it->second};
            all.insert(all.end(), args.begin(), args.end());
            return ctx.make_term(Op::Apply, ctx.bool_sort(), all);
        };
        lowering_result result;
        for (auto command : script.commands) {
            const auto& n = script.nodes[command];
            const auto c = script.nodes.children(command);
            if (n.kind == ir::kind::declare_const) {
                if (c.size() != 1)return std::unexpected("declare-const needs a sort");
                auto sort = detail::lower_sort(script, ctx, c[0]);
                if (!sort)return std::unexpected(sort.error());
                symbols[n.name] = ctx.make_symbol(script.symbols.get(n.name), *sort);
            }
            else if (n.kind == ir::kind::assert_) {
                if (c.size() != 1)return std::unexpected("assert needs one term");
                auto t = term(c[0]);
                if (!t)return std::unexpected(t.error());
                solver.assert_formula(*t);
                result.assertions.push_back(*t);
            }
            else if (n.kind == ir::kind::check_sat) result.last_result = solver.check_sat();
            else if (n.kind == ir::kind::push) solver.push();
            else if (n.kind == ir::kind::pop) solver.pop();
        }
        return result;
    }
} // namespace tarka::frontend
