#pragma once

// ============================================================================
// sanchaya/backend/petika_backend.hpp — In-Memory / WAL Storage Engine via Petika
// ============================================================================

#include "sanchaya/fwd.hpp"
#include "petika/petika.hpp"
#include "glaze/beve.hpp"
#include <string>
#include <string_view>
#include <optional>
#include <expected>
#include <type_traits>
#include <cstring>

namespace sanchaya::backend {

    template <class T>
    struct object_codec {
        static std::string encode(const T& val) {
            if constexpr (std::is_arithmetic_v<T>) {
                std::string buf(sizeof(T), '\0');
                std::memcpy(buf.data(), &val, sizeof(T));
                return buf;
            } else {
                std::string buf;
                auto ec = glz::write<glz::opts{.format = glz::BEVE}>(val, buf);
                if (ec) return {};
                return buf;
            }
        }

        static std::optional<T> decode(std::string_view buf) {
            if constexpr (std::is_arithmetic_v<T>) {
                if (buf.size() != sizeof(T)) return std::nullopt;
                T val;
                std::memcpy(static_cast<void*>(&val), buf.data(), sizeof(T));
                return val;
            } else {
                T val{};
                auto ec = glz::read<glz::opts{.format = glz::BEVE}>(val, buf);
                if (ec) return std::nullopt;
                return val;
            }
        }
    };


    template <class Key = std::string, class Value = std::string>
    class petika_storage_backend {
    public:
        explicit petika_storage_backend(std::string db_dir = "./petika_sanchaya_db")
            : options_{.db_dir = std::move(db_dir), .segment_size = 16 * 1024 * 1024, .sync_on_write = false},
              store_(options_) {}

        void set_fail_writes(bool fail) noexcept {
            fail_writes_ = fail;
        }

        auto put(const Key& key, const Value& val) -> std::expected<bool, sanchaya_error> {
            if (fail_writes_) {
                return std::unexpected(sanchaya_error{
                    .domain = error_domain::storage,
                    .code = 500,
                    .message = "Injected Petika write failure"
                });
            }
            auto res = store_.put(key, val);
            if (!res) {
                return std::unexpected(sanchaya_error{
                    .domain = error_domain::storage,
                    .code = 500,
                    .message = "Petika put failed"
                });
            }
            return true;
        }

        auto get(const Key& key) -> std::expected<std::optional<Value>, sanchaya_error> {
            auto res = store_.get(key);
            if (res.has_value()) {
                return std::optional<Value>{*res};
            }
            return std::optional<Value>{std::nullopt};
        }

        auto erase(const Key& key) -> std::expected<bool, sanchaya_error> {
            auto res = store_.erase(key);
            if (!res) {
                return std::unexpected(sanchaya_error{
                    .domain = error_domain::storage,
                    .code = 500,
                    .message = "Petika erase failed"
                });
            }
            return true;
        }

        template <class Fn>
        void scan(std::string_view prefix, Fn&& fn) {
            store_.engine().for_each([&](const auto& entry) {
                std::string_view k{entry.key};
                if (k.starts_with(prefix)) {
                    fn(k, std::string_view{entry.value});
                }
            });
        }

        template <class Fn>
        void scan_prefix(std::string_view prefix, Fn&& fn) {
            scan(prefix, std::forward<Fn>(fn));
        }

    private:
        petika::PetikaOptions options_;
        petika::SkipStore<Key, Value> store_;
        bool fail_writes_{false};
    };

    template <class Key = std::string, class Value = std::string>
    using petika_kv_backend = petika_storage_backend<Key, Value>;

} // namespace sanchaya::backend

