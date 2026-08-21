#pragma once

// generic/module_system.hpp — Language-agnostic module descriptor, resolver, dependency graph.
//
// C++23, header-only, no virtual, no macros. Namespace: lang
//
// Generalizes crank/module.hpp into a reusable layer. Key differences:
//   - source_extension is configurable (not hardcoded ".crank")
//   - module_resolver uses resolver_config::source_extension for path mapping
//   - dependency_graph adds cycle_nodes() — returns modules forming the cycle
//   - module_kind, module_hash, module_descriptor, dep_edge identical to crank
//
// Depends on: generic/identity.hpp
//
// Usage:
//   lang::resolver_config cfg;
//   cfg.source_extension = ".mylang";
//   lang::module_resolver resolver{cfg};
//   resolver.add_project_path("/workspace/src");
//   auto desc = resolver.resolve("util.math");  // → /workspace/src/util/math.mylang
//
// For the crank frontend:
//   lang::resolver_config crank_cfg; crank_cfg.source_extension = ".crank";
//   lang::module_resolver crank_resolver{crank_cfg};

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace lang {

    // =========================================================================
    // module_kind
    // =========================================================================

    enum class module_kind : std::uint8_t {
        source            = 0,  // source file on disk
        embedded_src      = 1,  // in-memory source text
        embedded_artifact = 2,  // pre-compiled binary artifact
        native            = 3,  // registered host (C++) module
        package_root      = 4,  // package root module (module.lang)
    };

    [[nodiscard]] constexpr std::string_view to_string(module_kind k) noexcept {
        switch (k) {
        case module_kind::source:            return "source";
        case module_kind::embedded_src:      return "embedded_src";
        case module_kind::embedded_artifact: return "embedded_artifact";
        case module_kind::native:            return "native";
        case module_kind::package_root:      return "package_root";
        }
        return "unknown";
    }

    // =========================================================================
    // module_hash — FNV-1a content hash (stable across identical source)
    // =========================================================================

    struct module_hash {
        std::uint64_t value = 0;
        [[nodiscard]] bool operator==(const module_hash&) const noexcept = default;
        [[nodiscard]] bool empty() const noexcept { return value == 0; }
    };

    [[nodiscard]] inline module_hash hash_source(std::string_view src) noexcept {
        std::uint64_t h = 14695981039346656037ULL;
        for (unsigned char c : src) {
            h ^= static_cast<std::uint64_t>(c);
            h *= 1099511628211ULL;
        }
        return {h};
    }

    // =========================================================================
    // version_triple — semantic version for modules
    // =========================================================================

    struct version_triple {
        std::uint16_t major = 0;
        std::uint16_t minor = 0;
        std::uint16_t patch = 0;

        [[nodiscard]] constexpr bool operator==(const version_triple&) const noexcept = default;
        [[nodiscard]] constexpr bool operator<(const version_triple& o) const noexcept {
            if (major != o.major) return major < o.major;
            if (minor != o.minor) return minor < o.minor;
            return patch < o.patch;
        }
        [[nodiscard]] constexpr bool operator<=(const version_triple& o) const noexcept {
            return !(o < *this);
        }
    };

    // =========================================================================
    // module_capabilities — effect/capability masks for the module boundary
    // =========================================================================

    struct module_capabilities {
        std::uint64_t effect_mask     = 0;
        std::uint64_t capability_mask = 0;
    };

    // =========================================================================
    // module_descriptor — static identity block for a language module
    // =========================================================================

    struct module_descriptor {
        std::string         name;           // "math.vector"
        version_triple      version{};
        module_kind         kind = module_kind::source;
        module_hash         content_hash{};
        module_capabilities capabilities{};
        std::string         source_path;    // filesystem path (kind == source)
        std::string         package_name;   // package clause
    };

    // =========================================================================
    // resolver_config — controls resolution policy + pluggable file extension
    // =========================================================================

    struct resolver_config {
        bool        allow_system_paths    = false;
        bool        allow_package_registry = false;
        std::string source_extension      = ".lang"; // override per language
    };

    // =========================================================================
    // module_resolver — resolves import "name" → module_descriptor (9-tier)
    //
    // Tier order:
    //   1. native           (registered C++ modules)
    //   2. embedded_artifact (pre-compiled, in-memory)
    //   3. embedded_src     (in-memory source text)
    //   4. in_memory        (runtime-injected source strings)
    //   5. project_paths    (source on disk, project-relative)
    //   6. app_paths        (application-level search paths)
    //   7. cache            (compiled cache directories)
    //   8. system           (if config.allow_system_paths)
    //   9. package_registry (if config.allow_package_registry)
    // =========================================================================

    class module_resolver {
    public:
        explicit module_resolver(resolver_config config = {})
            : config_(std::move(config)) {}

        // ── registration ──────────────────────────────────────────────────────

        void add_native(module_descriptor desc) {
            desc.kind = module_kind::native;
            native_[desc.name] = std::move(desc);
        }

        void add_embedded_artifact(module_descriptor desc) {
            desc.kind = module_kind::embedded_artifact;
            embedded_artifact_[desc.name] = std::move(desc);
        }

        void add_embedded_src(std::string module_name, std::string source) {
            module_descriptor d;
            d.name         = module_name;
            d.kind         = module_kind::embedded_src;
            d.content_hash = hash_source(source);
            embedded_src_[module_name] = {std::move(d), std::move(source)};
        }

        // Runtime-injected source (tier 4 — separate from embedded_src).
        void add_in_memory(std::string module_name, std::string source) {
            module_descriptor d;
            d.name         = module_name;
            d.kind         = module_kind::embedded_src;
            d.content_hash = hash_source(source);
            in_memory_[module_name] = {std::move(d), std::move(source)};
        }

        void add_project_path(std::filesystem::path p) { project_paths_.push_back(std::move(p)); }
        void add_app_path(std::filesystem::path p)     { app_paths_.push_back(std::move(p));     }
        void add_cache_path(std::filesystem::path p)   { cache_paths_.push_back(std::move(p));   }
        void add_system_path(std::filesystem::path p)  { system_paths_.push_back(std::move(p));  }

        // ── resolution ────────────────────────────────────────────────────────

        [[nodiscard]] std::optional<module_descriptor>
        resolve(std::string_view import_name) const {
            auto key = std::string(import_name);

            if (auto it = native_.find(key); it != native_.end())            return it->second;
            if (auto it = embedded_artifact_.find(key); it != embedded_artifact_.end()) return it->second;
            if (auto it = embedded_src_.find(key); it != embedded_src_.end()) return it->second.desc;
            if (auto it = in_memory_.find(key); it != in_memory_.end())      return it->second.desc;

            if (auto d = search_paths(import_name, project_paths_)) return d;
            if (auto d = search_paths(import_name, app_paths_))     return d;
            if (auto d = search_paths(import_name, cache_paths_))   return d;

            if (config_.allow_system_paths) {
                if (auto d = search_paths(import_name, system_paths_)) return d;
            }
            return std::nullopt;
        }

        [[nodiscard]] std::optional<std::string>
        source_text(std::string_view module_name) const {
            auto key = std::string(module_name);
            if (auto it = embedded_src_.find(key); it != embedded_src_.end())
                return it->second.source;
            if (auto it = in_memory_.find(key); it != in_memory_.end())
                return it->second.source;
            return std::nullopt;
        }

        [[nodiscard]] const resolver_config& config() const noexcept { return config_; }

    private:
        resolver_config config_;

        std::unordered_map<std::string, module_descriptor> native_;
        std::unordered_map<std::string, module_descriptor> embedded_artifact_;

        struct src_entry { module_descriptor desc; std::string source; };
        std::unordered_map<std::string, src_entry> embedded_src_;
        std::unordered_map<std::string, src_entry> in_memory_;

        std::vector<std::filesystem::path> project_paths_;
        std::vector<std::filesystem::path> app_paths_;
        std::vector<std::filesystem::path> cache_paths_;
        std::vector<std::filesystem::path> system_paths_;

        // "math.vector" → <base>/math/vector<ext>
        [[nodiscard]] std::filesystem::path import_to_path(std::string_view name) const {
            std::filesystem::path p;
            std::string seg;
            for (char c : name) {
                if (c == '.') { p /= seg; seg.clear(); }
                else           seg += c;
            }
            if (!seg.empty()) p /= seg;
            p.replace_extension(config_.source_extension);
            return p;
        }

        [[nodiscard]] std::optional<module_descriptor>
        search_paths(std::string_view name,
                     const std::vector<std::filesystem::path>& paths) const {
            auto rel = import_to_path(name);
            for (const auto& base : paths) {
                auto full = base / rel;
                if (std::filesystem::exists(full)) {
                    module_descriptor d;
                    d.name        = std::string(name);
                    d.kind        = module_kind::source;
                    d.source_path = full.string();
                    return d;
                }
            }
            return std::nullopt;
        }
    };

    // =========================================================================
    // dep_edge — directed import edge (importer → importee)
    // =========================================================================

    struct dep_edge {
        std::string importer;
        std::string importee;
    };

    // =========================================================================
    // dependency_graph — build and query module dependency graph
    //
    // Additions over crank::dependency_graph:
    //   cycle_nodes() — returns the module names forming a detected cycle.
    //   find_dependents(name) — all modules that directly import name.
    //   find_dependencies(name) — all modules that name directly imports.
    // =========================================================================

    class dependency_graph {
    public:
        void add_module(module_descriptor desc) {
            name_to_desc_[desc.name] = std::move(desc);
        }

        void add_import(std::string_view importer, std::string_view importee) {
            auto imp_str = std::string(importer);
            auto imt_str = std::string(importee);
            edges_.push_back({imp_str, imt_str});
            adj_[imp_str].push_back(imt_str);
            rev_adj_[imt_str].push_back(imp_str);
            // Auto-register nodes if not already present.
            if (!name_to_desc_.count(imp_str)) {
                module_descriptor d; d.name = imp_str;
                name_to_desc_[imp_str] = std::move(d);
            }
            if (!name_to_desc_.count(imt_str)) {
                module_descriptor d; d.name = imt_str;
                name_to_desc_[imt_str] = std::move(d);
            }
        }

        [[nodiscard]] const module_descriptor* descriptor(std::string_view name) const {
            auto it = name_to_desc_.find(std::string(name));
            return it == name_to_desc_.end() ? nullptr : &it->second;
        }

        [[nodiscard]] bool empty() const noexcept { return name_to_desc_.empty(); }
        [[nodiscard]] std::size_t size() const noexcept { return name_to_desc_.size(); }

        [[nodiscard]] const module_descriptor* find(std::string_view name) const {
            return descriptor(name);
        }

        // Topological order — importees before importers (Kahn's algorithm).
        // Returns empty vector on cycle detection.
        [[nodiscard]] std::vector<std::string> topo_order() const {
            std::unordered_map<std::string, int> in_degree;
            for (const auto& [k, _] : name_to_desc_) in_degree[k] = 0;
            for (const auto& e : edges_) in_degree[e.importer]++;

            std::vector<std::string> queue, order;
            for (const auto& [k, d] : in_degree)
                if (d == 0) queue.push_back(k);

            while (!queue.empty()) {
                auto n = queue.back(); queue.pop_back();
                order.push_back(n);
                if (auto it = rev_adj_.find(n); it != rev_adj_.end())
                    for (const auto& importer : it->second)
                        if (--in_degree[importer] == 0)
                            queue.push_back(importer);
            }
            if (order.size() != name_to_desc_.size()) return {};
            return order;
        }

        // Returns modules that form a cycle (empty if no cycle).
        // Uses DFS coloring: white=0, gray=1, black=2.
        [[nodiscard]] std::vector<std::string> cycle_nodes() const {
            std::unordered_map<std::string, int> color;
            for (const auto& [k, _] : name_to_desc_) color[k] = 0;

            std::vector<std::string> cycle, path;

            std::function<bool(const std::string&)> dfs =
                [&](const std::string& n) -> bool {
                color[n] = 1; // gray = in stack
                path.push_back(n);
                if (auto it = adj_.find(n); it != adj_.end()) {
                    for (const auto& dep : it->second) {
                        if (color[dep] == 1) {
                            // Found cycle — collect path from dep onward.
                            auto start = std::find(path.begin(), path.end(), dep);
                            cycle.assign(start, path.end());
                            cycle.push_back(dep); // close the loop
                            return true;
                        }
                        if (color[dep] == 0 && dfs(dep)) return true;
                    }
                }
                path.pop_back();
                color[n] = 2; // black = done
                return false;
            };

            for (const auto& [k, _] : name_to_desc_)
                if (color[k] == 0 && dfs(k)) break;

            return cycle;
        }

        // Direct importers of a module (modules that import name).
        [[nodiscard]] std::vector<std::string> find_dependents(std::string_view name) const {
            auto it = rev_adj_.find(std::string(name));
            if (it == rev_adj_.end()) return {};
            return it->second;
        }

        // Direct dependencies of a module (modules that name imports).
        [[nodiscard]] std::vector<std::string> find_dependencies(std::string_view name) const {
            auto it = adj_.find(std::string(name));
            if (it == adj_.end()) return {};
            return it->second;
        }

        [[nodiscard]] const std::vector<dep_edge>& edges() const noexcept { return edges_; }

    private:
        std::unordered_map<std::string, module_descriptor>           name_to_desc_;
        std::vector<dep_edge>                                         edges_;
        std::unordered_map<std::string, std::vector<std::string>>    adj_;     // importer → [importees]
        std::unordered_map<std::string, std::vector<std::string>>    rev_adj_; // importee → [importers]
    };

} // namespace lang
