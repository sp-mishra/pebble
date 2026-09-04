#pragma once

// ============================================================================
// sanchaya/backend/sqlite_backend.hpp — Embedded OLTP Backend via SQLite Amalgamation
// ============================================================================

#include "sanchaya/fwd.hpp"
#include <string>
#include <expected>
#include <vector>

#if __has_include("sqlite/sqlite3.h")
#include "sqlite/sqlite3.h"
#define SANCHAYA_HAS_SQLITE 1
#elif __has_include(<sqlite3.h>)
#include <sqlite3.h>
#define SANCHAYA_HAS_SQLITE 1
#else
#define SANCHAYA_HAS_SQLITE 0
#endif

namespace sanchaya::backend {

    class sqlite_storage_backend {
    public:
        sqlite_storage_backend() = default;

        explicit sqlite_storage_backend(const std::string& path) {
            open(path);
        }

        ~sqlite_storage_backend() {
            close();
        }

        sqlite_storage_backend(const sqlite_storage_backend&) = delete;
        sqlite_storage_backend& operator=(const sqlite_storage_backend&) = delete;

        sqlite_storage_backend(sqlite_storage_backend&& other) noexcept
            : db_(other.db_), is_open_(other.is_open_) {
            other.db_ = nullptr;
            other.is_open_ = false;
        }

        sqlite_storage_backend& operator=(sqlite_storage_backend&& other) noexcept {
            if (this != &other) {
                close();
                db_ = other.db_;
                is_open_ = other.is_open_;
                other.db_ = nullptr;
                other.is_open_ = false;
            }
            return *this;
        }

        auto open(const std::string& path) -> std::expected<bool, sanchaya_error> {
#if SANCHAYA_HAS_SQLITE
            if (is_open_) close();
            int rc = sqlite3_open(path.c_str(), &db_);
            if (rc != SQLITE_OK) {
                sanchaya_error err{
                    .domain = error_domain::storage,
                    .code = static_cast<std::uint32_t>(rc),
                    .message = sqlite3_errmsg(db_)
                };
                close();
                return std::unexpected(err);
            }
            is_open_ = true;
            return true;
#else
            return std::unexpected(sanchaya_error{
                .domain = error_domain::storage,
                .code = 501,
                .message = "SQLite dependency not compiled into current target"
            });
#endif
        }

        auto execute(std::string_view sql) -> std::expected<bool, sanchaya_error> {
#if SANCHAYA_HAS_SQLITE
            if (!is_open_ || db_ == nullptr) {
                return std::unexpected(sanchaya_error{
                    .domain = error_domain::storage,
                    .code = 400,
                    .message = "SQLite database not open"
                });
            }
            char* err_msg = nullptr;
            std::string sql_str(sql);
            int rc = sqlite3_exec(db_, sql_str.c_str(), nullptr, nullptr, &err_msg);
            if (rc != SQLITE_OK) {
                sanchaya_error err{
                    .domain = error_domain::storage,
                    .code = static_cast<std::uint32_t>(rc),
                    .message = err_msg ? err_msg : "SQLite execution error"
                };
                if (err_msg) sqlite3_free(err_msg);
                return std::unexpected(err);
            }
            return true;
#else
            return std::unexpected(sanchaya_error{
                .domain = error_domain::storage,
                .code = 501,
                .message = "SQLite dependency not compiled into current target"
            });
#endif
        }

        template <class Callback>
        auto execute_query(std::string_view sql, Callback&& callback) -> std::expected<bool, sanchaya_error> {
#if SANCHAYA_HAS_SQLITE
            if (!is_open_ || db_ == nullptr) {
                return std::unexpected(sanchaya_error{
                    .domain = error_domain::storage,
                    .code = 400,
                    .message = "SQLite database not open"
                });
            }
            auto bridge = [](void* data, int argc, char** argv, char** col_names) -> int {
                auto& cb = *static_cast<std::decay_t<Callback>*>(data);
                cb(argc, argv, col_names);
                return 0;
            };
            char* err_msg = nullptr;
            std::string sql_str(sql);
            int rc = sqlite3_exec(db_, sql_str.c_str(), bridge, &callback, &err_msg);
            if (rc != SQLITE_OK) {
                sanchaya_error err{
                    .domain = error_domain::storage,
                    .code = static_cast<std::uint32_t>(rc),
                    .message = err_msg ? err_msg : "SQLite query error"
                };
                if (err_msg) sqlite3_free(err_msg);
                return std::unexpected(err);
            }
            return true;
#else
            (void)sql;
            (void)callback;
            return std::unexpected(sanchaya_error{
                .domain = error_domain::storage,
                .code = 501,
                .message = "SQLite dependency not compiled into current target"
            });
#endif
        }

        void close() noexcept {
#if SANCHAYA_HAS_SQLITE
            if (db_ != nullptr) {
                sqlite3_close(db_);
                db_ = nullptr;
            }
            is_open_ = false;
#endif
        }

        [[nodiscard]] bool is_open() const noexcept { return is_open_; }

    private:
#if SANCHAYA_HAS_SQLITE
        sqlite3* db_{nullptr};
#else
        void* db_{nullptr};
#endif
        bool is_open_{false};
    };

} // namespace sanchaya::backend
