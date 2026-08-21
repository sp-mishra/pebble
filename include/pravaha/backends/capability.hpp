#pragma once
// ============================================================================
// backends/capability.hpp — ComputeBackend concept, traits, and the
//   compile-time backend registry for the Pravaha heterogeneous overlay.
//
// The sole surviving platform #if lives here, confined to default_backend_set
// assembly. It never leaks into hetero_executor or any user-facing header.
// ============================================================================

#include <concepts>
#include <cstdint>
#include <string_view>
#include <tuple>
#include <type_traits>

// Bring in compute value types (buffer_descriptor, data_element_type).
// capability.hpp is included by pravaha_hetero.hpp after those are defined.
// This header relies on them being in scope — do not include it standalone.

namespace pravaha::compute {
    // ============================================================================
    // backend_metadata — static descriptor every backend must expose.
    // ============================================================================

    struct backend_metadata {
        std::string_view name;
        std::uint32_t hardware_priority = 0; // higher = preferred by cost model
    };

    // ============================================================================
    // ComputeBackend concept — structural, compile-time interface.
    // No virtual functions. No base class. Any type satisfying these requirements
    // is a valid backend. The execute_elementwise / execute_reduction entry points
    // are templated on the Lithe expression type (not erased) so backends retain
    // full Lithe fusion/codegen capability.
    // ============================================================================

    template <typename B>
    concept ComputeBackend = requires(
        B backend,
        std::size_t structural_hash,
        data_element_type type,
        const buffer_descriptor& desc) {
            // Static metadata (name, priority) — usable at compile time.
            { B::static_metadata() } noexcept -> std::same_as<backend_metadata>;

            // Runtime availability probe (device present, driver loaded). Not a macro.
            { backend.is_available() } noexcept -> std::same_as<bool>;

            // Capability query: can this backend handle the given (topology, element type)?
            {
                backend.supports_expression(structural_hash, type)
            } noexcept
            -> std::same_as<bool>;

            // Cost weight (higher suitability = lower cost relative to priority).
            // Returns 0 if backend is unusable for this shape.
            {
                backend.evaluate_cost(desc, structural_hash)
            } noexcept
            -> std::same_as<std::uint64_t>;
        };

    // ============================================================================
    // backend_traits — decentralized capability advertisement.
    // Each backend declares its own type-level properties via specialization or
    // by satisfying the concept (which auto-specializes the concept-constrained
    // partial below).
    //
    // NOTE: pravaha::compute::backend_traits is distinct from
    // lithe::execution::backend_traits (which lives in lithe_codegen_*.hpp and
    // describes code-generation target properties — device family, memory model,
    // etc.). This struct describes *compute routing* properties: priority,
    // supported element types, and availability. Same name, orthogonal axis.
    // ============================================================================

    template <typename Backend>
    struct backend_traits {
        static constexpr bool is_backend = false;
        static constexpr std::uint32_t priority() noexcept { return 0; }
        static constexpr bool supports_type(data_element_type) noexcept { return false; }
    };

    template <ComputeBackend Backend>
    struct backend_traits<Backend> {
        static constexpr bool is_backend = true;
        static std::uint32_t priority() noexcept { return Backend::static_metadata().hardware_priority; }

        // Delegates to the backend's own per-type capability predicate when present,
        // otherwise defaults to true (let supports_expression() do runtime filtering).
        static constexpr bool supports_type(data_element_type t) noexcept {
            if constexpr (requires { Backend::supports_type(t); })
                return Backend::supports_type(t);
            else
                return true;
        }
    };

    // ============================================================================
    // backend_set — compile-time ordered list of backend types.
    // Parameters are unconstrained at declaration to allow forward-declared types
    // in default_hetero_executor aliases. The ComputeBackend concept is enforced
    // at the point of use inside hetero_executor's dispatch methods (backend_score
    // and the execute_elementwise/execute_reduction call sites).
    // Provide backends in descending priority order (GPU first, SIMD last).
    // ============================================================================

    template <typename... Backends>
    struct backend_set {
        using types = std::tuple<Backends...>;
        static constexpr std::size_t count = sizeof...(Backends);
    };

    // ============================================================================
    // Default backend set assembly — the one place platform detection is allowed.
    // __has_include is used, not __APPLE__ user-space guards, so the check is
    // purely about what is compiled in rather than the OS identity.
    //
    // Include order: higher-priority GPU backends first, SIMD fallback last.
    // ============================================================================
} // namespace pravaha::compute
// default_backend_set is defined at the bottom of pravaha_hetero.hpp after all
// backend wrappers (HostSimdBackend, MetalGpuBackend) are fully defined.
