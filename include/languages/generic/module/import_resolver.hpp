#pragma once

// generic/import_resolver.hpp — Full import semantics with symbol flow.
//
// C++23, header-only, no virtual, no macros. Namespace: lang
//
// Implements the complete import pipeline:
//   1. Circular import detection (LANG-IMP-003)
//   2. Topological sort (compile order: importees before importers)
//   3. Version constraint checks (LANG-IMP-004)
//   4. Capability gate checks (LANG-IMP-005)
//   5. Symbol flow: exported symbols from each importee flow into the
//      importer's symbol_table via a user-supplied symbol_provider callback.
//
// Depends on: generic/module_system.hpp, generic/symbol_table.hpp,
//             generic/diagnostics.hpp, generic/rules.hpp
//
// import_spec          — one import statement with optional version/capability constraints
// resolved_import      — result of resolving one import_spec
// import_error         — structured error for import failures
// import_graph         — declare_imports() + resolve() → resolve_result
//
// Error codes:
//   LANG-IMP-001  module not found
//   LANG-IMP-002  import resolution failed (I/O or resolver error)
//   LANG-IMP-003  circular import detected
//   LANG-IMP-004  version constraint not satisfied
//   LANG-IMP-005  required capability not available
//
// Usage:
//   lang::import_graph graph;
//   graph.declare_imports("app", {
//       {.module_name = "math", .min_version = {1,0,0}},
//       {.module_name = "util"},
//   });
//   graph.declare_imports("math", {
//       {.module_name = "core"},
//   });
//
//   auto result = graph.resolve(resolver, caps_map);
//   if (!result.ok()) { /* handle errors */ }
//   // result.compile_order = ["core", "util", "math", "app"]

#include "languages/generic/module/module_system.hpp"
#include "languages/generic/semantic/symbol_table.hpp"
#include "languages/generic/core/diagnostics.hpp"
#include "languages/generic/semantic/rules.hpp"

