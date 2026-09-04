#pragma once

// ============================================================================
// sanchaya/engine/relational_engine.hpp — Parameterized SQL Dialect Emission
// ============================================================================

#include "sanchaya/fwd.hpp"
#include "sanchaya/planner/physical_ir.hpp"
#include <string>
#include <vector>
#include <sstream>

namespace sanchaya::engine {

    struct prepared_sql_artifact {
        std::string sql_text{};
        std::vector<std::string> parameters{};
        std::size_t estimated_rows{0};
    };

    class sqlite_dialect_emitter {
    public:
        template <class Entity>
        [[nodiscard]] static prepared_sql_artifact emit_scan(std::string_view table_name, std::size_t limit_val = 0) {
            prepared_sql_artifact artifact{};
            std::ostringstream ss;
            ss << "SELECT * FROM " << table_name;
            if (limit_val > 0) {
                ss << " LIMIT " << limit_val;
            }
            artifact.sql_text = ss.str();
            return artifact;
        }

        template <class Entity>
        [[nodiscard]] static prepared_sql_artifact emit_filtered_scan(
            std::string_view table_name,
            std::string_view where_clause,
            std::string_view param_val,
            std::size_t limit_val = 0)
        {
            prepared_sql_artifact artifact{};
            std::ostringstream ss;
            ss << "SELECT * FROM " << table_name << " WHERE " << where_clause;
            artifact.parameters.push_back(std::string(param_val));
            if (limit_val > 0) {
                ss << " LIMIT " << limit_val;
            }
            artifact.sql_text = ss.str();
            return artifact;
        }

        template <class Entity>
        [[nodiscard]] static prepared_sql_artifact emit_insert(
            std::string_view table_name,
            std::span<const std::string_view> col_names)
        {
            prepared_sql_artifact artifact{};
            std::ostringstream ss;
            ss << "INSERT INTO " << table_name << " (";
            for (std::size_t i = 0; i < col_names.size(); ++i) {
                if (i > 0) ss << ", ";
                ss << col_names[i];
            }
            ss << ") VALUES (";
            for (std::size_t i = 0; i < col_names.size(); ++i) {
                if (i > 0) ss << ", ";
                ss << "?";
            }
            ss << ");";
            artifact.sql_text = ss.str();
            return artifact;
        }
    };

    class duckdb_dialect_emitter {
    public:
        template <class Entity>
        [[nodiscard]] static prepared_sql_artifact emit_vectorized_scan(
            std::string_view table_name,
            std::string_view projection_cols,
            std::string_view filter_expr)
        {
            prepared_sql_artifact artifact{};
            std::ostringstream ss;
            ss << "SELECT " << projection_cols << " FROM " << table_name;
            if (!filter_expr.empty()) {
                ss << " WHERE " << filter_expr;
            }
            artifact.sql_text = ss.str();
            return artifact;
        }

        template <class Entity>
        [[nodiscard]] static prepared_sql_artifact emit_insert(
            std::string_view table_name,
            std::span<const std::string_view> col_names)
        {
            prepared_sql_artifact artifact{};
            std::ostringstream ss;
            ss << "INSERT INTO " << table_name << " (";
            for (std::size_t i = 0; i < col_names.size(); ++i) {
                if (i > 0) ss << ", ";
                ss << col_names[i];
            }
            ss << ") VALUES (";
            for (std::size_t i = 0; i < col_names.size(); ++i) {
                if (i > 0) ss << ", ";
                ss << "?";
            }
            ss << ");";
            artifact.sql_text = ss.str();
            return artifact;
        }
    };

} // namespace sanchaya::engine

