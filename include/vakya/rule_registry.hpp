#pragma once

// vakya/rule_registry.hpp — ecosystem-scale rewrite-rule registry.
//
// Opt-in via:  #include "vakya/rule_registry.hpp"
// Namespace:   vakya  (rule_descriptor / rule_category / rule_pack / rule_registry)
//
// Motivation: rewrite rules must be DISCOVERABLE, SELECTIVELY LOADABLE and
// TRACEABLE at ecosystem scale (arithmetic / boolean / algebra / tensor /
// physics / statistics packs) WITHOUT the registry knowing any rule's
// internals. This layers metadata + a dynamic registry on top of the existing
// vakya::pattern::rule_set — pattern.hpp is unchanged.
//
// Design:
//   - rule_descriptor: a POD metadata record (id / category / description /
//     version), mirroring lithe's pass_type_traits / profile_descriptor
//     conventions. Extension ids live in a reserved band (>= kRuleExtensionBase).
//   - rule_pack<Rules...>: a zero-cost [[no_unique_address]] aggregate over
//     pattern rule_sets — the compile-time bundle. rules::arithmetic (already
//     in pattern.hpp) is the first pack.
//   - rule_registry: a dynamic store (descriptor + type-erased apply thunk).
//     register_pack / find / by_category / discover. Selective load = only
//     register the packs you want. The registry is constraint-agnostic:
//     predicates / type / shape constraints ride the Property System, so
//     future ML rule-selection needs NO registry change.
//
// Constraints: C++23, header-only, no virtual, no macros, pay-for-what-you-use.
// Zero upward dependency (vakya only).

#include "pattern.hpp"

