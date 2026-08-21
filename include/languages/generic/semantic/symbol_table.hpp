#pragma once

// generic/symbol_table.hpp — Generic scope-stack symbol table with pluggable visibility.
//
// C++23, header-only, no virtual, no macros. Namespace: lang
//
// Provides a scope-aware symbol table where visibility inference is a pluggable
// compile-time policy (template parameter). This allows each language to define its
// own convention (e.g. crank: uppercase → exported; explicit: requires annotation).
//
// Depends on: generic/diagnostics.hpp
//
// sym_mutability   — immutable / mutable_ / constant
// sym_visibility   — module_local / exported
// sym_kind         — variable / function / type / module / field / resource
//
// symbol_entry     — one registered symbol with all metadata
//
// visibility_policy concept — requires infer_visibility(string_view) -> sym_visibility
// uppercase_export_policy   — crank convention: name[0] is uppercase → exported
// explicit_export_policy    — annotation-based: all local by default
//
// symbol_table<Policy> — scope-stack symbol table:
//   define(name, entry)     — register in current scope; shadow detection
//   lookup(name)            — walk scope chain newest-first; nullptr if not found
//   push_scope() / pop_scope()
//   exported_symbols()      — all symbols with visibility::exported
//
// Usage:
//   lang::symbol_table<lang::uppercase_export_policy> tbl;
//   tbl.push_scope();
//   lang::symbol_entry e; e.name = "Dot"; e.kind = lang::sym_kind::function;
//   tbl.define(e);
//   auto* found = tbl.lookup("Dot");  // non-null
//   tbl.pop_scope();

