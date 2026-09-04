#pragma once

// ============================================================================
// sanchaya/backend/duckdb_backend.hpp — Embedded Analytical Backend via DuckDB
// ============================================================================

#include "sanchaya/fwd.hpp"
#include <string>
#include <expected>
#include <memory>

#if __has_include("libduckdb/duckdb.hpp")
#include "libduckdb/duckdb.hpp"
#define SANCHAYA_HAS_DUCKDB 1
#elif __has_include(<duckdb.hpp>)
#include <duckdb.hpp>
#define SANCHAYA_HAS_DUCKDB 1
#else
#define SANCHAYA_HAS_DUCKDB 0
#endif

namespace sanchaya::backend {

    class duckdb_analytical_backend {
    public:
        duckdb_analytical_backend() = default;

        explicit duckdb_analytical_backend(const std::string& path) {
            open(path);
        }

        auto open(const std::string& path) -> std::expected<bool, sanchaya_error> {
#if SANCHAYA_HAS_DUCKDB
            try {
                db_ = std::make_unique<duckdb::DuckDB>(path.empty() ? nullptr : path.c_str());
                conn_ = std::make_unique<duckdb::Connection>(*db_);
                is_open_ = true;
                return true;
            } catch (const std::exception& e) {
                return std::unexpected(sanchaya_error{
                    .domain = error_domain::storage,
                    .code = 500,
                    .message = e.what()
                });
            }
#else
            (void)path;
            return std::unexpected(sanchaya_error{
                .domain = error_domain::storage,
                .code = 501,
                .message = "DuckDB dependency not compiled into current target"
            });
#endif
        }

        auto query(std::string_view sql) -> std::expected<std::size_t, sanchaya_error> {
#if SANCHAYA_HAS_DUCKDB
            if (!is_open_ || !conn_) {
                return std::unexpected(sanchaya_error{
                    .domain = error_domain::storage,
                    .code = 400,
                    .message = "DuckDB instance not open"
                });
            }
            auto result = conn_->Query(std::string(sql));
            if (result->HasError()) {
                return std::unexpected(sanchaya_error{
                    .domain = error_domain::storage,
                    .code = 500,
                    .message = result->GetError()
                });
            }
            return result->RowCount();
#else
            (void)sql;
            return std::unexpected(sanchaya_error{
                .domain = error_domain::storage,
                .code = 501,
                .message = "DuckDB dependency not compiled into current target"
            });
#endif
        }

        [[nodiscard]] bool is_open() const noexcept { return is_open_; }

    private:
#if SANCHAYA_HAS_DUCKDB
        std::unique_ptr<duckdb::DuckDB> db_{nullptr};
        std::unique_ptr<duckdb::Connection> conn_{nullptr};
#endif
        bool is_open_{false};
    };

} // namespace sanchaya::backend
