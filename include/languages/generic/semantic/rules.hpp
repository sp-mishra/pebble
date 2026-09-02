#pragma once

// generic/rules.hpp — Generic rule engine with EasyRules bridge + proof obligation bridge.
//
// C++23, header-only, no virtual, no macros. Namespace: lang
//
// Two complementary rule systems:
//
//   1. Structural rule_engine (compile-time / resolution-time)
//      Checks structural constraints between modules and symbols:
//      requires_ / conflicts / implies / capability_gate / version_constraint.
//      Lives in this header, no runtime state.
//      Rules with generates_obligation = true bridge to proof.hpp.
//
//   2. easy_rules_bridge (runtime fact-based rules)
//      Thin adapter over easy_rules::EasyRuleEngine (rules/easy_rules.hpp).
//      Lets language frontends register runtime rules that fire against an
//      easy_rules::ExecutionContext during module execution / analysis.
//      The EasyRuleEngine provides: type-safe Facts store, fluent when/then DSL,
//      CRTP listeners (AuditListener, EnhancedAuditListener), std::expected errors.
//
// Include flow:
//   rules/easy_rules.hpp   — EasyRuleEngine, Facts, ExecutionContext, RuleListener<>
//   generic/proof.hpp      — obligation_record, discharge_driver
//   generic/symbol_table.hpp / module_system.hpp — for structural checks
//
// Depends on: generic/identity.hpp, generic/diagnostics.hpp,
//             generic/module_system.hpp, generic/symbol_table.hpp,
//             generic/proof.hpp, rules/easy_rules.hpp
//
// Usage — structural:
//   lang::rule_engine engine;
//   lang::rule_descriptor r;
//   r.kind = lang::rule_kind::requires_; r.subject = "tx"; r.object = "io";
//   engine.add_rule(r);
//   auto result = engine.check(adapter, dep_graph, caps_map);
//
// Usage — runtime (EasyRules):
//   lang::easy_rules_bridge bridge;
//   bridge.when("hot", easy_rules::dsl::fact<int>("temp") > 30)
//         .then([](easy_rules::ExecutionContext& ctx) {
//             ctx.facts.set("alert", std::string("hot"));
//         });
//   easy_rules::ExecutionContext ctx;
//   ctx.facts.set("temp", 35);
//   bridge.run(ctx);

