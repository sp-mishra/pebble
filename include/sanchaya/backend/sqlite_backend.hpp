#pragma once

// ============================================================================
// sanchaya/backend/sqlite_backend.hpp — Embedded OLTP Backend via SQLite Amalgamation
// ============================================================================

#include "sanchaya/fwd.hpp"
#include <string>
#include <string_view>
#include <expected>
#include <vector>
#include <type_traits>
#include <limits>

#if __has_include("sqlite/sqlite3.h")
#include "sqlite/sqlite3.h"
inline constexpr bool has_sqlite_support = true;
#elif __has_include(<sqlite3.h>)
#include <sqlite3.h>
inline constexpr bool has_sqlite_support = true;
#else
inline constexpr bool has_sqlite_support = false;
#endif

namespace sanchaya::backend {

    using native_stmt_handle = std::conditional_t<has_sqlite_support, sqlite3_stmt*, void*>;
    using native_db_handle   = std::conditional_t<has_sqlite_support, sqlite3*, void*>;

    class sqlite_statement {
    public:
        sqlite_statement() = default;

        explicit sqlite_statement(native_stmt_handle stmt) noexcept : stmt_(stmt) {}

        ~sqlite_statement() {
            reset();
        }

        sqlite_statement(const sqlite_statement&) = delete;
        sqlite_statement& operator=(const sqlite_statement&) = delete;

        sqlite_statement(sqlite_statement&& other) noexcept : stmt_(other.stmt_) {
            other.stmt_ = nullptr;
        }

        sqlite_statement& operator=(sqlite_statement&& other) noexcept {
            if (this != &other) {
                reset();
                stmt_ = other.stmt_;
                other.stmt_ = nullptr;
            }
            return *this;
        }

        auto bind(int index, int val) -> std::expected<void, sanchaya_error> {
            if constexpr (has_sqlite_support) {
                if (!stmt_) return std::unexpected(sanchaya_error{.domain = error_domain::storage, .code = 400, .message = "Null statement"});
                int rc = sqlite3_bind_int(stmt_, index, val);
                if (rc != SQLITE_OK) return std::unexpected(sanchaya_error{.domain = error_domain::storage, .code = static_cast<std::uint32_t>(rc), .message = "Bind int error"});
                return {};
            } else {
                (void)index; (void)val;
                return std::unexpected(sanchaya_error{.domain = error_domain::storage, .code = 501, .message = "SQLite not supported"});
            }
        }

        auto bind(int index, std::int64_t val) -> std::expected<void, sanchaya_error> {
            if constexpr (has_sqlite_support) {
                if (!stmt_) return std::unexpected(sanchaya_error{.domain = error_domain::storage, .code = 400, .message = "Null statement"});
                int rc = sqlite3_bind_int64(stmt_, index, val);
                if (rc != SQLITE_OK) return std::unexpected(sanchaya_error{.domain = error_domain::storage, .code = static_cast<std::uint32_t>(rc), .message = "Bind int64 error"});
                return {};
            } else {
                (void)index; (void)val;
                return std::unexpected(sanchaya_error{.domain = error_domain::storage, .code = 501, .message = "SQLite not supported"});
            }
        }

        auto bind(int index, std::uint64_t val) -> std::expected<void, sanchaya_error> {
            if (val > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                return std::unexpected(sanchaya_error{
                    .domain = error_domain::storage,
                    .code = 400,
                    .message = "Integer exceeds SQLite int64 capacity"
                });
            }
            return bind(index, static_cast<std::int64_t>(val));
        }

        auto bind_null(int index) -> std::expected<void, sanchaya_error> {
            if constexpr (has_sqlite_support) {
                if (!stmt_) return std::unexpected(sanchaya_error{.domain = error_domain::storage, .code = 400, .message = "Null statement"});
                int rc = sqlite3_bind_null(stmt_, index);
                if (rc != SQLITE_OK) return std::unexpected(sanchaya_error{.domain = error_domain::storage, .code = static_cast<std::uint32_t>(rc), .message = "Bind null error"});
                return {};
            } else {
                (void)index;
                return std::unexpected(sanchaya_error{.domain = error_domain::storage, .code = 501, .message = "SQLite not supported"});
            }
        }

        auto bind(int index, double val) -> std::expected<void, sanchaya_error> {
            if constexpr (has_sqlite_support) {
                if (!stmt_) return std::unexpected(sanchaya_error{.domain = error_domain::storage, .code = 400, .message = "Null statement"});
                int rc = sqlite3_bind_double(stmt_, index, val);
                if (rc != SQLITE_OK) return std::unexpected(sanchaya_error{.domain = error_domain::storage, .code = static_cast<std::uint32_t>(rc), .message = "Bind double error"});
                return {};
            } else {
                (void)index; (void)val;
                return std::unexpected(sanchaya_error{.domain = error_domain::storage, .code = 501, .message = "SQLite not supported"});
            }
        }