#include <any>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace vakya {
    // -----------------------------------------------------------------------
    // version_triple — local structural semver (vakya cannot depend on lithe).
    // -----------------------------------------------------------------------
    struct rule_version {
        std::uint32_t major = 0;
        std::uint32_t minor = 0;
        std::uint32_t patch = 0;
        constexpr bool operator==(const rule_version&) const noexcept = default;
    };

    // -----------------------------------------------------------------------
    // rule_category — coarse domain classification for discovery / selective load.
    //   Open by design: `custom` + a free-form domain string cover downstream
    //   domains without editing this enum.
    // -----------------------------------------------------------------------
    enum class rule_category : std::uint8_t {
        arithmetic = 0,
        boolean,
        algebra,
        tensor,
        physics,
        statistics,
        custom,
    };

    [[nodiscard]] constexpr std::string_view to_string(rule_category c) noexcept {
        switch (c) {
        case rule_category::arithmetic: return "arithmetic";
        case rule_category::boolean: return "boolean";
        case rule_category::algebra: return "algebra";
        case rule_category::tensor: return "tensor";
        case rule_category::physics: return "physics";
        case rule_category::statistics: return "statistics";
        case rule_category::custom: return "custom";
        }
        return "unknown";
    }

    // Built-in descriptor ids occupy [0, kRuleExtensionBase); downstream packs
    // MUST assign ids >= kRuleExtensionBase so they never collide.
    inline constexpr std::size_t kRuleExtensionBase = 1000u;

    // -----------------------------------------------------------------------
    // rule_descriptor — metadata record for one rule (or rule_set entry).
    // -----------------------------------------------------------------------
    struct rule_descriptor {
        std::size_t id{};
        rule_category category{rule_category::custom};
        std::string_view name{}; // stable label (points to static storage)
        std::string_view description{};
        rule_version version{};
    };

    // -----------------------------------------------------------------------
    // rule_pack<RuleSet> — compile-time bundle of a descriptor + a rule_set.
    //   RuleSet is any vakya::pattern::rule_set<...>. Zero-cost storage.
    // -----------------------------------------------------------------------
    template <class RuleSet>
    struct rule_pack {
        rule_descriptor descriptor{};
        [[no_unique_address]] RuleSet rules{};

        constexpr rule_pack(rule_descriptor d, RuleSet rs)
            : descriptor(d), rules(std::move(rs)) {}

        // Apply the pack's rule_set to an expression, first-match semantics.
        template <class Expr>
        [[nodiscard]] std::optional<std::any> apply_first(const Expr& e) const {
            return rules.apply_first(e);
        }
    };

    template <class RuleSet>
    [[nodiscard]] constexpr auto make_rule_pack(rule_descriptor d, RuleSet rs) {
        return rule_pack<RuleSet>{d, std::move(rs)};
    }

    // -----------------------------------------------------------------------
    // rule_registry — dynamic, discoverable store of registered packs.
    //   Each entry keeps the descriptor plus a type-erased apply thunk so the
    //   registry can drive rules without naming their concrete rule_set type.
    //   The thunk erases over std::any (matching rule_set::apply_first) — no
    //   virtual, one std::function per entry (plugin-only cost, off the hot path).
    // -----------------------------------------------------------------------
    class rule_registry {
    public:
        // Erased application: given a std::any-wrapped expression, return the
        // rewrite result (also std::any) or nullopt on miss. The concrete Expr
        // type is captured at register time.
        template <class Expr>
        using apply_fn = std::optional<std::any> (*)(const void* rules, const Expr& e);

        struct entry {
            rule_descriptor descriptor{};
            const void* rules_ptr{}; // points to caller-owned rule_pack::rules
        };

        rule_registry() = default;

        // Register a pack. The pack must outlive the registry (caller owns it,
        // exactly like the compile-time rule_sets in pattern.hpp — they are
        // inline-const with static storage duration).
        template <class RuleSet>
        void register_pack(const rule_pack<RuleSet>& pack) {
            entries_.push_back(entry{pack.descriptor, static_cast<const void*>(&pack.rules)});
        }

        // Registration by explicit descriptor + rule_set (when no pack wrapper).
        template <class RuleSet>
        void register_rules(rule_descriptor d, const RuleSet& rules) {
            entries_.push_back(entry{d, static_cast<const void*>(&rules)});
        }

        // -------- discovery --------------------------------------------------
        [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
        [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }

        [[nodiscard]] const std::vector<entry>& discover() const noexcept {
            return entries_;
        }

        [[nodiscard]] const entry* find(std::size_t id) const noexcept {
            for (const auto& e : entries_) if (e.descriptor.id == id) return &e;
            return nullptr;
        }

        [[nodiscard]] const entry* find(std::string_view name) const noexcept {
            for (const auto& e : entries_) if (e.descriptor.name == name) return &e;
            return nullptr;
        }

        [[nodiscard]] std::vector<const entry*> by_category(rule_category c) const {
            std::vector<const entry*> out;
            for (const auto& e : entries_)
                if (e.descriptor.category == c) out.push_back(&e);
            return out;
        }

        // -------- application ------------------------------------------------
        // Apply a specific registered pack (by name) to an expression. The
        // caller supplies the concrete RuleSet type so the erased pointer is
        // recovered type-safely.
        template <class RuleSet, class Expr>
        [[nodiscard]] std::optional<std::any>
        apply(std::string_view name, const Expr& e) const {
            if (const entry* en = find(name)) {
                const auto* rs = static_cast<const RuleSet*>(en->rules_ptr);
                return rs->apply_first(e);
            }
            return std::nullopt;
        }

    private:
        std::vector<entry> entries_;
    };

    // -----------------------------------------------------------------------
    // Built-in descriptors + packs for the shipped arithmetic rule_sets.
    //   These reference pattern::rules::arithmetic — the single source of the
    //   actual rewrites. Ids are in the built-in band.
    // -----------------------------------------------------------------------
    namespace rule_packs {
        inline const auto arithmetic_add_zero = make_rule_pack(
            rule_descriptor{
                0, rule_category::arithmetic, "arith.add_zero",
                "x + 0 -> x, 0 + x -> x", rule_version{1, 0, 0}
            },
            pattern::rules::arithmetic::add_zero);

        inline const auto arithmetic_mul_one = make_rule_pack(
            rule_descriptor{
                1, rule_category::arithmetic, "arith.mul_one",
                "x * 1 -> x, 1 * x -> x", rule_version{1, 0, 0}
            },
            pattern::rules::arithmetic::mul_one);

        inline const auto arithmetic_mul_zero = make_rule_pack(
            rule_descriptor{
                2, rule_category::arithmetic, "arith.mul_zero",
                "x * 0 -> 0, 0 * x -> 0", rule_version{1, 0, 0}
            },
            pattern::rules::arithmetic::mul_zero);

        inline const auto arithmetic_double_neg = make_rule_pack(
            rule_descriptor{
                3, rule_category::arithmetic, "arith.double_neg",
                "-(-x) -> x", rule_version{1, 0, 0}
            },
            pattern::rules::arithmetic::double_neg);
    } // namespace rule_packs

    // Convenience: a registry pre-loaded with all built-in arithmetic packs.
    [[nodiscard]] inline rule_registry make_arithmetic_registry() {
        rule_registry r;
        r.register_pack(rule_packs::arithmetic_add_zero);
        r.register_pack(rule_packs::arithmetic_mul_one);
        r.register_pack(rule_packs::arithmetic_mul_zero);
        r.register_pack(rule_packs::arithmetic_double_neg);
        return r;
    }
} // namespace vakya
