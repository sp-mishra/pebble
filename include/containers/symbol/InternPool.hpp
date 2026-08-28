#pragma once


// ============================================================================
// InternPool — thread-safe string intern pool
//
// Stores each unique string exactly once. Returns a stable std::string_view
// (pointer into the pool) so callers can use pointer-equality instead of full
// string comparison on hot paths.
//
// Stability guarantee
// -------------------
//   std::unordered_set<std::string> is node-based: insert() never moves
//   existing nodes, so all previously returned string_views remain valid
//   for the lifetime of the pool.
//
// Thread safety
// -------------
//   intern()    — exclusive write lock (only on cache miss; readers unaffected
//                 while another thread is registering a new string)
//   contains()  — shared read lock
//   size()      — shared read lock
//   clear()     — exclusive write lock
//   all()       — caller must hold the pool alive; snapshot is unlocked copy
//
// Usage
//   symtab::InternPool pool;
//   auto sv1 = pool.intern("hello");
//   auto sv2 = pool.intern("hello");
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <expected>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "mem/smriti.hpp"
#include "mem/arena.hpp"

#ifdef SYMTAB_ENABLE_HIGHWAY
#include <span>
#endif

namespace symtab {
    // ============================================================================
    // Transparent hash/equal — lets unordered_set<string> accept string_view keys
    // without constructing a temporary std::string.
    // ============================================================================

    struct StringHash {
        using is_transparent = void;

        std::size_t operator()(const std::string_view sv) const noexcept {
            return std::hash<std::string_view>{}(sv);
        }

        std::size_t operator()(const std::string& s) const noexcept {
            return std::hash<std::string_view>{}(s);
        }
    };

    struct StringEqual {
        using is_transparent = void;

        bool operator()(const std::string_view a, const std::string_view b) const noexcept {
            return a == b;
        }
    };

    // ============================================================================
    // Errors
    // ============================================================================

    enum class InternError : std::uint8_t {
        EmptyString, // intern("") is rejected
        PoolCleared, // a previously returned view was used after clear()
    };

    template <typename T>
    using InternResult = std::expected<T, InternError>;

    // ============================================================================
    // InternPool
    // ============================================================================

    class InternPool {
    public:
        InternPool() = default;

        explicit InternPool(const std::size_t initial_capacity) {
            store_.reserve(initial_capacity);
        }

        ~InternPool() = default;

        InternPool(const InternPool&) = delete;

        InternPool& operator=(const InternPool&) = delete;

        // Move ctor: lock both to prevent data races on concurrent intern() calls.
        InternPool(InternPool&& other) noexcept {
            std::scoped_lock lk(mtx_, other.mtx_);
            arena_ = std::move(other.arena_);
            store_ = std::move(other.store_);
            intern_count_.store(other.intern_count_.load(std::memory_order_relaxed),
                                std::memory_order_relaxed);
            other.intern_count_.store(0, std::memory_order_relaxed);
        }

        InternPool& operator=(InternPool&& other) noexcept {
            if (this != &other) {
                std::scoped_lock lk(mtx_, other.mtx_);
                arena_ = std::move(other.arena_);
                store_ = std::move(other.store_);
                intern_count_.store(other.intern_count_.load(std::memory_order_relaxed),
                                    std::memory_order_relaxed);
                other.intern_count_.store(0, std::memory_order_relaxed);
            }
            return *this;
        }

