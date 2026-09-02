#pragma once

// generic/registry.hpp — Generic registry builder → finalized snapshot.
//
// C++23, header-only, no virtual, no macros. Namespace: lang
//
// Two-phase lifecycle:
//   registry_builder<DescSet>  — mutable; add_function / add_type / etc.
//   finalized_registry<DescSet> — immutable snapshot; O(1) hash lookup.
//
// Collision detection: duplicate stable_entity_id → build_diagnostic (error).
// Global fingerprint: deterministic over all registered descriptors in insertion order.
//
// Depends on: generic/identity.hpp, generic/descriptors.hpp, generic/diagnostics.hpp
//
// DescSet concept — type bundle used by both builder and finalized registry:
//   DescSet::function_type  (must have .id, .name, .fingerprint)
//   DescSet::type_type      (must have .id, .name, .fingerprint)
//   DescSet::field_type     (must have .id, .name)
//   DescSet::resource_type  (must have .id, .name, .fingerprint)
//
// Usage:
//   struct my_desc_set {
//       using function_type = lang::function_descriptor_base;
//       using type_type     = lang::type_descriptor_base;
//       using field_type    = lang::field_descriptor_base;
//       using resource_type = lang::resource_descriptor_base;
//   };
//   lang::registry_builder<my_desc_set> builder;
//   builder.add_function(fn_desc);
//   auto result = builder.build();
//   if (result) { /* use result->find_function("math.dot") */ }