#include "languages/generic/core/diagnostics.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace lang {

    // =========================================================================
    // sym_mutability
    // =========================================================================

    enum class sym_mutability : std::uint8_t {
        immutable,
        mutable_,
        constant,    // compile-time constant — can be folded
    };

    // =========================================================================
    // sym_visibility
    // =========================================================================

    enum class sym_visibility : std::uint8_t {
        module_local,  // private to the module
        exported,      // visible to importing modules
    };

    // =========================================================================
    // sym_kind
    // =========================================================================

    enum class sym_kind : std::uint8_t {
        variable,
        function,
        type_alias,
        module,
        field,
        resource,
        constant,
    };

    // =========================================================================
    // symbol_entry — one symbol in the table
    // =========================================================================

    struct symbol_entry {
        std::string      name;
        sym_kind         kind       = sym_kind::variable;
        sym_mutability   mutability = sym_mutability::immutable;
        sym_visibility   visibility = sym_visibility::module_local;
        std::string      type_name;          // resolved type name (may be empty)
        std::string      module_origin;      // which module defined this symbol
        std::size_t      scope_depth = 0;    // assigned by symbol_table on define()
        bool             is_extern   = false; // declared via extern fn
    };

    // =========================================================================
    // shadow_diagnostic — emitted when a new definition shadows an outer one
    // =========================================================================

    struct shadow_diag_kind {
        enum class kind : std::uint8_t { shadowed };
        kind value = kind::shadowed;

        constexpr shadow_diag_kind() = default;
        constexpr shadow_diag_kind(kind k) noexcept : value(k) {}

        [[nodiscard]] static constexpr std::string_view to_code(shadow_diag_kind k) noexcept {
            switch (k.value) { case kind::shadowed: return "LANG-SYM-001"; }
            return "LANG-SYM-000";
        }

        [[nodiscard]] constexpr bool operator==(const shadow_diag_kind&) const noexcept = default;
    };

    using shadow_diagnostic = lang_diagnostic<shadow_diag_kind>;

    // =========================================================================
    // Visibility policies
    // =========================================================================

    // uppercase_export_policy — crank convention: starts with uppercase → exported.
    struct uppercase_export_policy {
        [[nodiscard]] static sym_visibility
        infer_visibility(std::string_view name) noexcept {
            if (!name.empty() && name[0] >= 'A' && name[0] <= 'Z')
                return sym_visibility::exported;
            return sym_visibility::module_local;
        }
    };

    // explicit_export_policy — all symbols module_local unless explicitly overridden.
    struct explicit_export_policy {
        [[nodiscard]] static sym_visibility
        infer_visibility(std::string_view /*name*/) noexcept {
            return sym_visibility::module_local;
        }
    };

    // visibility_policy concept
    template <class P>
    concept visibility_policy =
        requires(std::string_view name) {
            { P::infer_visibility(name) } -> std::same_as<sym_visibility>;
        };

    // =========================================================================
    // symbol_table<Policy>
    // =========================================================================

    template <visibility_policy Policy = uppercase_export_policy>
    class symbol_table {
    public:
        symbol_table() {
            scopes_.push_back({}); // global scope at depth 0
        }

        // Push a new scope (entering a block / function body).
        void push_scope() {
            scopes_.push_back({});
        }

        // Pop the current scope.  No-op if at global scope.
        void pop_scope() {
            if (scopes_.size() > 1)
                scopes_.pop_back();
        }

        [[nodiscard]] std::size_t scope_depth() const noexcept {
            return scopes_.size() - 1;
        }

        // Define a symbol in the current scope.
        // Sets scope_depth and infers visibility from the policy if not overridden.
        // Emits a shadow_diagnostic if an outer scope already has the same name.
        void define(symbol_entry entry,
                    collecting_sink<shadow_diagnostic>* shadow_sink = nullptr) {
            entry.scope_depth = scopes_.size() - 1;
            if (entry.visibility == sym_visibility::module_local) {
                entry.visibility = Policy::infer_visibility(entry.name);
            }
            // Shadow detection — outer scopes only (not current scope = redefinition)
            if (shadow_sink) {
                for (std::size_t i = 0; i + 1 < scopes_.size(); ++i) {
                    const auto& scope = scopes_[i];
                    for (const auto& sym : scope) {
                        if (sym.name == entry.name) {
                            shadow_diagnostic d;
                            d.kind    = shadow_diag_kind{shadow_diag_kind::kind::shadowed};
                            d.symbol  = entry.name;
                            d.message = "symbol '" + entry.name + "' shadows outer definition";
                            d.level   = severity::warning;
                            shadow_sink->on_diagnostic(std::move(d));
                            break;
                        }
                    }
                }
            }
            scopes_.back().push_back(std::move(entry));
        }

        // Lookup by name — newest scope first.  Returns nullptr if not found.
        [[nodiscard]] const symbol_entry* lookup(std::string_view name) const noexcept {
            for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
                for (const auto& sym : *it) {
                    if (sym.name == name) return &sym;
                }
            }
            return nullptr;
        }

        // Lookup restricted to exported symbols only.
        [[nodiscard]] const symbol_entry*
        lookup_exported(std::string_view name) const noexcept {
            const auto* sym = lookup(name);
            if (sym && sym->visibility == sym_visibility::exported) return sym;
            return nullptr;
        }

        // All exported symbols across all scopes (for import resolution).
        [[nodiscard]] std::vector<const symbol_entry*> exported_symbols() const {
            std::vector<const symbol_entry*> result;
            for (const auto& scope : scopes_)
                for (const auto& sym : scope)
                    if (sym.visibility == sym_visibility::exported)
                        result.push_back(&sym);
            return result;
        }

        [[nodiscard]] bool empty() const noexcept {
            for (const auto& scope : scopes_)
                if (!scope.empty()) return false;
            return true;
        }

        [[nodiscard]] std::size_t total_symbols() const noexcept {
            std::size_t n = 0;
            for (const auto& scope : scopes_) n += scope.size();
            return n;
        }

        void clear() {
            scopes_.clear();
            scopes_.push_back({});
        }

    private:
        std::vector<std::vector<symbol_entry>> scopes_;
    };

} // namespace lang
