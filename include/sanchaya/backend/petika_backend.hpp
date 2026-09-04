#pragma once

// ============================================================================
// sanchaya/backend/petika_backend.hpp — In-Memory / WAL Storage Engine via Petika
// ============================================================================

#include "sanchaya/fwd.hpp"
#include "petika/petika.hpp"
#include <string>
#include <sstream>
#include <expected>

namespace sanchaya::backend {

    template <class T>
    struct object_codec {
        static std::string encode(const T& val) {
            if constexpr (std::is_trivially_copyable_v<T>) {
                std::string buf(sizeof(T), '\0');
                std::memcpy(buf.data(), &val, sizeof(T));
                return buf;
            } else if constexpr (requires { val.id; val.name; val.age; val.salary; val.department_id; val.address; }) {
                // Serializer for Employee
                std::ostringstream ss;
                ss << val.id.value << '\0'
                   << val.name << '\0'
                   << val.age << '\0'
                   << val.salary << '\0'
                   << val.department_id.value << '\0'
                   << val.address.city << '\0'
                   << val.address.country << '\0';
                return ss.str();
            } else {
                std::ostringstream ss;
                return ss.str();
            }
        }

        static std::optional<T> decode(std::string_view buf) {
            if constexpr (std::is_trivially_copyable_v<T>) {
                if (buf.size() != sizeof(T)) return std::nullopt;
                T val;
                std::memcpy(static_cast<void*>(&val), buf.data(), sizeof(T));
                return val;
            } else if constexpr (requires { std::declval<T>().id; std::declval<T>().name; std::declval<T>().age; std::declval<T>().salary; std::declval<T>().department_id; std::declval<T>().address; }) {
                T val{};
                std::string str(buf);
                std::vector<std::string> parts;
                std::size_t start = 0;
                while (start < str.size()) {
                    auto next = str.find('\0', start);
                    if (next == std::string::npos) {
                        parts.push_back(str.substr(start));
                        break;
                    }
                    parts.push_back(str.substr(start, next - start));
                    start = next + 1;
                }
                if (parts.size() >= 7) {
                    val.id.value = std::stoull(parts[0]);
                    val.name = parts[1];
                    val.age = std::stoi(parts[2]);
                    val.salary = std::stod(parts[3]);
                    val.department_id.value = std::stoull(parts[4]);
                    val.address.city = parts[5];
                    val.address.country = parts[6];
                    return val;
                }
                return std::nullopt;
            } else {
                return std::nullopt;
            }
        }
    };


    template <class Key = std::string, class Value = std::string>
    class petika_storage_backend {
    public:
        explicit petika_storage_backend(std::string db_dir = "./petika_sanchaya_db")
            : options_{.db_dir = std::move(db_dir), .segment_size = 16 * 1024 * 1024, .sync_on_write = false},
              store_(options_) {}

        auto put(const Key& key, const Value& val) -> std::expected<bool, sanchaya_error> {
            store_.put(key, val);
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
            store_.erase(key);
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
    };

    template <class Key = std::string, class Value = std::string>
    using petika_kv_backend = petika_storage_backend<Key, Value>;

} // namespace sanchaya::backend