#include "languages/generic/core/identity.hpp"
#include "languages/generic/core/diagnostics.hpp"
#include "languages/generic/module/module_system.hpp"
#include "languages/generic/semantic/symbol_table.hpp"
#include "languages/generic/semantic/proof.hpp"
#include "rules/easy_rules.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace lang {
    // =========================================================================
    // rule_kind
    // =========================================================================

    enum class rule_kind : std::uint8_t {
        requires_, // subject requires object to be active
        conflicts, // subject and object are mutually exclusive
        implies, // subject's presence forces object to be present
        capability_gate, // subject requires a specific capability mask
        version_constraint, // subject requires object's version in [min, max]
    };

    [[nodiscard]] constexpr std::string_view to_string(rule_kind k) noexcept {
        switch (k) {
        case rule_kind::requires_: return "requires";
        case rule_kind::conflicts: return "conflicts";
        case rule_kind::implies: return "implies";
        case rule_kind::capability_gate: return "capability_gate";
        case rule_kind::version_constraint: return "version_constraint";
        }
        return "unknown";
    }

    // =========================================================================
    // rule_descriptor
    // =========================================================================

    struct rule_descriptor {
        stable_rule_id id;
        rule_kind kind = rule_kind::requires_;
        std::string subject; // module / symbol name
        std::string object; // dependency / conflicting symbol

        // For capability_gate:
        std::uint64_t required_capability_mask = 0;

        // For version_constraint:
        version_triple min_version{0, 0, 0};
        version_triple max_version{255, 255, 255};

        // If true, a violated rule emits an obligation_record for proof.hpp discharge.
        bool generates_obligation = false;
        std::string obligation_description;

        // Error code emitted on violation (e.g. "LANG-RULE-001").
        std::string diagnostic_code;
        std::string message_template; // {subject} and {object} placeholders replaced
    };

    // =========================================================================
    // rule_diagnostic_kind
    // =========================================================================

    struct rule_diag_kind {
        enum class kind : std::uint8_t {
            requires_violated, // LANG-RULE-001
            conflicts_violated, // LANG-RULE-002
            capability_missing, // LANG-RULE-003
            version_out_of_range, // LANG-RULE-004
        };

        kind value = kind::requires_violated;

        constexpr rule_diag_kind() = default;
        constexpr rule_diag_kind(kind k) noexcept : value(k) {}

        [[nodiscard]] static constexpr std::string_view to_code(rule_diag_kind k) noexcept {
            switch (k.value) {
            case kind::requires_violated: return "LANG-RULE-001";
            case kind::conflicts_violated: return "LANG-RULE-002";
            case kind::capability_missing: return "LANG-RULE-003";
            case kind::version_out_of_range: return "LANG-RULE-004";
            }
            return "LANG-RULE-000";
        }

        [[nodiscard]] constexpr bool operator==(const rule_diag_kind&) const noexcept = default;
    };

    using rule_diagnostic = lang_diagnostic<rule_diag_kind>;

    // =========================================================================
    // module_capabilities_map — capability mask per module name
    // =========================================================================

    using module_capabilities_map = std::unordered_map<std::string, std::uint64_t>;

    // =========================================================================
    // symbol_table_view_adapter — non-owning read-only view for rule checking.
    // Template-based (no virtual) — pass to rule_engine::check().
    // =========================================================================

    template <visibility_policy P>
    class symbol_table_view_adapter {
    public:
        explicit symbol_table_view_adapter(const symbol_table<P>& tbl) : tbl_(tbl) {}

        [[nodiscard]] bool has_symbol(std::string_view name) const noexcept {
            return tbl_.lookup(name) != nullptr;
        }

    private:
        const symbol_table<P>& tbl_;
    };

    // =========================================================================
    // rule_engine
    // =========================================================================

    class rule_engine {
    public:
        void add_rule(rule_descriptor r) {
            rules_.push_back(std::move(r));
        }

        [[nodiscard]] std::size_t rule_count() const noexcept { return rules_.size(); }

        struct check_result {
            std::vector<rule_descriptor> violated;
            std::vector<obligation_record> obligations; // for proof.hpp
            std::vector<rule_diagnostic> diagnostics;

            [[nodiscard]] bool ok() const noexcept { return violated.empty(); }
        };

        // Check all rules given:
        //   active_symbols — names currently in scope (module + imported)
        //   graph          — module dependency graph (for version checks)
        //   caps_map       — capability mask per active module
        template <visibility_policy P>
        [[nodiscard]] check_result
        check(const symbol_table_view_adapter<P>& symbols,
              const dependency_graph& graph,
              const module_capabilities_map& caps_map) const {
            check_result result;

            for (const auto& r : rules_) {
                bool subject_active = symbols.has_symbol(r.subject) ||
                    (graph.find(r.subject) != nullptr);

                if (!subject_active) continue; // rule only applies when subject is active

                bool violated = false;
                rule_diag_kind diag_kind = rule_diag_kind{rule_diag_kind::kind::requires_violated};

                switch (r.kind) {
                case rule_kind::requires_: {
                    bool object_present = symbols.has_symbol(r.object) ||
                        (graph.find(r.object) != nullptr);
                    if (!object_present) {
                        violated = true;
                        diag_kind = rule_diag_kind{rule_diag_kind::kind::requires_violated};
                    }
                    break;
                }

                case rule_kind::conflicts: {
                    bool object_present = symbols.has_symbol(r.object) ||
                        (graph.find(r.object) != nullptr);
                    if (object_present) {
                        violated = true;
                        diag_kind = rule_diag_kind{rule_diag_kind::kind::conflicts_violated};
                    }
                    break;
                }

                case rule_kind::implies: {
                    // Implies: if subject is active, object must also be active.
                    bool object_present = symbols.has_symbol(r.object) ||
                        (graph.find(r.object) != nullptr);
                    if (!object_present) {
                        violated = true;
                        diag_kind = rule_diag_kind{rule_diag_kind::kind::requires_violated};
                    }
                    break;
                }

                case rule_kind::capability_gate: {
                    // Find the capability mask for the subject module.
                    auto it = caps_map.find(r.subject);
                    std::uint64_t mask = (it != caps_map.end()) ? it->second : 0;
                    if ((mask & r.required_capability_mask) != r.required_capability_mask) {
                        violated = true;
                        diag_kind = rule_diag_kind{rule_diag_kind::kind::capability_missing};
                    }
                    break;
                }

                case rule_kind::version_constraint: {
                    const auto* desc = graph.find(r.object);
                    if (desc) {
                        // module_descriptor::version is lang::version_triple
                        const auto& v = desc->version;
                        if (!(r.min_version <= v && v <= r.max_version)) {
                            violated = true;
                            diag_kind = rule_diag_kind{rule_diag_kind::kind::version_out_of_range};
                        }
                    }
                    break;
                }
                }

                if (violated) {
                    result.violated.push_back(r);

                    rule_diagnostic d;
                    d.kind = diag_kind;
                    d.symbol = r.subject;
                    d.message = r.message_template.empty()
                                    ? ("rule violated: " + r.subject + " / " + r.object)
                                    : r.message_template;
                    d.level = severity::error;
                    result.diagnostics.push_back(std::move(d));

                    if (r.generates_obligation) {
                        obligation_record ob;
                        ob.description = r.obligation_description.empty()
                                             ? ("rule: " + r.subject + " " + std::string(to_string(r.kind)) + " " + r.
                                                 object)
                                             : r.obligation_description;
                        ob.kind = proof_construct_kind::proof;
                        ob.policy = verify_policy::check;
                        result.obligations.push_back(std::move(ob));
                    }
                }
            }

            return result;
        }

        [[nodiscard]] const std::vector<rule_descriptor>& rules() const noexcept {
            return rules_;
        }

    private:
        std::vector<rule_descriptor> rules_;
    };

    // =========================================================================
    // easy_rules_bridge — runtime fact-based rule engine adapter.
    //
    // Wraps easy_rules::EasyRuleEngine providing the full EasyRules surface:
    //   when(name, predicate).then(action)  — fluent rule registration
    //   run(ctx)                             — evaluate all rules against facts
    //   add_listener(listener)              — CRTP-based hooks (no virtual)
    //   AuditListener / EnhancedAuditListener built-in via easy_rules::
    //
    // Use alongside rule_engine for:
    //   rule_engine       → compile-time structural constraints (requires/conflicts)
    //   easy_rules_bridge → runtime business rules that fire against Facts
    // =========================================================================

    class easy_rules_bridge {
    public:
        // Fluent rule registration — delegates directly to EasyRuleEngine.
        template <class Predicate>
        auto& when(std::string name, Predicate pred) {
            return engine_.when(std::move(name), std::move(pred));
        }

        void run(easy_rules::ExecutionContext& ctx) {
            engine_.run(ctx);
        }

        template <class Listener>
        void add_listener(Listener& listener) {
            engine_.add_listener(listener);
        }

        [[nodiscard]] std::size_t rule_count() const noexcept {
            return engine_.get_all_rules().size();
        }

        // Direct access for advanced use (AuditListener queries, etc.).
        [[nodiscard]] easy_rules::EasyRuleEngine& engine() noexcept { return engine_; }
        [[nodiscard]] const easy_rules::EasyRuleEngine& engine() const noexcept { return engine_; }

    private:
        easy_rules::EasyRuleEngine engine_;
    };
} // namespace lang
