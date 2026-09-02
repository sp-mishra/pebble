#pragma once

// Optional Vakya -> Tarka frontend adapter. This stays on the Vakya side.
#include "vakya/vakya.hpp"
#include "tarka/frontend/ir.hpp"
#include <tuple>
#include <type_traits>

namespace vakya::tarka_frontend {
    namespace smt = ::tarka::frontend::ir;

    struct symbol {
        std::string_view name;
        using vakya_terminal = void;
    };

    namespace detail {
        template <class T>
        inline constexpr bool always_false = false;

        template <class Tag>
        constexpr std::string_view spelling() {
            if constexpr (std::same_as<Tag, add_tag>) return "+";
            else if constexpr (std::same_as<Tag, sub_tag>) return "-";
            else if constexpr (std::same_as<Tag, mul_tag>) return "*";
            else if constexpr (std::same_as<Tag, div_tag>) return "/";
            else if constexpr (std::same_as<Tag, mod_tag>) return "mod";
            else if constexpr (std::same_as<Tag, eq_tag>) return "=";
            else if constexpr (std::same_as<Tag, ne_tag>) return "distinct";
            else if constexpr (std::same_as<Tag, lt_tag>) return "<";
            else if constexpr (std::same_as<Tag, le_tag>) return "<=";
            else if constexpr (std::same_as<Tag, gt_tag>) return ">";
            else if constexpr (std::same_as<Tag, ge_tag>) return ">=";
            else if constexpr (std::same_as<Tag, and_tag>) return "and";
            else if constexpr (std::same_as<Tag, or_tag>) return "or";
            else if constexpr (std::same_as<Tag, not_tag>) return "not";
            else static_assert(always_false<Tag>, "Vakya tag is not representable in SMT-LIB");
        }

        template <class E>
        smt::node_id lower(smt::script& out, const E& e) {
            using D = std::decay_t<E>;
            if constexpr (std::same_as<D, symbol>) return smt::append(out, smt::kind::symbol, {},
                                                                      out.symbols.intern(e.name));
            else if constexpr (std::same_as<D, bool>) return smt::append(
                out, smt::kind::bool_literal, {}, 0, {{std::uint32_t(e)}});
            else if constexpr (std::integral<D>) return smt::append(out, smt::kind::int_literal, {}, 0,
                                                                    {{std::int64_t(e)}});
            else if constexpr (Expression<D>) {
                std::vector<smt::node_id> kids;
                std::apply([&](const auto&... x) { (kids.push_back(lower(out, x)), ...); }, e.children);
                return smt::append(out, smt::kind::application, {},
                                   out.symbols.intern(spelling<typename D::tag_type>()), {}, kids);
            }
            else static_assert(always_false<D>, "Vakya terminal is not representable in Tarka SMT IR");
        }
    }

    template <class E>
    [[nodiscard]] smt::script expression(const E& e) {
        smt::script out;
        auto t = detail::lower(out, e);
        smt::node_id k[] = {t};
        out.commands.push_back(smt::append(out, smt::kind::assert_, {}, 0, {}, k));
        return out;
    }
}
