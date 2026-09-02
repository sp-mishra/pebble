#pragma once

// generic/effects.hpp — Generic effect/capability infrastructure (delegates to vakya/types/).
//
// C++23, header-only, no virtual, no macros. Namespace: lang
//
// Re-exports and extends the vakya::types effect/capability system so the generic
// layer and all language frontends share one implementation rather than duplicating it.
//
// vakya::types provides:
//   effect_descriptor, effect_registry, make_builtin_effect_registry()
//   capability_descriptor, capability_registry, make_builtin_capability_registry()
//   effect_mask, capability_mask (uint64_t bitmasks)
//   builtin effect stable_ids 1–5: FileSystem/Memory/IO/Network/Exception
//   builtin cap  stable_ids 1–5: Read/Write/Network/Execute/Allocate
//
// lang:: adds:
//   kEffectExtBase / kCapExtBase        — first available extension-band mask bit
//   kEffectExtensionBase                — stable_id threshold for ext-band entries
//   fn_attribute_set                    — declared @pure/@io/@net/etc. per function
//   effects_result                      — computed final masks + conflict flag
//   effect_checker                      — validates declared vs inferred effects
//
// Extension-band usage (language adds its own effects, stable_id >= 1000):
//   auto reg = lang::make_builtin_effect_registry(); // from vakya
//   vakya::types::effect_descriptor ext;
//   ext.stable_id = 1000; ext.symbol = "@host"; ext.bit_mask = lang::kEffectExtBase;
//   reg.register_desc(ext);
//
// crank/effects.hpp does exactly this: starts from make_crank_effects_registry()
// which calls make_builtin_effect_registry() then appends @host/@gpu/@parallel-safe.

#include "vakya/types/effect.hpp"
#include "vakya/types/capability.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace lang {
    // =========================================================================
    // Re-export vakya types under lang:: so generic/ headers stay consistent.
    // =========================================================================

    using effect_mask = vakya::types::effect_mask;
    using capability_mask = vakya::types::capability_mask;
    using effect_registry = vakya::types::effect_registry;
    using capability_registry = vakya::types::capability_registry;
    using effect_descriptor = vakya::types::effect_descriptor;
    using capability_descriptor = vakya::types::capability_descriptor;

    // =========================================================================
    // Extension-band constants — IDs and mask bits for language-defined effects.
    // Builtin effects occupy stable_ids 1–5 and mask bits 0–4.
    // Languages register their own effects starting at kEffectExtensionBase (1000).
    // =========================================================================

    inline constexpr std::uint32_t kEffectExtensionBase = 1000u;
    inline constexpr effect_mask kEffectExtBase = 1ULL << 32; // first ext mask bit
    inline constexpr capability_mask kCapExtBase = 1ULL << 32;

    // =========================================================================
    // Delegating factory helpers — call vakya's builtins, language adds ext-band.
    // =========================================================================

    [[nodiscard]] inline effect_registry make_builtin_effect_registry() {
        return vakya::types::make_builtin_effect_registry();
    }

    [[nodiscard]] inline capability_registry make_builtin_capability_registry() {
        return vakya::types::make_builtin_capability_registry();
    }

    // =========================================================================
    // fn_attribute_set — declared function attributes (@pure, @io, @net, etc.)
    // =========================================================================

    struct fn_attribute_set {
        effect_mask declared_effects = 0;
        capability_mask declared_caps = 0;
        bool is_pure = false; // @pure: asserts final effects == 0 && caps == 0
    };

    // =========================================================================
    // effects_result — output of effect checking for one function
    // =========================================================================

    struct effects_result {
        effect_mask final_effects = 0;
        capability_mask final_caps = 0;
        bool conflict = false; // @pure declared but non-zero inferred effects
        std::string conflict_detail;
    };

    // =========================================================================
    // effect_checker — validates declared vs inferred effects using the registries.
    // Accumulates effects_result per declare_fn() call; retrieve via take().
    // =========================================================================

    class effect_checker {
    public:
        effect_checker(const effect_registry& ereg, const capability_registry& creg)
            : ereg_(ereg), creg_(creg) {}

        void declare_fn(std::string_view name,
                        const fn_attribute_set& attrs,
                        effect_mask inferred_effects,
                        capability_mask inferred_caps) {
            effects_result r;
            r.final_effects = inferred_effects | attrs.declared_effects;
            r.final_caps = inferred_caps | attrs.declared_caps;

            if (attrs.is_pure && (r.final_effects != 0 || r.final_caps != 0)) {
                r.conflict = true;
                r.conflict_detail = std::string("function '") + std::string(name) +
                    "' declared @pure but has inferred effects/caps";
                r.final_effects = 0;
                r.final_caps = 0;
            }
            results_.push_back(std::move(r));
        }

        [[nodiscard]] std::vector<effects_result> take() { return std::move(results_); }

    private:
        [[maybe_unused]] const effect_registry& ereg_;
        [[maybe_unused]] const capability_registry& creg_;
        std::vector<effects_result> results_;
    };
} // namespace lang