        // -------------------------------------------------------------------------
        // intern — fast path: shared read lock, pointer lookup.
        // Slow path (new string): upgrades to write lock, inserts into smriti arena.
        // Returns std::unexpected(EmptyString) if s is empty.
        // intern_count_ counts every intern() call (including cache hits).
        // -------------------------------------------------------------------------
        [[nodiscard]] InternResult<std::string_view> intern(std::string_view s) {
            if (s.empty()) return std::unexpected(InternError::EmptyString);

            ++intern_count_; // total call count, including fast-path hits

            // Fast path: already interned?
            {
                std::shared_lock rl(mtx_);
                if (const auto it = store_.find(s); it != store_.end()) {
                    return *it;
                }
            }

            // Slow path: copy string bytes into bump arena under exclusive lock.
            std::unique_lock wl(mtx_);
            // Re-check after acquiring write lock (another thread may have raced).
            if (const auto it = store_.find(s); it != store_.end()) {
                return *it;
            }

            void* mem = arena_.allocate(s.size() + 1, alignof(char));
            if (!mem) {
                // Fallback: if arena exhausted, store with std::string copy
                return std::unexpected(InternError::EmptyString);
            }
            char* dest = static_cast<char*>(mem);
            std::memcpy(dest, s.data(), s.size());
            dest[s.size()] = '\0';
            std::string_view interned_view{dest, s.size()};

            store_.insert(interned_view);
            return interned_view;
        }

        // intern_or_throw: asserts s is non-empty; throws std::bad_expected_access
        // on empty string. Use only where emptiness is a programming error.
        [[nodiscard]] std::string_view intern_or_throw(const std::string_view s) {
            return intern(s).value();
        }

        [[nodiscard]] bool contains(const std::string_view s) const {
            std::shared_lock rl(mtx_);
            return store_.contains(s);
        }

        [[nodiscard]] std::size_t size() const {
            std::shared_lock rl(mtx_);
            return store_.size();
        }

        // Total number of intern() calls (including cache hits).
        [[nodiscard]] std::size_t intern_call_count() const noexcept {
            return intern_count_.load(std::memory_order_relaxed);
        }

        // Pre-size the internal hash set to avoid rehash on bulk loads.
        void reserve(const std::size_t n) {
            std::unique_lock wl(mtx_);
            store_.reserve(n);
        }

        // WARNING: invalidates all previously returned string_views.
        void clear() {
            std::unique_lock wl(mtx_);
            store_.clear();
            arena_.reset();
            intern_count_.store(0, std::memory_order_relaxed);
        }

        // Returns a snapshot copy of all interned strings. Unlocked after copy.
        [[nodiscard]] std::vector<std::string> all() const {
            std::shared_lock rl(mtx_);
            std::vector<std::string> res;
            res.reserve(store_.size());
            for (const auto& sv : store_) {
                res.emplace_back(sv);
            }
            return res;
        }

        // Returns total bytes allocated across arena pages
        [[nodiscard]] std::size_t bytes_allocated() const noexcept {
            return arena_.used_bytes();
        }

#ifdef SYMTAB_ENABLE_HIGHWAY
        // -------------------------------------------------------------------------
        // batch_intern — bulk interning.
        //
        // Interns each non-null entry in `names` and writes the resulting
        // string_view into `out`.
        //
        // Preconditions:
        //   names.size() == out.size()
        //   every pointer in names is non-null and null-terminated
        //
        // Returns the count of failed interns (non-empty but rejected entries).
        // -------------------------------------------------------------------------
        [[nodiscard]] std::size_t batch_intern(std::span<const char* const> names,
                                               std::span<std::string_view> out) {
            assert(names.size() == out.size());
            std::size_t n = names.size();
            std::size_t failed = 0;
            for (std::size_t i = 0; i < n; ++i) {
                auto r = intern(std::string_view{names[i], std::strlen(names[i])});
                if (r.has_value()) {
                    out[i] = *r;
                }
                else {
                    out[i] = std::string_view{};
                    ++failed;
                }
            }
            return failed;
        }
#endif // SYMTAB_ENABLE_HIGHWAY

    private:
        mutable std::shared_mutex mtx_;
        smriti::pools::BumpPool<smriti::domains::SystemRAMDomain> arena_{64 * 1024};
        std::unordered_set<std::string_view, StringHash, StringEqual> store_;
        std::atomic<std::size_t> intern_count_{0};
    };
} // namespace symtab
