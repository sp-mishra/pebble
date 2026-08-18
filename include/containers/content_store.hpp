#pragma once

// =============================================================================
// containers/content_store.hpp — generic content-addressed blob store (G3)
//
// Provides:
//   content_store concept     — put/get/contains/erase over content-addressed bytes
//   blob_address              — content digest (32 bytes, SHA-256)
//   blob_handle               — non-owning span view into mmapped or buffered bytes
//   store_error               — error type for content_store operations
//   filesystem_content_store  — atomic-rename sharded layout, Setu zero-copy get()
//
// Key properties:
//   • Identical bytes → same address (dedupe for free).
//   • put() writes to temp file + atomic rename → no torn writes.
//   • get() returns a Setu region_view (zero-copy mmap, not heap read).
//   • Address = content_digest<sha256_digest_policy> from canonical_codec.hpp.
//   • Sharded layout: <root>/<aa>/<bb>/<full-hex-digest> (2+2 prefix sharding).
//   • Zero Lithe types: fully generic, no dependency on portable_module or artifacts.
//
// Guard: __has_include check for Setu is not required — Setu is always available.
// No virtual, no macros. Header-only C++23. macOS first.
// =============================================================================

#include <array>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>

#include "canonical_codec.hpp"   // content_digest, sha256_digest_policy
#include "../utils/setu.hpp"     // region_view / mmap zero-copy read

namespace containers {
    // =============================================================================
    // blob_address — 32-byte SHA-256 content digest (stable, serializable)
    // =============================================================================

    struct blob_address {
        std::array<std::uint8_t, 32> digest{};

        [[nodiscard]] bool operator==(const blob_address&) const noexcept = default;

        [[nodiscard]] std::string hex() const {
            static constexpr char kHex[] = "0123456789abcdef";
            std::string s;
            s.reserve(64);
            for (const auto b : digest) {
                s.push_back(kHex[(b >> 4) & 0xf]);
                s.push_back(kHex[b & 0xf]);
            }
            return s;
        }

        [[nodiscard]] static blob_address from_hex(std::string_view h) noexcept {
            blob_address a;
            if (h.size() != 64) return a;
            auto nibble = [](char c) -> std::uint8_t {
                if (c >= '0' && c <= '9') return static_cast<std::uint8_t>(c - '0');
                if (c >= 'a' && c <= 'f') return static_cast<std::uint8_t>(c - 'a' + 10);
                if (c >= 'A' && c <= 'F') return static_cast<std::uint8_t>(c - 'A' + 10);
                return 0;
            };
            for (std::size_t i = 0; i < 32; ++i)
                a.digest[i] = static_cast<std::uint8_t>((nibble(h[2 * i]) << 4) | nibble(h[2 * i + 1]));
            return a;
        }
    };

    // =============================================================================
    // store_error
    // =============================================================================

    enum class store_error_code : std::uint8_t {
        not_found,
        io_error,
        integrity_error,
    };

    struct store_error {
        store_error_code code = store_error_code::io_error;
        std::string detail;

        [[nodiscard]] static store_error not_found() {
            return {store_error_code::not_found, "not found"};
        }

        [[nodiscard]] static store_error io(std::string d) {
            return {store_error_code::io_error, std::move(d)};
        }

        [[nodiscard]] static store_error integrity(std::string d) {
            return {store_error_code::integrity_error, std::move(d)};
        }
    };

    // =============================================================================
    // blob_handle — non-owning view over retrieved bytes (lifetime tied to mapping)
    // =============================================================================

    struct blob_handle {
        std::span<const std::uint8_t> data;

        [[nodiscard]] bool empty() const noexcept { return data.empty(); }
        [[nodiscard]] std::size_t size() const noexcept { return data.size(); }
    };

    // =============================================================================
    // content_store concept
    // =============================================================================

    template <class S>
    concept content_store =
        requires(S& s, std::span<const std::uint8_t> bytes, const blob_address& a) {
            { s.put(bytes) } -> std::same_as<std::expected<blob_address, store_error>>;
            { s.get(a) } -> std::same_as<std::expected<blob_handle, store_error>>;
            { s.contains(a) } -> std::same_as<bool>;
            { s.erase(a) } -> std::same_as<std::expected<void, store_error>>;
        };