#include "languages/generic/core/identity.hpp"
#include "languages/generic/host/descriptors.hpp"
#include "languages/generic/core/diagnostics.hpp"

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace lang {
    // =========================================================================
    // build_diagnostic — emitted during registry_builder::build()
    // =========================================================================

    struct build_diag_kind {
        enum class kind : std::uint8_t {
            duplicate_id, // LANG-REG-001: two descriptors share a stable_entity_id
            empty_name, // LANG-REG-002: descriptor has empty name
        };

        kind value = kind::duplicate_id;

        constexpr build_diag_kind() = default;
        constexpr build_diag_kind(kind k) noexcept : value(k) {}

        [[nodiscard]] static constexpr std::string_view to_code(build_diag_kind k) noexcept {
            switch (k.value) {
            case kind::duplicate_id: return "LANG-REG-001";
            case kind::empty_name: return "LANG-REG-002";
            }
            return "LANG-REG-000";
        }

        [[nodiscard]] constexpr bool operator==(const build_diag_kind&) const noexcept = default;
    };

    using build_diagnostic = lang_diagnostic<build_diag_kind>;

    // =========================================================================
    // DescSet concept
    // =========================================================================

    template <class DS>
    concept DescSet = requires {
        typename DS::function_type;
        typename DS::type_type;
        typename DS::field_type;
        typename DS::resource_type;
    };

    // =========================================================================
    // default_desc_set — uses the base descriptor types from descriptors.hpp
    // =========================================================================

    struct default_desc_set {
        using function_type = function_descriptor_base;
        using type_type = type_descriptor_base;
        using field_type = field_descriptor_base;
        using resource_type = resource_descriptor_base;
    };

    // =========================================================================
    // finalized_registry<DS> — immutable, read-only snapshot
    // =========================================================================

    template <DescSet DS = default_desc_set>
    class finalized_registry {
    public:
        using fn_map = std::unordered_map<std::string, typename DS::function_type>;
        using ty_map = std::unordered_map<std::string, typename DS::type_type>;
        using fd_map = std::unordered_map<std::string, typename DS::field_type>;
        using res_map = std::unordered_map<std::string, typename DS::resource_type>;

        finalized_registry(fn_map fns, ty_map tys, fd_map fds, res_map ress,
                           descriptor_fingerprint global_fp)
            : functions_(std::move(fns)),
              types_(std::move(tys)),
              fields_(std::move(fds)),
              resources_(std::move(ress)),
              global_fingerprint_(global_fp) {}

        [[nodiscard]] const typename DS::function_type*
        find_function(std::string_view name) const noexcept {
            auto it = functions_.find(std::string(name));
            return it == functions_.end() ? nullptr : &it->second;
        }

        [[nodiscard]] const typename DS::type_type*
        find_type(std::string_view name) const noexcept {
            auto it = types_.find(std::string(name));
            return it == types_.end() ? nullptr : &it->second;
        }

        [[nodiscard]] const typename DS::field_type*
        find_field(std::string_view name) const noexcept {
            auto it = fields_.find(std::string(name));
            return it == fields_.end() ? nullptr : &it->second;
        }

        [[nodiscard]] const typename DS::resource_type*
        find_resource(std::string_view name) const noexcept {
            auto it = resources_.find(std::string(name));
            return it == resources_.end() ? nullptr : &it->second;
        }

        [[nodiscard]] std::size_t function_count() const noexcept { return functions_.size(); }
        [[nodiscard]] std::size_t type_count() const noexcept { return types_.size(); }
        [[nodiscard]] std::size_t field_count() const noexcept { return fields_.size(); }
        [[nodiscard]] std::size_t resource_count() const noexcept { return resources_.size(); }

        [[nodiscard]] descriptor_fingerprint global_fingerprint() const noexcept {
            return global_fingerprint_;
        }

        template <class Fn>
        void for_each_function(Fn&& fn) const {
            for (const auto& [_, d] : functions_) fn(d);
        }

        template <class Fn>
        void for_each_type(Fn&& fn) const {
            for (const auto& [_, d] : types_) fn(d);
        }

    private:
        fn_map functions_;
        ty_map types_;
        fd_map fields_;
        res_map resources_;
        descriptor_fingerprint global_fingerprint_ = 0;
    };

    // =========================================================================
    // registry_builder<DS> — mutable phase; produces finalized_registry on build()
    // =========================================================================

    template <DescSet DS = default_desc_set>
    class registry_builder {
    public:
        using build_result = std::expected<finalized_registry<DS>,
                                           std::vector<build_diagnostic>>;

        void add_function(typename DS::function_type desc) {
            pending_fns_.push_back(std::move(desc));
        }

        void add_type(typename DS::type_type desc) {
            pending_tys_.push_back(std::move(desc));
        }

        void add_field(typename DS::field_type desc) {
            pending_fds_.push_back(std::move(desc));
        }

        void add_resource(typename DS::resource_type desc) {
            pending_res_.push_back(std::move(desc));
        }

        // Build the finalized registry.
        // Returns an error vector if any collision or empty-name is detected.
        [[nodiscard]] build_result build() {
            std::vector<build_diagnostic> errors;
            typename finalized_registry<DS>::fn_map fns;
            typename finalized_registry<DS>::ty_map tys;
            typename finalized_registry<DS>::fd_map fds;
            typename finalized_registry<DS>::res_map ress;
            descriptor_fingerprint gfp = 0;

            for (auto& d : pending_fns_) {
                if (d.name.empty()) {
                    build_diagnostic bd;
                    bd.kind = build_diag_kind{build_diag_kind::kind::empty_name};
                    bd.symbol = "(function)";
                    bd.message = "function descriptor has empty name";
                    errors.push_back(std::move(bd));
                    continue;
                }
                if (fns.count(d.name)) {
                    build_diagnostic bd;
                    bd.kind = build_diag_kind{build_diag_kind::kind::duplicate_id};
                    bd.symbol = d.name;
                    bd.message = "duplicate function registration: '" + d.name + "'";
                    errors.push_back(std::move(bd));
                    continue;
                }
                gfp = detail::fp_combine(gfp, d.fingerprint);
                fns.emplace(d.name, std::move(d));
            }

            for (auto& d : pending_tys_) {
                if (d.name.empty()) {
                    build_diagnostic bd;
                    bd.kind = build_diag_kind{build_diag_kind::kind::empty_name};
                    bd.symbol = "(type)";
                    bd.message = "type descriptor has empty name";
                    errors.push_back(std::move(bd));
                    continue;
                }
                if (tys.count(d.name)) {
                    build_diagnostic bd;
                    bd.kind = build_diag_kind{build_diag_kind::kind::duplicate_id};
                    bd.symbol = d.name;
                    bd.message = "duplicate type registration: '" + d.name + "'";
                    errors.push_back(std::move(bd));
                    continue;
                }
                gfp = detail::fp_combine(gfp, d.fingerprint);
                tys.emplace(d.name, std::move(d));
            }

            for (auto& d : pending_fds_)
                fds.emplace(d.name, std::move(d));

            for (auto& d : pending_res_) {
                gfp = detail::fp_combine(gfp, d.fingerprint);
                ress.emplace(d.name, std::move(d));
            }

            if (!errors.empty()) return std::unexpected(std::move(errors));
            return finalized_registry<DS>(std::move(fns), std::move(tys),
                                          std::move(fds), std::move(ress), gfp);
        }

    private:
        std::vector<typename DS::function_type> pending_fns_;
        std::vector<typename DS::type_type> pending_tys_;
        std::vector<typename DS::field_type> pending_fds_;
        std::vector<typename DS::resource_type> pending_res_;
    };
} // namespace lang