#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace lang {

    // =========================================================================
    // import_spec — one import statement
    // =========================================================================

    struct import_spec {
        std::string    module_name;
        version_triple min_version{0, 0, 0};        // version constraint lower bound
        version_triple max_version{255, 255, 255};   // version constraint upper bound
        std::uint64_t  required_capabilities = 0;   // capability gate (0 = no gate)
        bool           is_optional = false;          // if true, not-found is a warning
    };

    // =========================================================================
    // resolved_import — one successfully resolved import
    // =========================================================================

    struct resolved_import {
        module_descriptor           desc;
        std::vector<symbol_entry>   exported_symbols; // symbols flowing into importer
        bool                        was_cached = false;
    };

    // =========================================================================
    // import_error — structured failure for one import
    // =========================================================================

    struct import_error_kind {
        enum class kind : std::uint8_t {
            not_found,           // LANG-IMP-001
            resolution_failed,   // LANG-IMP-002
            circular,            // LANG-IMP-003
            version_mismatch,    // LANG-IMP-004
            capability_mismatch, // LANG-IMP-005
        };
        kind value = kind::not_found;

        constexpr import_error_kind() = default;
        constexpr import_error_kind(kind k) noexcept : value(k) {}

        [[nodiscard]] static constexpr std::string_view to_code(import_error_kind k) noexcept {
            switch (k.value) {
            case kind::not_found:           return "LANG-IMP-001";
            case kind::resolution_failed:   return "LANG-IMP-002";
            case kind::circular:            return "LANG-IMP-003";
            case kind::version_mismatch:    return "LANG-IMP-004";
            case kind::capability_mismatch: return "LANG-IMP-005";
            }
            return "LANG-IMP-000";
        }

        [[nodiscard]] constexpr bool operator==(const import_error_kind&) const noexcept = default;
    };

    struct import_error {
        import_error_kind kind;
        std::string       module_name;
        std::string       message;
        std::string       code;
    };

    using import_diagnostic = lang_diagnostic<import_error_kind>;

    // =========================================================================
    // symbol_provider — callback that returns exported symbols for a module.
    // The import_graph calls this during resolve() for each importee so its
    // symbols can flow into importers.
    //
    // Signature: std::vector<symbol_entry>(std::string_view module_name)
    // =========================================================================

    using symbol_provider = std::function<std::vector<symbol_entry>(std::string_view)>;

    // =========================================================================
    // import_graph
    // =========================================================================

    class import_graph {
    public:
        // Declare the imports of a module.
        void declare_imports(std::string_view module_name,
                             std::vector<import_spec> specs) {
            imports_[std::string(module_name)] = std::move(specs);
        }

        // Declare a single import.
        void add_import(std::string_view importer, import_spec spec) {
            imports_[std::string(importer)].push_back(std::move(spec));
        }

        struct resolve_result {
            std::vector<std::string>   compile_order; // topo-sorted (importees first)
            std::vector<resolved_import> resolved;
            std::vector<import_error>  errors;
            std::vector<import_diagnostic> diagnostics;

            [[nodiscard]] bool ok() const noexcept { return errors.empty(); }
        };

        // Resolve all declared imports:
        //   - detect cycles
        //   - topo-sort
        //   - check version constraints
        //   - check capability gates
        //   - flow exported symbols via symbol_provider
        //
        // resolver       — 9-tier module resolver
        // caps_map       — capability mask per module name
        // sym_provider   — optional; called per resolved module to get its exports
        [[nodiscard]] resolve_result
        resolve(const module_resolver& resolver,
                const module_capabilities_map& caps_map = {},
                const symbol_provider& sym_provider = {}) const {
            resolve_result result;

            // Build adjacency for cycle detection + topo sort.
            // Nodes = all declared importers + all imported modules.
            std::unordered_map<std::string, std::vector<std::string>> adj;
            std::unordered_set<std::string> all_nodes;

            for (const auto& [importer, specs] : imports_) {
                all_nodes.insert(importer);
                for (const auto& s : specs) {
                    all_nodes.insert(s.module_name);
                    adj[importer].push_back(s.module_name);
                }
            }

            // 1. Cycle detection (DFS coloring).
            {
                std::unordered_map<std::string, int> color;
                for (const auto& n : all_nodes) color[n] = 0;
                std::vector<std::string> path;
                bool found_cycle = false;

                std::function<void(const std::string&)> dfs =
                    [&](const std::string& n) {
                    if (found_cycle) return;
                    color[n] = 1;
                    path.push_back(n);
                    if (auto it = adj.find(n); it != adj.end()) {
                        for (const auto& dep : it->second) {
                            if (color[dep] == 1) {
                                // Cycle: collect participating modules.
                                auto start = std::find(path.begin(), path.end(), dep);
                                std::string cycle_str;
                                for (auto ci = start; ci != path.end(); ++ci) {
                                    if (!cycle_str.empty()) cycle_str += " → ";
                                    cycle_str += *ci;
                                }
                                cycle_str += " → " + dep;

                                import_error e;
                                e.kind        = import_error_kind{import_error_kind::kind::circular};
                                e.module_name = dep;
                                e.message     = "circular import detected: " + cycle_str;
                                e.code        = std::string(import_error_kind::to_code(
                                                    import_error_kind{import_error_kind::kind::circular}));
                                result.errors.push_back(std::move(e));
                                found_cycle = true;
                                return;
                            }
                            if (color[dep] == 0) dfs(dep);
                        }
                    }
                    path.pop_back();
                    color[n] = 2;
                };

                for (const auto& n : all_nodes)
                    if (color[n] == 0) dfs(n);

                if (found_cycle) return result;
            }

            // 2. Topo sort (Kahn's — importees first).
            {
                std::unordered_map<std::string, int> in_deg;
                for (const auto& n : all_nodes) in_deg[n] = 0;
                for (const auto& [n, deps] : adj)
                    for ([[maybe_unused]] const auto& dep : deps)
                        in_deg[n]++;

                // Reverse adjacency for Kahn's (importee → importers).
                std::unordered_map<std::string, std::vector<std::string>> rev;
                for (const auto& [importer, deps] : adj)
                    for (const auto& dep : deps)
                        rev[dep].push_back(importer);

                std::vector<std::string> queue, order;
                for (const auto& [k, d] : in_deg)
                    if (d == 0) queue.push_back(k);

                while (!queue.empty()) {
                    auto n = queue.back(); queue.pop_back();
                    order.push_back(n);
                    if (auto it = rev.find(n); it != rev.end())
                        for (const auto& importer : it->second)
                            if (--in_deg[importer] == 0)
                                queue.push_back(importer);
                }

                result.compile_order = std::move(order);
            }

            // 3–5. Resolve each import spec: version + capability + symbol flow.
            for (const auto& mod_name : result.compile_order) {
                auto spec_it = imports_.find(mod_name);
                if (spec_it == imports_.end()) continue;

                for (const auto& spec : spec_it->second) {
                    auto desc_opt = resolver.resolve(spec.module_name);

                    if (!desc_opt) {
                        if (spec.is_optional) {
                            import_diagnostic d;
                            d.kind    = import_error_kind{import_error_kind::kind::not_found};
                            d.symbol  = spec.module_name;
                            d.message = "optional module '" + spec.module_name + "' not found";
                            d.level   = severity::warning;
                            result.diagnostics.push_back(std::move(d));
                            continue;
                        }
                        import_error e;
                        e.kind        = import_error_kind{import_error_kind::kind::not_found};
                        e.module_name = spec.module_name;
                        e.message     = "module '" + spec.module_name + "' not found";
                        e.code        = std::string(import_error_kind::to_code(
                                            import_error_kind{import_error_kind::kind::not_found}));
                        result.errors.push_back(std::move(e));
                        continue;
                    }

                    const auto& desc = *desc_opt;

                    // 3. Version constraint.
                    if (!(spec.min_version <= desc.version &&
                          desc.version    <= spec.max_version)) {
                        import_error e;
                        e.kind        = import_error_kind{import_error_kind::kind::version_mismatch};
                        e.module_name = spec.module_name;
                        e.message     = "module '" + spec.module_name +
                            "' version not in required range";
                        e.code        = std::string(import_error_kind::to_code(
                                            import_error_kind{import_error_kind::kind::version_mismatch}));
                        result.errors.push_back(std::move(e));
                        continue;
                    }

                    // 4. Capability gate.
                    if (spec.required_capabilities != 0) {
                        auto it = caps_map.find(spec.module_name);
                        std::uint64_t cap = (it != caps_map.end()) ? it->second : 0;
                        if ((cap & spec.required_capabilities) != spec.required_capabilities) {
                            import_error e;
                            e.kind        = import_error_kind{import_error_kind::kind::capability_mismatch};
                            e.module_name = spec.module_name;
                            e.message     = "module '" + spec.module_name +
                                "' lacks required capabilities";
                            e.code        = std::string(import_error_kind::to_code(
                                                import_error_kind{import_error_kind::kind::capability_mismatch}));
                            result.errors.push_back(std::move(e));
                            continue;
                        }
                    }

                    // 5. Symbol flow.
                    resolved_import ri;
                    ri.desc = desc;
                    if (sym_provider) {
                        ri.exported_symbols = sym_provider(spec.module_name);
                    }
                    result.resolved.push_back(std::move(ri));
                }
            }

            return result;
        }

        [[nodiscard]] bool empty() const noexcept { return imports_.empty(); }

        [[nodiscard]] const std::vector<import_spec>*
        imports_of(std::string_view module_name) const noexcept {
            auto it = imports_.find(std::string(module_name));
            return it == imports_.end() ? nullptr : &it->second;
        }

    private:
        std::unordered_map<std::string, std::vector<import_spec>> imports_;
    };

} // namespace lang
