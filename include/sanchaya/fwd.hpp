#pragma once

// ============================================================================
// sanchaya/fwd.hpp — Forward declarations, fundamental types & logic models
// ============================================================================

#include <cstdint>
#include <chrono>
#include <string_view>
#include <optional>
#include <expected>
#include <tuple>
#include <concepts>
#include <type_traits>
#include "meta/meta.hpp"
#include "meta/akshara.hpp"
#include "vakya/vakya.hpp"
#include "containers/dynamic/SmallVector.hpp"

namespace sanchaya {

    // ========================================================================
    // 1. Error Reporting & Domains
    // ========================================================================
    enum class error_domain : std::uint8_t {
        model,
        schema,
        binding,
        planner,
        execution,
        storage,
        concurrency,
        network
    };

    struct sanchaya_error {
        error_domain domain{error_domain::execution};
        std::uint32_t code{0};
        std::string_view message{};
    };

    // ========================================================================
    // 2. Logic Policies & Three-Valued Logic (Domain-Specific)
    // ========================================================================
    enum class logic_policy : std::uint8_t {
        two_valued,              // Strict C++ bool (true, false)
        sql_three_valued,        // SQL NULL semantics (true, false, unknown)
        optional_propagating     // Monadic std::optional propagation
    };

    struct sql_bool {
        enum class state : std::uint8_t {
            is_false = 0,
            is_true = 1,
            is_unknown = 2
        } value{state::is_unknown};

        [[nodiscard]] constexpr bool is_true() const noexcept { return value == state::is_true; }
        [[nodiscard]] constexpr bool is_false() const noexcept { return value == state::is_false; }
        [[nodiscard]] constexpr bool is_unknown() const noexcept { return value == state::is_unknown; }

        constexpr auto operator<=>(const sql_bool&) const noexcept = default;
        constexpr bool operator==(const sql_bool&) const noexcept = default;
    };

    template <logic_policy Policy>
    struct logic_traits;

    template <>
    struct logic_traits<logic_policy::two_valued> {
        using result_type = bool;
    };

    template <>
    struct logic_traits<logic_policy::sql_three_valued> {
        using result_type = sql_bool;
    };

    template <>
    struct logic_traits<logic_policy::optional_propagating> {
        using result_type = std::optional<bool>;
    };

    template <class Expr>
    concept typed_expression = vakya::Expression<Expr> && requires {
        typename std::decay_t<Expr>::result_type;
    };

    template <class Expr, logic_policy Policy>
    concept boolean_expression =
        (typed_expression<Expr> &&
         std::same_as<typename std::decay_t<Expr>::result_type, typename logic_traits<Policy>::result_type>) ||
        (vakya::Expression<Expr> && !typed_expression<Expr>);

    [[nodiscard]] constexpr bool where_accepts(bool val) noexcept { return val; }
    [[nodiscard]] constexpr bool where_accepts(sql_bool val) noexcept { return val.is_true(); }
    [[nodiscard]] constexpr bool where_accepts(const std::optional<bool>& val) noexcept { return val.value_or(false); }

    // ========================================================================
    // 3. Schema Identity
    // ========================================================================
    struct schema_identity {
        std::uint32_t algorithm_version{1};
        std::uint64_t namespace_hash{0};
        std::uint64_t local_hash{0};

        constexpr auto operator<=>(const schema_identity&) const noexcept = default;
        constexpr bool operator==(const schema_identity&) const noexcept = default;
    };

    template <akshara::fixed_string Namespace, akshara::fixed_string LocalName>
    struct named_schema_identity {
        static constexpr auto canonical_name = Namespace + akshara::fixed_string<2>{"."} + LocalName;
        static constexpr schema_identity value{
            .algorithm_version = 1,
            .namespace_hash = akshara::fnv1a64(Namespace),
            .local_hash = akshara::fnv1a64(LocalName)
        };
    };

    // ========================================================================
    // 4. Query Ordering, Consistency, & Residency Requirements
    // ========================================================================
    enum class sort_direction : std::uint8_t {
        ascending,
        descending
    };

    enum class null_placement : std::uint8_t {
        nulls_first,
        nulls_last
    };

    enum class collation_type : std::uint8_t {
        binary,
        nocase,
        natural
    };

    enum class consistency_requirement : std::uint8_t {
        strict_serializable,
        read_committed,
        snapshot,
        bounded_staleness,
        eventual
    };

    enum class residency_requirement : std::uint8_t {
        any,
        local_memory,
        local_disk,
        remote_persistent
    };

    enum class join_kind : std::uint8_t {
        inner,
        left_outer,
        right_outer,
        full_outer,
        cross
    };

    struct bounded_staleness {
        std::chrono::nanoseconds max_staleness{std::chrono::seconds(10)};
    };

    // ========================================================================
    // 5. Member Access Terminals
    // ========================================================================
    template <auto MemberPtr>
    struct member_access_descriptor {
        using vakya_terminal = void;
        using owner_type = meta::member_owner_t<MemberPtr>;
        using value_type = meta::member_value_t<MemberPtr>;
        static constexpr auto member = MemberPtr;
    };

    template <akshara::fixed_string Alias, auto MemberPtr>
    struct aliased_member_access_descriptor {
        using vakya_terminal = void;
        static constexpr auto alias = Alias;
        using owner_type = meta::member_owner_t<MemberPtr>;
        using value_type = meta::member_value_t<MemberPtr>;
        static constexpr auto member = MemberPtr;
    };

} // namespace sanchaya
