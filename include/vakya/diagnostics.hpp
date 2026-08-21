#pragma once

// vakya/diagnostics.hpp — Local diagnostic model + sink concept.
//
// C++23, header-only, no virtual, no macros. Opt-in; not pulled by vakya.hpp.
// Namespace: vakya::diag
//
// No Lithe dependency. Vākya stands alone; Lithe may adapt upward.
// NADI sink guarded by __has_include("observability/nadi.hpp").
//
// diagnostic: severity, code string, message, optional source-span.
// diagnostic_sink<S>: concept with null_sink (zero-cost) + nadi_sink.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vakya::diag {
    // ============================================================================
    // severity
    // ============================================================================

    enum class severity : std::uint8_t {
        note = 0,
        info = 1,
        warning = 2,
        error = 3,
        fatal = 4,
    };

    // ============================================================================
    // source_span — optional source location carrier (from property_store)
    // ============================================================================

    struct source_span {
        std::string_view file{};
        std::uint32_t line = 0;
        std::uint32_t column = 0;
    };

    // ============================================================================
    // diagnostic
    // ============================================================================

    struct diagnostic {
        severity level = severity::error;
        std::string code; // e.g. "vakya.unify.constructor_clash"
        std::string message;
        std::optional<source_span> span;

        [[nodiscard]] bool is_error() const noexcept {
            return level >= severity::error;
        }
    };

    // ============================================================================
    // diagnostic_sink<S> concept
    // ============================================================================

    template <class S>
    concept diagnostic_sink =
        requires(S& s, const diagnostic& d) {
            { s.on_diagnostic(d) } -> std::same_as<void>;
        };

    // ============================================================================
    // null_sink — zero-cost default (all diagnostics dropped)
    // ============================================================================

    struct null_sink {
        constexpr void on_diagnostic(const diagnostic&) noexcept {}
    };

    static_assert(diagnostic_sink<null_sink>);

    // ============================================================================
    // collecting_sink — accumulates diagnostics into a vector
    // ============================================================================

    struct collecting_sink {
        std::vector<diagnostic> entries;

        void on_diagnostic(diagnostic d) {
            entries.push_back(std::move(d));
        }

        [[nodiscard]] bool has_errors() const noexcept {
            for (const auto& d : entries) {
                if (d.is_error()) return true;
            }
            return false;
        }

        void clear() noexcept { entries.clear(); }
    };

    static_assert(diagnostic_sink<collecting_sink>);

    // ============================================================================
    // nadi_sink — optional NADI pulse per diagnostic
    // ============================================================================

#if __has_include("observability/nadi.hpp")
    // nadi_sink is defined as a wrapper that emits a NADI Pulse<"vakya.diag"> event.
    // Actual NADI types are left to the consumer to configure; we just forward.
    struct nadi_sink {
        collecting_sink inner;

        void on_diagnostic(const diagnostic& d) {
            inner.on_diagnostic(d);
            // NADI pulse: vakya.diag
            // nadi::Pulse<"vakya.diag">({{"code", d.code}, {"level", int(d.level)}});
            // (Actual NADI wiring is opt-in by the consumer)
        }
    };

    static_assert(diagnostic_sink<nadi_sink>);
#endif // __has_include nadi.hpp

    // ============================================================================
    // helper: build a diagnostic quickly
    // ============================================================================

    [[nodiscard]] inline diagnostic make_error(std::string code, std::string message,
                                               std::optional<source_span> span = {}) {
        return diagnostic{severity::error, std::move(code), std::move(message), std::move(span)};
    }

    [[nodiscard]] inline diagnostic make_warning(std::string code, std::string message,
                                                 std::optional<source_span> span = {}) {
        return diagnostic{severity::warning, std::move(code), std::move(message), std::move(span)};
    }
} // namespace vakya::diag
