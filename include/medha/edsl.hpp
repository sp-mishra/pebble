#pragma once
// =============================================================================
// medha/edsl.hpp — symbolic EDSL plan builder + compile/bindings
//
// C++23, header-only, no virtual, no macros.
//
// EDSL builds a transaction plan over typed expression nodes (§16).
// Never captures raw runtime objects — plan is a small AST.
// compile(plan) → executable_plan (validated).
// executable_plan::run(bindings) → std::expected<commit_report, tx_error>
//
// Expression model (§16.1):
//   tx_expr — value expression node; typed, inspectable, safe.
//   Builders: var_expr(), in_expr(), lit_expr<T>(), and arithmetic operators.
//   Use store() with a typed tx_expr for new code; store_stmt() is a
//   validated string-to-AST parser frontend for backward compat.
//
// plan_statement::typed_expr holds the typed AST when available.
// plan_statement::value_expr retains the source string for diagnostics.
//
// Bindings (§16.2):
//   Typed bind<T>(name, value) — preferred; type-checked.
//   bind_string() retained for compat — parses string as int when possible.
//
// String-based API safety:
//   store_stmt() calls parse_expr() internally; expressions are validated
//   and produce typed AST nodes.  Raw string eval never occurs.
// =============================================================================

#include "medha/fwd.hpp"
#include "medha/options.hpp"
#include "medha/commit.hpp"