    // =============================================================================
    // filesystem_content_store
    //
    // Layout: <root>/<aa>/<bb>/<full-64-char-hex>
    //   where aa = first two hex chars of digest, bb = next two.
    // Writes: temp file in <root>/tmp/<random>, atomic rename on success.
    // Reads:  zero-copy via Setu read-only mapping.
    // =============================================================================

    class filesystem_content_store {
    public:
        explicit filesystem_content_store(std::filesystem::path root)
            : root_(std::move(root)) {
            std::filesystem::create_directories(root_ / "tmp");
        }

        // -------------------------------------------------------------------------
        // put — hash bytes, write to sharded path, return address.
        // Dedupe: if the file already exists the write is skipped (address returned).
        // Atomicity: write to temp + rename (never exposes partial content).
        // -------------------------------------------------------------------------
        [[nodiscard]] std::expected<blob_address, store_error>
        put(std::span<const std::uint8_t> bytes) {
            const auto addr = compute_address(bytes);
            const auto dest = blob_path(addr);

            if (std::filesystem::exists(dest))
                return addr; // dedupe

            std::filesystem::create_directories(dest.parent_path());

            // Write to a temp path, then rename atomically.
            const auto tmp = tmp_path(addr);
            {
                std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
                if (!f)
                    return std::unexpected(store_error::io("put: cannot open temp file"));
                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
                f.write(reinterpret_cast<const char*>(bytes.data()),
                        static_cast<std::streamsize>(bytes.size()));
                if (!f)
                    return std::unexpected(store_error::io("put: write failed"));
            }

            std::error_code ec;
            std::filesystem::rename(tmp, dest, ec);
            if (ec) {
                std::filesystem::remove(tmp, ec);
                return std::unexpected(store_error::io("put: rename failed: " + ec.message()));
            }
            return addr;
        }

        // -------------------------------------------------------------------------
        // get — zero-copy Setu mmap. Returned blob_handle lifetime = mapping lifetime.
        // The mapping is stored in mappings_ to keep it alive while the store is alive.
        // -------------------------------------------------------------------------
        [[nodiscard]] std::expected<blob_handle, store_error>
        get(const blob_address& addr) {
            const auto p = blob_path(addr);
            if (!std::filesystem::exists(p))
                return std::unexpected(store_error::not_found());

            auto map_result = setu::mapping<setu::read_only>::open_existing(p);
            if (!map_result)
                return std::unexpected(store_error::io("get: mmap failed"));

            // Obtain a const byte span from the mapping (std::byte → uint8_t cast).
            const auto bytes_span = map_result->as_bytes();
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
            const auto* u8 = reinterpret_cast<const std::uint8_t*>(bytes_span.data());

            blob_handle h{std::span<const std::uint8_t>{u8, bytes_span.size()}};
            mappings_.emplace_back(std::move(*map_result));
            return h;
        }

        [[nodiscard]] bool contains(const blob_address& addr) const {
            return std::filesystem::exists(blob_path(addr));
        }

        [[nodiscard]] std::expected<void, store_error>
        erase(const blob_address& addr) {
            std::error_code ec;
            std::filesystem::remove(blob_path(addr), ec);
            if (ec && ec != std::errc::no_such_file_or_directory)
                return std::unexpected(store_error::io("erase: " + ec.message()));
            return {};
        }

    private:
        std::filesystem::path root_;
        // Keep mappings alive so blob_handle spans remain valid.
        std::vector<setu::mapping<setu::read_only>> mappings_;

        [[nodiscard]] static blob_address
        compute_address(std::span<const std::uint8_t> bytes) noexcept {
            const auto hash = containers::content_digest<containers::sha256_digest_policy>(bytes);
            blob_address addr;
            static_assert(hash.size() >= 32);
            std::copy_n(hash.begin(), 32, addr.digest.begin());
            return addr;
        }

        [[nodiscard]] std::filesystem::path blob_path(const blob_address& addr) const {
            const auto h = addr.hex();
            return root_ / h.substr(0, 2) / h.substr(2, 2) / h;
        }

        [[nodiscard]] std::filesystem::path tmp_path(const blob_address& addr) const {
            return root_ / "tmp" / addr.hex();
        }
    };

    static_assert(content_store<filesystem_content_store>);
} // namespace containers
