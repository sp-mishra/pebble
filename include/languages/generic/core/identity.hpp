#pragma once

// generic/identity.hpp — Stable entity identifiers, FNV-1a hashing, fingerprint utilities.
//
// C++23, header-only, no virtual, no macros. Namespace: lang
//
// Provides language-agnostic stable identity primitives shared across all headers
// in the generic layer. Zero external dependencies (only <cstdint> + <string_view>).
//
// stable_entity_id — deterministic 128-bit ID derived from qualified name + kind + version.
//   Cross-compile stable: same inputs always produce the same id.
//   Kind constants: kKindFunction, kKindType, kKindField, kKindResource, kKindBackend,
//                   kKindContainer, kKindRule, kKindModule.
//
// descriptor_fingerprint — uint64_t hash of a descriptor's observable state.
//   Combines multiple fingerprints via fp_combine (XOR + rotate).
//
// Usage:
//   constexpr auto id = lang::detail::make_id("math.dot", lang::kKindFunction);
//   constexpr auto fp = lang::detail::fp_combine(f1, f2);

#include <cstdint>
#include <string_view>

namespace lang {

    // =========================================================================
    // Kind constants — discriminate entity categories within stable_entity_id
    // =========================================================================

    inline constexpr std::uint32_t kKindFunction  = 1;
    inline constexpr std::uint32_t kKindType      = 2;
    inline constexpr std::uint32_t kKindField     = 3;
    inline constexpr std::uint32_t kKindResource  = 4;
    inline constexpr std::uint32_t kKindBackend   = 5;
    inline constexpr std::uint32_t kKindContainer = 6;
    inline constexpr std::uint32_t kKindRule      = 7;
    inline constexpr std::uint32_t kKindModule    = 8;

    // =========================================================================
    // descriptor_fingerprint — observable-state hash for change detection
    // =========================================================================

    using descriptor_fingerprint = std::uint64_t;

    // =========================================================================
    // stable_entity_id — cross-compile-stable entity identity
    // =========================================================================

    struct stable_entity_id {
        std::uint64_t namespace_hash  = 0; // FNV-1a of "namespace.name"
        std::uint64_t name_hash       = 0; // FNV-1a of bare name
        std::uint32_t kind            = 0; // kKind* constant
        std::uint32_t schema_version  = 1; // bump to invalidate cached IDs

        [[nodiscard]] constexpr bool operator==(const stable_entity_id&) const noexcept = default;
        [[nodiscard]] constexpr bool operator!=(const stable_entity_id&) const noexcept = default;

        [[nodiscard]] constexpr bool valid() const noexcept {
            return namespace_hash != 0 || name_hash != 0;
        }
    };

    using stable_function_id  = stable_entity_id;
    using stable_type_id      = stable_entity_id;
    using stable_field_id     = stable_entity_id;
    using stable_resource_id  = stable_entity_id;
    using stable_module_id    = stable_entity_id;
    using stable_rule_id      = stable_entity_id;

    // =========================================================================
    // detail — implementation helpers (constexpr, no allocation)
    // =========================================================================

    namespace detail {

        // FNV-1a 64-bit — deterministic, constexpr, cross-platform.
        [[nodiscard]] constexpr std::uint64_t fnv1a(std::string_view s) noexcept {
            std::uint64_t h = 14695981039346656037ULL;
            for (unsigned char c : s) {
                h ^= static_cast<std::uint64_t>(c);
                h *= 1099511628211ULL;
            }
            return h;
        }

        // Derive stable_entity_id from a fully-qualified name (e.g. "math.dot"),
        // kind constant, and schema version. Splits on last '.' for namespace/name split.
        [[nodiscard]] constexpr stable_entity_id
        make_id(std::string_view qualified, std::uint32_t kind,
                std::uint32_t version = 1) noexcept {
            stable_entity_id id;
            id.kind           = kind;
            id.schema_version = version;

            auto dot = qualified.rfind('.');
            if (dot == std::string_view::npos) {
                id.namespace_hash = fnv1a("");
                id.name_hash      = fnv1a(qualified);
            } else {
                id.namespace_hash = fnv1a(qualified.substr(0, dot));
                id.name_hash      = fnv1a(qualified.substr(dot + 1));
            }
            return id;
        }

        // Combine two fingerprints — XOR + rotate left 17.
        [[nodiscard]] constexpr descriptor_fingerprint
        fp_combine(descriptor_fingerprint a, descriptor_fingerprint b) noexcept {
            // Rotation avoids fp_combine(x,y) == fp_combine(y,x) symmetry collapse.
            constexpr int kRotate = 17;
            a = (a << kRotate) | (a >> (64 - kRotate));
            return a ^ b;
        }

        // Derive a fingerprint from a string (name, source, etc.).
        [[nodiscard]] constexpr descriptor_fingerprint
        fp_from_string(std::string_view s) noexcept {
            return fnv1a(s);
        }

        // Combine a fingerprint with a uint64 scalar (arity, flags, etc.).
        [[nodiscard]] constexpr descriptor_fingerprint
        fp_with_scalar(descriptor_fingerprint base, std::uint64_t scalar) noexcept {
            return fp_combine(base, scalar);
        }

    } // namespace detail

} // namespace lang