        auto bind(int index, std::string_view val) -> std::expected<void, sanchaya_error> {
            if constexpr (has_sqlite_support) {
                if (!stmt_) return std::unexpected(sanchaya_error{.domain = error_domain::storage, .code = 400, .message = "Null statement"});
                int rc = sqlite3_bind_text(stmt_, index, val.data(), static_cast<int>(val.size()), SQLITE_TRANSIENT);
                if (rc != SQLITE_OK) return std::unexpected(sanchaya_error{.domain = error_domain::storage, .code = static_cast<std::uint32_t>(rc), .message = "Bind text error"});
                return {};
            } else {
                (void)index; (void)val;
                return std::unexpected(sanchaya_error{.domain = error_domain::storage, .code = 501, .message = "SQLite not supported"});
            }
        }

        auto step() -> std::expected<bool, sanchaya_error> {
            if constexpr (has_sqlite_support) {
                if (!stmt_) return std::unexpected(sanchaya_error{.domain = error_domain::storage, .code = 400, .message = "Null statement"});
                int rc = sqlite3_step(stmt_);
                if (rc == SQLITE_ROW) return true;
                if (rc == SQLITE_DONE) {
                    sqlite3_reset(stmt_);
                    return false;
                }
                return std::unexpected(sanchaya_error{.domain = error_domain::storage, .code = static_cast<std::uint32_t>(rc), .message = "Step error"});
            } else {
                return std::unexpected(sanchaya_error{.domain = error_domain::storage, .code = 501, .message = "SQLite not supported"});
            }
        }

        void reset() noexcept {
            if constexpr (has_sqlite_support) {
                if (stmt_) {
                    sqlite3_finalize(stmt_);
                    stmt_ = nullptr;
                }
            }
        }

        [[nodiscard]] bool valid() const noexcept { return stmt_ != nullptr; }

    private:
        native_stmt_handle stmt_{nullptr};
    };

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
            if constexpr (has_sqlite_support) {
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
            } else {
                (void)path;
                return std::unexpected(sanchaya_error{
                    .domain = error_domain::storage,
                    .code = 501,
                    .message = "SQLite dependency not compiled into current target"
                });
            }
        }

        auto prepare(std::string_view sql) -> std::expected<sqlite_statement, sanchaya_error> {
            if constexpr (has_sqlite_support) {
                if (!is_open_ || db_ == nullptr) {
                    return std::unexpected(sanchaya_error{
                        .domain = error_domain::storage,
                        .code = 400,
                        .message = "SQLite database not open"
                    });
                }
                sqlite3_stmt* stmt = nullptr;
                int rc = sqlite3_prepare_v2(db_, sql.data(), static_cast<int>(sql.size()), &stmt, nullptr);
                if (rc != SQLITE_OK) {
                    return std::unexpected(sanchaya_error{
                        .domain = error_domain::storage,
                        .code = static_cast<std::uint32_t>(rc),
                        .message = sqlite3_errmsg(db_)
                    });
                }
                return sqlite_statement(stmt);
            } else {
                (void)sql;
                return std::unexpected(sanchaya_error{
                    .domain = error_domain::storage,
                    .code = 501,
                    .message = "SQLite dependency not compiled into current target"
                });
            }
        }

        auto execute(std::string_view sql) -> std::expected<bool, sanchaya_error> {
            if constexpr (has_sqlite_support) {
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
            } else {
                (void)sql;
                return std::unexpected(sanchaya_error{
                    .domain = error_domain::storage,
                    .code = 501,
                    .message = "SQLite dependency not compiled into current target"
                });
            }
        }

        template <class Callback>
        auto execute_query(std::string_view sql, Callback&& callback) -> std::expected<bool, sanchaya_error> {
            if constexpr (has_sqlite_support) {
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
            } else {
                (void)sql;
                (void)callback;
                return std::unexpected(sanchaya_error{
                    .domain = error_domain::storage,
                    .code = 501,
                    .message = "SQLite dependency not compiled into current target"
                });
            }
        }

        void close() noexcept {
            if constexpr (has_sqlite_support) {
                if (db_ != nullptr) {
                    sqlite3_close(db_);
                    db_ = nullptr;
                }
            }
            is_open_ = false;
        }

        [[nodiscard]] bool is_open() const noexcept { return is_open_; }

    private:
        native_db_handle db_{nullptr};
        bool is_open_{false};
    };

} // namespace sanchaya::backend
