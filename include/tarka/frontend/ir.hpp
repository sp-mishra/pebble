#pragma once

// Tarka SMT front-end IR.  Syntax producers depend on this header; lowering to
// Context/RouterEngine is deliberately separate.

#include "languages/generic/ir/ir_module.hpp"
#include "languages/generic/tree/spans.hpp"
#include "languages/generic/core/diagnostics.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace tarka::frontend::ir {
    enum class kind : std::uint8_t {
        script, set_logic, declare_const, declare_fun, assert_, check_sat, push, pop, exit_,
        sort_bool, sort_int, sort_real, sort_string, sort_bitvec, sort_array, sort_function,
        symbol, bool_literal, int_literal, real_literal, bv_literal, application
    };

    struct bit_vector { std::uint64_t bits = 0; std::uint32_t width = 0; };
    struct payload {
        std::variant<std::monostate, std::int64_t, double, bit_vector, std::uint32_t> value{};
    };

    using module = lang::ir_module<kind, payload>;
    using node_id = lang::ir_node_id;

    // Owns spellings used by generic ir_node::name.  The generic ir interner is
    // intentionally not used because it currently has no id-to-spelling query.
    class names {
    public:
        [[nodiscard]] lang::symbol_id intern(std::string_view text) {
            if (auto it = ids_.find(std::string(text)); it != ids_.end()) return it->second;
            const auto id = static_cast<lang::symbol_id>(spellings_.size() + 1);
            spellings_.emplace_back(text); ids_.emplace(spellings_.back(), id); return id;
        }
        [[nodiscard]] std::string_view get(lang::symbol_id id) const noexcept {
            return id == 0 || id > spellings_.size() ? std::string_view{} : spellings_[id - 1];
        }
    private:
        std::vector<std::string> spellings_;
        std::unordered_map<std::string, lang::symbol_id> ids_;
    };

    enum class diagnostic_code : std::uint8_t { syntax, unsupported, unknown_symbol, sort, arity };
    [[nodiscard]] constexpr std::string_view to_code(diagnostic_code c) noexcept {
        switch (c) {
        case diagnostic_code::syntax: return "TARKA-SMT-SYNTAX";
        case diagnostic_code::unsupported: return "TARKA-SMT-UNSUPPORTED";
        case diagnostic_code::unknown_symbol: return "TARKA-SMT-UNKNOWN-SYMBOL";
        case diagnostic_code::sort: return "TARKA-SMT-SORT";
        case diagnostic_code::arity: return "TARKA-SMT-ARITY";
        } return "TARKA-SMT";
    }
    struct diagnostic : lang::lang_diagnostic<diagnostic_code> { lang::byte_span span{}; };

    struct script {
        module nodes;
        names symbols;
        std::vector<node_id> commands;
        lang::collecting_sink<diagnostic> diagnostics;
        [[nodiscard]] bool valid() const noexcept { return !diagnostics.has_errors(); }
    };

    [[nodiscard]] inline node_id append(script& s, kind k, lang::byte_span span = {},
                                        lang::symbol_id name = 0, payload data = {},
                                        std::span<const node_id> children = {}) {
        node_id id = s.nodes.push({k, span, lang::k_null_ir, 0, 0, name, std::move(data)});
        if (!children.empty()) s.nodes.append_children(id, children);
        return id;
    }
    inline void error(script& s, diagnostic_code code, lang::byte_span span, std::string message) {
        diagnostic d; d.kind = code; d.span = span; d.message = std::move(message); s.diagnostics.on_diagnostic(std::move(d));
    }
} // namespace tarka::frontend::ir