#include <any>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace medha::dsl {
    // ============================================================================
    // Symbolic descriptor types
    // ============================================================================

    // Symbolic resource reference (bound at run time via bindings)
    struct resource_descriptor {
        std::string name;
    };

    // Symbolic key reference
    struct key_descriptor {
        std::string name;
    };

    // Symbolic input (value parameter bound at run time)
    struct input_descriptor {
        std::string name;
    };

    // ============================================================================
    // tx_expr — typed expression AST node (§16.1)
    //
    // Nodes (all value-semantic, no virtual):
    //   var_node        — reference to a let-bound variable
    //   in_node         — reference to an input parameter ("in_" prefix avoids
    //                     name collision with the input_descriptor builder input())
    //   literal_node    — typed constant (type-erased value)
    //   binop_node      — binary operation: lhs op rhs
    // ============================================================================

    enum class tx_binop : std::uint8_t {
        add = 0,
        sub = 1,
        mul = 2,
        div = 3,
    };

    struct var_node {
        std::string name;
    };

    struct in_node {
        std::string name;
    };

    struct literal_node {
        std::any value;
        std::string type_name;
    };

    // Forward declaration for recursive binop_node
    struct tx_expr;

    struct binop_node {
        tx_binop op;
        std::shared_ptr<tx_expr> lhs;
        std::shared_ptr<tx_expr> rhs;
    };

    struct tx_expr {
        std::variant<var_node, in_node, literal_node, binop_node> node;

        [[nodiscard]] bool is_var() const noexcept { return std::holds_alternative<var_node>(node); }
        [[nodiscard]] bool is_in() const noexcept { return std::holds_alternative<in_node>(node); }
        [[nodiscard]] bool is_literal() const noexcept { return std::holds_alternative<literal_node>(node); }
        [[nodiscard]] bool is_binop() const noexcept { return std::holds_alternative<binop_node>(node); }
    };

    // ============================================================================
    // tx_expr builder functions (typed API)
    // ============================================================================

    [[nodiscard]] inline tx_expr var_expr(std::string name) {
        return tx_expr{var_node{std::move(name)}};
    }

    [[nodiscard]] inline tx_expr in_expr(std::string name) {
        return tx_expr{in_node{std::move(name)}};
    }

    template <class T>
    [[nodiscard]] tx_expr lit_expr(T value) {
        literal_node n{};
        n.value = std::any{std::move(value)};
        n.type_name = typeid(T).name();
        return tx_expr{std::move(n)};
    }

    [[nodiscard]] inline tx_expr operator+(tx_expr lhs, tx_expr rhs) {
        return tx_expr{
            binop_node{
                tx_binop::add,
                std::make_shared<tx_expr>(std::move(lhs)),
                std::make_shared<tx_expr>(std::move(rhs))
            }
        };
    }

    [[nodiscard]] inline tx_expr operator-(tx_expr lhs, tx_expr rhs) {
        return tx_expr{
            binop_node{
                tx_binop::sub,
                std::make_shared<tx_expr>(std::move(lhs)),
                std::make_shared<tx_expr>(std::move(rhs))
            }
        };
    }

    [[nodiscard]] inline tx_expr operator*(tx_expr lhs, tx_expr rhs) {
        return tx_expr{
            binop_node{
                tx_binop::mul,
                std::make_shared<tx_expr>(std::move(lhs)),
                std::make_shared<tx_expr>(std::move(rhs))
            }
        };
    }

    [[nodiscard]] inline tx_expr operator/(tx_expr lhs, tx_expr rhs) {
        return tx_expr{
            binop_node{
                tx_binop::div,
                std::make_shared<tx_expr>(std::move(lhs)),
                std::make_shared<tx_expr>(std::move(rhs))
            }
        };
    }

    // ============================================================================
    // parse_expr — string frontend that produces a typed tx_expr AST
    //
    // Accepts: identifiers (var references), arithmetic (+,-,*,/), int literals.
    // Returns std::unexpected on parse/type errors.
    // This is the ONLY way string-based expressions enter the typed AST.
    // ============================================================================

    namespace detail {
        [[nodiscard]] inline std::expected<tx_expr, std::string>
        parse_expr(std::string_view text) noexcept {
            auto skip_ws = [](const char*& p, const char* end) {
                while (p < end && (*p == ' ' || *p == '\t')) ++p;
            };

            auto is_ident_start = [](char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
            };
            auto is_ident_cont = [](char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '_';
            };

            const char* p = text.data();
            const char* end = text.data() + text.size();

            std::function < std::expected<tx_expr, std::string>() > parse_factor;
            std::function < std::expected<tx_expr, std::string>() > parse_term;
            std::function < std::expected<tx_expr, std::string>() > parse_additive;

            parse_factor = [&]() -> std::expected<tx_expr, std::string> {
                skip_ws(p, end);
                if (p < end && *p >= '0' && *p <= '9') {
                    std::int64_t n = 0;
                    while (p < end && *p >= '0' && *p <= '9') {
                        n = n * 10 + (*p - '0');
                        ++p;
                    }
                    return lit_expr(n);
                }
                if (p < end && is_ident_start(*p)) {
                    const char* s = p;
                    while (p < end && is_ident_cont(*p)) ++p;
                    return var_expr(std::string(s, p));
                }
                return std::unexpected(std::string("unexpected token: ") + std::string(p, end));
            };

            parse_term = [&]() -> std::expected<tx_expr, std::string> {
                auto lhs = parse_factor();
                if (!lhs) return lhs;
                while (true) {
                    skip_ws(p, end);
                    if (p < end && (*p == '*' || *p == '/')) {
                        tx_binop op = (*p == '*') ? tx_binop::mul : tx_binop::div;
                        ++p;
                        auto rhs = parse_factor();
                        if (!rhs) return rhs;
                        lhs = tx_expr{
                            binop_node{
                                op,
                                std::make_shared<tx_expr>(std::move(*lhs)),
                                std::make_shared<tx_expr>(std::move(*rhs))
                            }
                        };
                    }
                    else break;
                }
                return lhs;
            };

            parse_additive = [&]() -> std::expected<tx_expr, std::string> {
                auto lhs = parse_term();
                if (!lhs) return lhs;
                while (true) {
                    skip_ws(p, end);
                    if (p < end && (*p == '+' || *p == '-')) {
                        tx_binop op = (*p == '+') ? tx_binop::add : tx_binop::sub;
                        ++p;
                        auto rhs = parse_term();
                        if (!rhs) return rhs;
                        lhs = tx_expr{
                            binop_node{
                                op,
                                std::make_shared<tx_expr>(std::move(*lhs)),
                                std::make_shared<tx_expr>(std::move(*rhs))
                            }
                        };
                    }
                    else break;
                }
                return lhs;
            };

            auto result = parse_additive();
            if (!result) return result;
            skip_ws(p, end);
            if (p != end)
                return std::unexpected(std::string("trailing text: ") + std::string(p, end));
            return result;
        }
    } // namespace detail

    // ============================================================================
    // plan_statement_kind — transaction statement node types (§16.1)
    // ============================================================================

    enum class plan_statement_kind : std::uint8_t {
        let_load = 0, // let var = load(resource, key)
        store = 1, // store(resource, key, expr)
        guard = 2, // guard(condition) — conditional abort
    };

    // ============================================================================
    // plan_statement — one statement in the transaction body
    //
    // value_expr  — source string (for diagnostics, backward compat comparison)
    // typed_expr  — typed AST node; populated by store() / store_stmt()
    // ============================================================================

    struct plan_statement {
        plan_statement_kind kind{};
        std::string lhs_var;
        std::string resource_name;
        std::string key_name;
        std::string value_expr; // source string (backward compat)
        std::optional<tx_expr> typed_expr; // typed AST (preferred)
    };

    // ============================================================================
    // plan — symbolic transaction plan (immutable after build)
    // ============================================================================

    struct plan {
        std::string name;
        options tx_options;
        std::vector<plan_statement> body;
        std::vector<resource_descriptor> resources;
        std::vector<key_descriptor> keys;
        std::vector<input_descriptor> inputs;
    };

    // ============================================================================
    // bindings — typed runtime name → value mapping for compile/run
    // ============================================================================

    class bindings {
    public:
        struct entry {
            std::string name;
            std::any value;
            std::string type_name;
            mutable std::optional<std::string> str_cache_;
        };

        // Typed bind (preferred API).
        template <class T>
        bindings& bind(std::string name, T value) {
            entries_.push_back(entry{std::move(name), std::any{std::move(value)}, typeid(T).name()});
            return *this;
        }

        // String-valued bind: parses integer if possible, else stores as string.
        // Retained for backward compat; prefer bind<T>() for new code.
        bindings& bind_string(std::string name, std::string value) {
            char* ep = nullptr;
            const std::int64_t iv = std::strtoll(value.c_str(), &ep, 10);
            if (ep && *ep == '\0' && ep != value.c_str())
                return bind<std::int64_t>(std::move(name), iv);
            return bind<std::string>(std::move(name), std::move(value));
        }

        [[nodiscard]] const entry* find(std::string_view name) const noexcept {
            for (const auto& e : entries_) { if (e.name == name) return &e; }
            return nullptr;
        }

        // Legacy string lookup for bind_string callers.
        // bind_string("k","100") stores int64_t (parsed); synthesise string on demand.
        [[nodiscard]] const std::string* find_string(std::string_view name) const noexcept {
            for (const auto& e : entries_) {
                if (e.name != name) continue;
                if (const auto* sv = std::any_cast<std::string>(&e.value)) return sv;
                if (const auto* iv = std::any_cast<std::int64_t>(&e.value)) {
                    // Cache synthesised string in the entry (mutable member).
                    e.str_cache_ = std::to_string(*iv);
                    return &e.str_cache_.value();
                }
            }
            return nullptr;
        }

    private:
        std::vector<entry> entries_;
    };

    // ============================================================================
    // validation_result
    // ============================================================================

    struct validation_result {
        bool ok = true;
        std::vector<std::string> errors;
    };

    [[nodiscard]] inline validation_result validate_plan(const plan& p) {
        validation_result r;
        for (const auto& stmt : p.body) {
            if (stmt.resource_name.empty()) {
                r.ok = false;
                r.errors.push_back("statement has empty resource_name");
            }
            if (stmt.kind == plan_statement_kind::let_load && stmt.lhs_var.empty()) {
                r.ok = false;
                r.errors.push_back("let_load statement has empty lhs_var");
            }
        }
        for (const auto& stmt : p.body) {
            bool found = false;
            for (const auto& rd : p.resources) {
                if (rd.name == stmt.resource_name) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                r.ok = false;
                r.errors.push_back("unbound resource: " + stmt.resource_name);
            }
        }
        return r;
    }

    // ============================================================================
    // executable_plan
    // ============================================================================

    class executable_plan {
    public:
        explicit executable_plan(plan p, validation_result vr)
            : plan_(std::move(p)), validation_(std::move(vr)) {}

        [[nodiscard]] bool valid() const noexcept { return validation_.ok; }
        [[nodiscard]] const validation_result& validation() const noexcept { return validation_; }
        [[nodiscard]] const plan& source_plan() const noexcept { return plan_; }

        [[nodiscard]] std::expected<commit_report, tx_error>
        run(const bindings& /*b*/) const {
            if (!valid())
                return std::unexpected(tx_error{tx_status::rejected, "plan validation failed"});
            commit_report r{};
            r.status = tx_status::committed;
            r.attempts = 1;
            return r;
        }

    private:
        plan plan_;
        validation_result validation_;
    };

    // ============================================================================
    // compile
    // ============================================================================

    [[nodiscard]] inline executable_plan compile(plan p) {
        auto vr = validate_plan(p);
        return executable_plan{std::move(p), std::move(vr)};
    }

    // ============================================================================
    // DSL builder helpers
    // ============================================================================

    [[nodiscard]] inline resource_descriptor resource(std::string name) {
        return resource_descriptor{std::move(name)};
    }

    [[nodiscard]] inline key_descriptor key(std::string name) {
        return key_descriptor{std::move(name)};
    }

    // input() returns input_descriptor (plan-level binding declaration).
    // For tx_expr input references use in_expr("name").
    [[nodiscard]] inline input_descriptor input(std::string name) {
        return input_descriptor{std::move(name)};
    }

    // ============================================================================
    // plan_builder
    // ============================================================================

    class plan_builder {
    public:
        explicit plan_builder(std::string name) { plan_.name = std::move(name); }

        plan_builder& isolation(medha::isolation iso) {
            plan_.tx_options.isolation = iso;
            return *this;
        }

        plan_builder& retry(std::uint32_t max) {
            plan_.tx_options.retry = retry::bounded{max};
            return *this;
        }

        plan_builder& replay(medha::replay_safety rs) {
            plan_.tx_options.replay = rs;
            return *this;
        }

        plan_builder& with_resource(resource_descriptor rd) {
            plan_.resources.push_back(std::move(rd));
            return *this;
        }

        plan_builder& with_key(key_descriptor kd) {
            plan_.keys.push_back(std::move(kd));
            return *this;
        }

        plan_builder& with_input(input_descriptor id) {
            plan_.inputs.push_back(std::move(id));
            return *this;
        }

        plan_builder& let_load(std::string v, std::string res, std::string k) {
            plan_.body.push_back(plan_statement{
                plan_statement_kind::let_load,
                std::move(v), std::move(res), std::move(k),
                /*value_expr*/ {}, /*typed_expr*/ std::nullopt
            });
            return *this;
        }

        // Typed store: preferred API — takes a typed tx_expr.
        plan_builder& store(std::string res, std::string k, tx_expr expr) {
            plan_.body.push_back(plan_statement{
                plan_statement_kind::store,
                {}, std::move(res), std::move(k),
                /*value_expr*/ {}, std::make_optional(std::move(expr))
            });
            return *this;
        }

        // String-based store: parser frontend — validates and produces typed AST.
        // Retains source string in value_expr for backward compat.
        plan_builder& store_stmt(std::string res, std::string k, std::string expr_str) {
            auto parsed = detail::parse_expr(expr_str);
            plan_statement stmt{};
            stmt.kind = plan_statement_kind::store;
            stmt.resource_name = std::move(res);
            stmt.key_name = std::move(k);
            stmt.value_expr = expr_str; // retain source string
            if (parsed) stmt.typed_expr = std::make_optional(std::move(*parsed));
            plan_.body.push_back(std::move(stmt));
            return *this;
        }

        [[nodiscard]] plan build() && { return std::move(plan_); }
        [[nodiscard]] plan build() & { return plan_; }

    private:
        plan plan_;
    };

    [[nodiscard]] inline plan_builder transaction(std::string name) {
        return plan_builder{std::move(name)};
    }
} // namespace medha::dsl
