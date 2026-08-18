// ============================================================================
// Setu - Memory Mapping Library for Systems Programming
// ============================================================================
// Version: 1.0.0
// C++23 header-only library
//
// Purpose:
//   Setu provides a modern, type-safe memory mapping abstraction for systems
//   programming, offering file-backed and anonymous mappings with page-oriented
//   helpers, typed access, and backend-extensible design.
//
// Design Goals:
//   - Easy to use for common cases
//   - Zero-copy, allocation-free views
//   - Type-safe and bounds-checked access
//   - Explicit semantics for flush/remap/durability
//   - Backend abstraction for cross-platform support
//
// Current Backend Support:
//   - macOS (POSIX mmap/munmap/msync/madvise)
//
// Safety Notes:
//   - Remap/resize operations invalidate all existing views, spans, and typed overlays
//   - Flush operations provide OS-level synchronization, not transaction durability
//   - Typed overlays are only safe for trivially copyable, standard-layout types
//   - All views are non-owning; lifetime is tied to the parent mapping
// ============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <expected>
#include <system_error>
#include <type_traits>
#include <filesystem>
#include <array>
#include <algorithm>
#include <optional>
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <bit>

// Platform detection
#if defined(__APPLE__) && defined(__MACH__)
#define SETU_PLATFORM_MACOS
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#else
#error "Currently only macOS is supported. Set up Linux or Windows backend to support other platforms."
#endif

namespace setu {
    // ============================================================================
    // Version Information
    // ============================================================================

    constexpr int version_major = 1;
    constexpr int version_minor = 0;
    constexpr int version_patch = 0;

    // ============================================================================
    // Access Mode Tags and Concepts
    // ============================================================================

    /// Tag type for read-only access mode
    struct read_only {
        static constexpr bool is_writable = false;
    };

    /// Tag type for read-write access mode
    struct read_write {
        static constexpr bool is_writable = true;
    };

    /// Tag type for copy-on-write access mode
    struct copy_on_write {
        static constexpr bool is_writable = true;
    };

    /// Concept for valid access modes
    template <typename T>
    concept access_mode =
        std::is_same_v<T, read_only> ||
        std::is_same_v<T, read_write> ||
        std::is_same_v<T, copy_on_write>;

    /// Concept for writable access modes
    template <typename T>
    concept writable_access_mode = access_mode<T> && T::is_writable;

    // ============================================================================
    // Core Enumerations
    // ============================================================================

    /// Flush synchronization mode
    enum class flush_mode : uint8_t {
        async, ///< Request asynchronous flush (MS_ASYNC)
        sync ///< Request synchronous flush (MS_SYNC)
    };

    /// Memory access pattern advice for OS optimization
    enum class advice_mode : uint8_t {
        normal, ///< No special treatment
        sequential, ///< Expect sequential access
        random, ///< Expect random access
        will_need, ///< Will need this data soon
        dont_need ///< Won't need this data anymore
    };

    /// Type of memory mapping
    enum class map_kind : uint8_t {
        file_backed, ///< Backed by a file on disk
        anonymous_private, ///< Anonymous private memory (MAP_PRIVATE | MAP_ANON)
        anonymous_shared ///< Anonymous shared memory (MAP_SHARED | MAP_ANON)
    };

    /// File opening mode
    enum class open_mode : uint8_t {
        open_existing, ///< Open existing file, fail if not present
        open_or_create, ///< Open existing or create new file
        create_truncate ///< Create new file, truncate if exists
    };

    /// Resize policy
    enum class resize_mode : uint8_t {
        preserve_existing, ///< Preserve existing content when resizing
        allow_shrink, ///< Allow shrinking the file
        grow_only ///< Only allow growing the file
    };

    // ============================================================================
    // Error Handling
    // ============================================================================

    /// Error codes specific to Setu operations
    enum class error_code : int {
        success = 0,
        invalid_argument,
        out_of_bounds,
        misaligned_access,
        map_failed,
        unmap_failed,
        flush_failed,
        resize_failed,
        remap_failed,
        unsupported_operation,
        permission_denied,
        file_not_found,
        file_too_small,
        invalid_layout,
        backend_error,
        type_constraint_violation
    };

    /// Custom error category for Setu
    class error_category_impl : public std::error_category {
    public:
        const char* name() const noexcept override {
            return "setu";
        }

        std::string message(int ev) const override {
            switch (static_cast<error_code>(ev)) {
            case error_code::success: return "Success";
            case error_code::invalid_argument: return "Invalid argument";
            case error_code::out_of_bounds: return "Out of bounds access";
            case error_code::misaligned_access: return "Misaligned access";
            case error_code::map_failed: return "Memory mapping failed";
            case error_code::unmap_failed: return "Memory unmapping failed";
            case error_code::flush_failed: return "Flush operation failed";
            case error_code::resize_failed: return "Resize operation failed";
            case error_code::remap_failed: return "Remap operation failed";
            case error_code::unsupported_operation: return "Unsupported operation";
            case error_code::permission_denied: return "Permission denied";
            case error_code::file_not_found: return "File not found";
            case error_code::file_too_small: return "File too small";
            case error_code::invalid_layout: return "Invalid layout";
            case error_code::backend_error: return "Backend error";
            case error_code::type_constraint_violation: return "Type constraint violation";
            default: return "Unknown error";
            }
        }
    };

    inline const std::error_category& error_category() {
        static error_category_impl instance;
        return instance;
    }

    inline std::error_code make_error_code(error_code e) {
        return {static_cast<int>(e), error_category()};
    }

    /// Result type alias for fallible operations
    template <typename T>
    using result = std::expected<T, std::error_code>;

    // ============================================================================
    // Option Structures
    // ============================================================================

    /// Options for opening a file mapping
    struct open_options {
        std::filesystem::path path;
        open_mode mode = open_mode::open_existing;
        std::size_t requested_size = 0;
        std::size_t initial_map_length = 0;
        std::size_t initial_offset = 0;
        mode_t create_permissions = 0644;
    };

    /// Options for memory mapping operations
    struct map_options {
        std::size_t offset = 0;
        std::size_t length = 0;
        bool prefault = false;
    };

    /// Options for flush operations
    struct flush_options {
        flush_mode mode = flush_mode::async;
        std::size_t offset = 0;
        std::size_t length = 0;
    };

    /// Options for advice operations
    struct advice_options {
        advice_mode mode = advice_mode::normal;
        std::size_t offset = 0;
        std::size_t length = 0;
    };

    /// Options for remap operations
    struct remap_options {
        std::size_t new_offset{};
        std::size_t new_length{};
        bool preserve_mode = true;
    };

    // ============================================================================
    // Forward Declarations
    // ============================================================================

    template <access_mode Mode>
    class mapping;

    class region_view;

    template <std::size_t PageSize>
    class page_view;

    template <typename T>
    class offset_ptr;

    namespace detail {
        struct posix_backend;
    }

    namespace platform {
        std::size_t page_size() noexcept;

        std::size_t allocation_granularity() noexcept;
    }

    namespace page {
        constexpr std::size_t align_up(const std::size_t value, const std::size_t alignment) noexcept {
            return (value + alignment - 1) & ~(alignment - 1);
        }

        constexpr std::size_t align_down(const std::size_t value, const std::size_t alignment) noexcept {
            return value & ~(alignment - 1);
        }

        constexpr bool is_aligned(const std::size_t value, const std::size_t alignment) noexcept {
            return (value & (alignment - 1)) == 0;
        }

        constexpr std::size_t page_index(const std::size_t offset, const std::size_t page_size) noexcept {
            return offset / page_size;
        }

        constexpr std::size_t page_offset(const std::size_t index, const std::size_t page_size) noexcept {
            return index * page_size;
        }

        constexpr std::size_t page_count(const std::size_t size, const std::size_t page_size) noexcept {
            return (size + page_size - 1) / page_size;
        }

        std::size_t page_count(const region_view& region, std::size_t page_size) noexcept;

        inline bool is_page_aligned(const std::size_t offset, const std::size_t page_size) noexcept {
            return is_aligned(offset, page_size);
        }

        inline std::size_t align_to_page(const std::size_t offset, const std::size_t page_size) noexcept {
            return align_up(offset, page_size);
        }

        inline std::size_t align_to_page_down(const std::size_t offset, const std::size_t page_size) noexcept {
            return align_down(offset, page_size);
        }
    }

    // ============================================================================
    // Type Constraints for Typed Access
    // ============================================================================

    template <typename T>
    concept mappable_type =
        std::is_trivially_copyable_v<T> &&
        std::is_standard_layout_v<T> &&
        !std::is_reference_v<T> &&
        !std::is_pointer_v<T> &&
        !std::is_function_v<T> &&
        !std::is_polymorphic_v<T> &&
        !std::is_volatile_v<T> &&
        !std::is_const_v<T>;

    // ============================================================================
    // Platform Utilities Implementation
    // ============================================================================

    namespace platform {
        inline std::size_t page_size() noexcept {
#ifdef SETU_PLATFORM_MACOS
            static const auto ps = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
            return ps;
#else
#error "Platform page_size() not implemented for this platform"
#endif
        }

        inline std::size_t allocation_granularity() noexcept {
            return page_size();
        }
    } // namespace platform

    // ============================================================================
    // Backend Abstraction
    // ============================================================================

    namespace detail {
        // Overflow-safe bounds checking helpers

        inline bool is_valid_range(const std::size_t offset, const std::size_t size, const std::size_t limit) noexcept {
            if (offset > limit) return false;
            if (size > limit - offset) return false;
            return true;
        }

        inline bool is_valid_array_range(const std::size_t offset, const std::size_t count,
                                         const std::size_t element_size,
                                         const std::size_t limit) noexcept {
            if (offset > limit) return false;

            if (count == 0 || element_size == 0) {
                return true;
            }

            if (count > SIZE_MAX / element_size) return false;

            const std::size_t total_size = count * element_size;
            if (total_size > limit - offset) return false;

            return true;
        }

        // Centralized Typed Access Validation Helpers

        template <typename T>
        [[nodiscard]] inline bool is_aligned_for_type(const void* ptr) noexcept {
            return (reinterpret_cast<std::uintptr_t>(ptr) % alignof(T)) == 0;
        }

        template <typename T>
        [[nodiscard]] inline std::optional<std::size_t> safe_array_byte_size(const std::size_t count) noexcept {
            if (count == 0) {
                return 0;
            }

            constexpr std::size_t max_count = SIZE_MAX / sizeof(T);
            if (count > max_count) {
                return std::nullopt;
            }

            return sizeof(T) * count;
        }

        template <typename T>
        [[nodiscard]] inline std::error_code validate_single_typed_access(
            const std::byte* base,
            const std::size_t total_size,
            const std::size_t offset
        ) noexcept {
            if (!is_valid_range(offset, sizeof(T), total_size)) {
                return make_error_code(error_code::out_of_bounds);
            }

            const void* target = base + offset;
            if (!is_aligned_for_type<T>(target)) {
                return make_error_code(error_code::misaligned_access);
            }

            return {};
        }

        template <typename T>
        [[nodiscard]] inline std::error_code validate_array_typed_access(
            const std::byte* base,
            const std::size_t total_size,
            const std::size_t offset,
            const std::size_t count
        ) noexcept {
            if (!is_valid_array_range(offset, count, sizeof(T), total_size)) {
                return make_error_code(error_code::out_of_bounds);
            }

            if (count == 0) {
                return {};
            }

            const void* target = base + offset;
            if (!is_aligned_for_type<T>(target)) {
                return make_error_code(error_code::misaligned_access);
            }

            return {};
        }

        /// Backend result for mapping operations
        struct map_result {
            void* base_address = nullptr;
            std::size_t physical_offset = 0;
            std::size_t physical_size = 0;
            std::size_t logical_offset = 0;
            std::size_t logical_size = 0;
        };

        /// RAII file handle wrapper
        class file_handle {
        public:
            file_handle() = default;

            explicit file_handle(const int fd, const std::size_t size = 0) noexcept
                : fd_(fd), size_(size) {}

            ~file_handle() {
                close();
            }

            file_handle(const file_handle&) = delete;

            file_handle& operator=(const file_handle&) = delete;

            file_handle(file_handle&& other) noexcept
                : fd_(other.fd_), size_(other.size_) {
                other.fd_ = -1;
                other.size_ = 0;
            }

            file_handle& operator=(file_handle&& other) noexcept {
                if (this != &other) {
                    close();
                    fd_ = other.fd_;
                    size_ = other.size_;
                    other.fd_ = -1;
                    other.size_ = 0;
                }
                return *this;
            }

            [[nodiscard]] bool is_valid() const noexcept { return fd_ >= 0; }
            [[nodiscard]] int fd() const noexcept { return fd_; }
            [[nodiscard]] std::size_t size() const noexcept { return size_; }

            void update_size(const std::size_t new_size) noexcept { size_ = new_size; }

            void close() noexcept {
                if (fd_ >= 0) {
                    ::close(fd_);
                    fd_ = -1;
                    size_ = 0;
                }
            }

            int release() noexcept {
                const int result = fd_;
                fd_ = -1;
                size_ = 0;
                return result;
            }

        private:
            int fd_ = -1;
            std::size_t size_ = 0;
        };

        template <access_mode Mode>
        static int get_file_open_flags() {
            if constexpr (std::is_same_v<Mode, read_only>)
                return O_RDONLY;
            else if constexpr (std::is_same_v<Mode, read_write>) {
                return O_RDWR;
            }
            else if constexpr (std::is_same_v<Mode, copy_on_write>) {
                return O_RDONLY;
            }
            return O_RDONLY;
        }

        /// POSIX backend implementation using mmap
        struct posix_backend {
            template <access_mode Mode>
            [[nodiscard]] static result<file_handle> open_file(
                const std::filesystem::path& path,
                const open_mode mode,
                const mode_t permissions = 0644) {
                int flags = get_file_open_flags<Mode>();

                switch (mode) {
                case open_mode::open_existing:
                    break;
                case open_mode::open_or_create:
                    flags |= O_CREAT;
                    break;
                case open_mode::create_truncate:
                    flags |= O_CREAT | O_TRUNC;
                    break;
                }

                int fd = ::open(path.c_str(), flags, permissions);
                if (fd < 0) {
                    if (errno == ENOENT) {
                        return std::unexpected(make_error_code(error_code::file_not_found));
                    }
                    else if (errno == EACCES) {
                        return std::unexpected(make_error_code(error_code::permission_denied));
                    }
                    return std::unexpected(make_error_code(error_code::backend_error));
                }

                struct stat st{};
                if (::fstat(fd, &st) < 0) {
                    ::close(fd);
                    return std::unexpected(make_error_code(error_code::backend_error));
                }

                return file_handle(fd, static_cast<std::size_t>(st.st_size));
            }

            [[nodiscard]] static result<std::size_t> get_file_size(const file_handle& handle) {
                if (!handle.is_valid()) {
                    return std::unexpected(make_error_code(error_code::invalid_argument));
                }

                struct stat st{};
                if (::fstat(handle.fd(), &st) < 0) {
                    return std::unexpected(make_error_code(error_code::backend_error));
                }

                return st.st_size;
            }

            [[nodiscard]] static result<void> resize_file(file_handle& handle, const std::size_t new_size) {
                if (!handle.is_valid()) {
                    return std::unexpected(make_error_code(error_code::invalid_argument));
                }

                if (::ftruncate(handle.fd(), static_cast<off_t>(new_size)) < 0) {
                    return std::unexpected(make_error_code(error_code::resize_failed));
                }

                handle.update_size(new_size);
                return {};
            }

            template <access_mode Mode>
            static int get_protection_flags() {
                if constexpr (std::is_same_v<Mode, read_only>) {
                    return PROT_READ;
                }
                else if constexpr (std::is_same_v<Mode, read_write>) {
                    return PROT_READ | PROT_WRITE;
                }
                else if constexpr (std::is_same_v<Mode, copy_on_write>) {
                    return PROT_READ | PROT_WRITE;
                }
                return PROT_READ;
            }

            template <access_mode Mode>
            static int get_map_flags(const map_kind kind) {
                int flags = 0;

                if constexpr (std::is_same_v<Mode, copy_on_write>) {
                    flags = MAP_PRIVATE;
                }
                else {
                    flags = (kind == map_kind::anonymous_private) ? MAP_PRIVATE : MAP_SHARED;
                }

                if (kind == map_kind::anonymous_private || kind == map_kind::anonymous_shared) {
                    flags |= MAP_ANON;
                }

                return flags;
            }

            template <access_mode Mode>
            [[nodiscard]] static result<map_result> map_file(
                const file_handle& handle,
                const std::size_t offset,
                std::size_t length) {
                if (!handle.is_valid()) {
                    return std::unexpected(make_error_code(error_code::invalid_argument));
                }

                if (!is_valid_range(offset, length == 0 ? (handle.size() - offset) : length, handle.size())) {
                    return std::unexpected(make_error_code(error_code::out_of_bounds));
                }

                if (length == 0) {
                    length = handle.size() - offset;
                }

                const std::size_t ps = platform::page_size();

                const std::size_t physical_offset = page::align_down(offset, ps);
                const std::size_t offset_adjustment = offset - physical_offset;
                const std::size_t physical_length = page::align_up(length + offset_adjustment, ps);

                const std::size_t max_physical_length = handle.size() - physical_offset;
                const std::size_t actual_physical_length = std::min(physical_length, max_physical_length);

                const int prot = get_protection_flags<Mode>();
                const int flags = get_map_flags<Mode>(map_kind::file_backed);

                void* addr = ::mmap(nullptr, actual_physical_length, prot, flags, handle.fd(),
                                    static_cast<off_t>(physical_offset));

                if (addr == MAP_FAILED) {
                    return std::unexpected(make_error_code(error_code::map_failed));
                }

                map_result result;
                result.base_address = addr;
                result.physical_offset = physical_offset;
                result.physical_size = actual_physical_length;
                result.logical_offset = offset;
                result.logical_size = length;

                return result;
            }

            template <access_mode Mode>
            [[nodiscard]] static result<map_result> map_anonymous(const std::size_t length, const map_kind kind) {
                if (length == 0) {
                    return std::unexpected(make_error_code(error_code::invalid_argument));
                }

                const std::size_t ps = platform::page_size();
                const std::size_t physical_length = page::align_up(length, ps);

                const int prot = get_protection_flags<Mode>();
                const int flags = get_map_flags<Mode>(kind);

                void* addr = ::mmap(nullptr, physical_length, prot, flags, -1, 0);

                if (addr == MAP_FAILED) {
                    return std::unexpected(make_error_code(error_code::map_failed));
                }

                map_result result;
                result.base_address = addr;
                result.physical_offset = 0;
                result.physical_size = physical_length;
                result.logical_offset = 0;
                result.logical_size = length;

                return result;
            }

            [[nodiscard]] static result<void> unmap(void* addr, const std::size_t length) {
                if (addr == nullptr || length == 0) {
                    return std::unexpected(make_error_code(error_code::invalid_argument));
                }

                if (::munmap(addr, length) < 0) {
                    return std::unexpected(make_error_code(error_code::unmap_failed));
                }

                return {};
            }

            [[nodiscard]] static result<void> flush(const void* addr, const std::size_t length, const flush_mode mode) {
                if (addr == nullptr || length == 0) {
                    return std::unexpected(make_error_code(error_code::invalid_argument));
                }

                if (const int flags = (mode == flush_mode::sync) ? MS_SYNC : MS_ASYNC; ::msync(
                    const_cast<void*>(addr), length, flags) < 0) {
                    return std::unexpected(make_error_code(error_code::flush_failed));
                }

                return {};
            }

            [[nodiscard]] static result<void> advise(void* addr, const std::size_t length, const advice_mode mode) {
                if (addr == nullptr || length == 0) {
                    return std::unexpected(make_error_code(error_code::invalid_argument));
                }

                int advice = MADV_NORMAL;
                switch (mode) {
                case advice_mode::normal: advice = MADV_NORMAL;
                    break;
                case advice_mode::sequential: advice = MADV_SEQUENTIAL;
                    break;
                case advice_mode::random: advice = MADV_RANDOM;
                    break;
                case advice_mode::will_need: advice = MADV_WILLNEED;
                    break;
                case advice_mode::dont_need: advice = MADV_DONTNEED;
                    break;
                }

                ::madvise(addr, length, advice);
                return {};
            }
        };
    } // namespace detail

    // ============================================================================
    // Owning Mapping Type
    // ============================================================================

    template <access_mode Mode>
    class mapping {
    public:
        using mode_type = Mode;
        static constexpr bool is_writable = Mode::is_writable;

        mapping() = default;

        ~mapping() {
            unmap_internal();
        }

        mapping(const mapping&) = delete;

        mapping& operator=(const mapping&) = delete;

        mapping(mapping&& other) noexcept
            : map_res_(other.map_res_)
              , file_handle_(std::move(other.file_handle_))
              , kind_(other.kind_)
#ifndef NDEBUG
              , generation_(other.generation_)
#endif
        {
            other.map_res_ = {};
        }

        mapping& operator=(mapping&& other) noexcept {
            if (this != &other) {
                unmap_internal();

                map_res_ = other.map_res_;
                file_handle_ = std::move(other.file_handle_);
                kind_ = other.kind_;
#ifndef NDEBUG
                generation_ = other.generation_;
#endif

                other.map_res_ = {};
            }
            return *this;
        }

        [[nodiscard]] static result<mapping> open_existing(
            const std::filesystem::path& path,
            const std::size_t offset = 0,
            const std::size_t length = 0) {
            open_options opts;
            opts.path = path;
            opts.mode = open_mode::open_existing;
            opts.initial_offset = offset;
            opts.initial_map_length = length;
            return open_with_options(opts);
        }

        [[nodiscard]] static result<mapping> open_or_create(
            const std::filesystem::path& path,
            const std::size_t size,
            const std::size_t offset = 0,
            const std::size_t length = 0) {
            open_options opts;
            opts.path = path;
            opts.mode = open_mode::open_or_create;
            opts.requested_size = size;
            opts.initial_offset = offset;
            opts.initial_map_length = length;
            return open_with_options(opts);
        }

        [[nodiscard]] static result<mapping> create_truncate(
            const std::filesystem::path& path,
            const std::size_t size,
            const std::size_t offset = 0,
            const std::size_t length = 0) {
            open_options opts;
            opts.path = path;
            opts.mode = open_mode::create_truncate;
            opts.requested_size = size;
            opts.initial_offset = offset;
            opts.initial_map_length = length;
            return open_with_options(opts);
        }

        [[nodiscard]] static result<mapping> open_with_options(const open_options& opts) {
            auto handle_result = detail::posix_backend::open_file<Mode>(
                opts.path, opts.mode, opts.create_permissions);

            if (!handle_result) {
                return std::unexpected(handle_result.error());
            }

            auto handle = std::move(*handle_result);

            if (opts.requested_size > 0 && handle.size() < opts.requested_size) {
                auto resize_result = detail::posix_backend::resize_file(handle, opts.requested_size);
                if (!resize_result) {
                    return std::unexpected(resize_result.error());
                }
            }

            std::size_t map_length = opts.initial_map_length;
            if (map_length == 0) {
                map_length = handle.size() - opts.initial_offset;
            }

            if (map_length == 0) {
                return std::unexpected(make_error_code(error_code::file_too_small));
            }

            auto map_result = detail::posix_backend::map_file<Mode>(
                handle, opts.initial_offset, map_length);

            if (!map_result) {
                return std::unexpected(map_result.error());
            }

            mapping m;
            m.map_res_ = *map_result;
            m.file_handle_ = std::move(handle);
            m.kind_ = map_kind::file_backed;

            return m;
        }

        [[nodiscard]] static result<mapping> create_anonymous_private(const std::size_t size) {
            return create_anonymous_internal(size, map_kind::anonymous_private);
        }

        [[nodiscard]] static result<mapping> create_anonymous_shared(const std::size_t size) {
            return create_anonymous_internal(size, map_kind::anonymous_shared);
        }

        [[nodiscard]] bool is_valid() const noexcept {
            return map_res_.base_address != nullptr;
        }

        explicit operator bool() const noexcept {
            return is_valid();
        }

        [[nodiscard]] std::size_t size() const noexcept {
            return map_res_.logical_size;
        }

        [[nodiscard]] std::size_t mapped_size() const noexcept {
            return map_res_.physical_size;
        }

        [[nodiscard]] map_kind kind() const noexcept {
            return kind_;
        }

        static constexpr bool writable() noexcept {
            return is_writable;
        }

        [[nodiscard]] bool is_file_backed() const noexcept {
            return kind_ == map_kind::file_backed;
        }

        static std::size_t page_size() noexcept {
            return platform::page_size();
        }

        std::byte* logical_base() noexcept requires (is_writable) {
            if (!is_valid()) return nullptr;
            const std::size_t adjustment = map_res_.logical_offset - map_res_.physical_offset;
            return static_cast<std::byte*>(map_res_.base_address) + adjustment;
        }

        const std::byte* logical_base() const noexcept {
            if (!is_valid()) return nullptr;
            const std::size_t adjustment = map_res_.logical_offset - map_res_.physical_offset;
            return static_cast<const std::byte*>(map_res_.base_address) + adjustment;
        }

        std::span<const std::byte> as_bytes() const noexcept {
            if (!is_valid()) return {};
            return {logical_base(), size()};
        }

        std::span<std::byte> as_bytes() noexcept requires (is_writable) {
            if (!is_valid()) return {};
            return {logical_base(), size()};
        }

        [[nodiscard]] region_view full_region() const noexcept;

        [[nodiscard]] result<region_view> subregion(std::size_t offset, std::size_t length) const;

        template <std::size_t PageSize>
        [[nodiscard]] std::size_t page_count() const noexcept {
            static_assert(PageSize > 0, "PageSize must be greater than zero");

            // Overflow-safe page count
            if (size() < PageSize) {
                return 0;
            }
            return size() / PageSize;
        }

        template <std::size_t PageSize>
        [[nodiscard]] page_view<PageSize> page_at(std::size_t index) const noexcept;

        template <std::size_t PageSize>
        [[nodiscard]] std::optional<std::size_t> page_index_to_offset(const std::size_t index) const noexcept {
            static_assert(PageSize > 0, "PageSize must be greater than zero");

            // Check for multiplication overflow
            if (index > SIZE_MAX / PageSize) {
                return std::nullopt;
            }

            const std::size_t offset = index * PageSize;

            if (offset > size() || PageSize > size() - offset) {
                return std::nullopt;
            }

            return offset;
        }

        template <std::size_t PageSize>
        [[nodiscard]] bool is_valid_page_index(std::size_t index) const noexcept {
            static_assert(PageSize > 0, "PageSize must be greater than zero");
            return index < page_count<PageSize>();
        }

        [[nodiscard]] result<void> flush(const flush_mode mode = flush_mode::async) const {
            if (!is_valid() || !is_file_backed()) {
                return std::unexpected(make_error_code(error_code::invalid_argument));
            }

            return detail::posix_backend::flush(
                map_res_.base_address,
                map_res_.physical_size,
                mode
            );
        }

        [[nodiscard]] result<void> flush_range(
            const std::size_t offset,
            const std::size_t length,
            const flush_mode mode = flush_mode::async) const {
            if (!is_valid() || !is_file_backed()) {
                return std::unexpected(make_error_code(error_code::invalid_argument));
            }

            if (length == 0) {
                return {};
            }

            if (!detail::is_valid_range(offset, length, size())) {
                return std::unexpected(make_error_code(error_code::out_of_bounds));
            }

            const std::size_t ps = platform::page_size();
            const std::byte* logical_start = logical_base() + offset;
            const auto logical_addr = reinterpret_cast<std::uintptr_t>(logical_start);

            const std::uintptr_t aligned_addr = logical_addr & ~(ps - 1);
            const auto* aligned_base = reinterpret_cast<const std::byte*>(aligned_addr);

            const std::size_t addr_adjustment = logical_addr - aligned_addr;
            const std::size_t aligned_length = page::align_up(length + addr_adjustment, ps);

            return detail::posix_backend::flush(
                aligned_base,
                aligned_length,
                mode
            );
        }

        [[nodiscard]] result<void> advise(const advice_mode mode) const {
            if (!is_valid()) {
                return std::unexpected(make_error_code(error_code::invalid_argument));
            }

            return detail::posix_backend::advise(
                map_res_.base_address,
                map_res_.physical_size,
                mode
            );
        }

        [[nodiscard]] result<void> advise_range(
            const std::size_t offset,
            const std::size_t length,
            const advice_mode mode) const {
            if (!is_valid()) {
                return std::unexpected(make_error_code(error_code::invalid_argument));
            }

            if (length == 0) {
                return {};
            }

            if (!detail::is_valid_range(offset, length, size())) {
                return std::unexpected(make_error_code(error_code::out_of_bounds));
            }

            const std::size_t ps = platform::page_size();
            const std::byte* logical_start = logical_base() + offset;
            const auto logical_addr = reinterpret_cast<std::uintptr_t>(logical_start);

            const std::uintptr_t aligned_addr = logical_addr & ~(ps - 1);
            const auto* aligned_base = reinterpret_cast<const std::byte*>(aligned_addr);

            const std::size_t addr_adjustment = logical_addr - aligned_addr;
            const std::size_t aligned_length = page::align_up(length + addr_adjustment, ps);

            return detail::posix_backend::advise(
                const_cast<std::byte*>(aligned_base),
                aligned_length,
                mode
            );
        }

        [[nodiscard]] result<void> resize_file(const std::size_t new_size) {
            if (!is_valid() || !is_file_backed()) {
                return std::unexpected(make_error_code(error_code::unsupported_operation));
            }

            auto result = detail::posix_backend::resize_file(file_handle_, new_size);

#ifndef NDEBUG
            if (result) {
                increment_generation();
            }
#endif

            return result;
        }

        [[nodiscard]] result<void> remap(const std::size_t new_offset, const std::size_t new_length) {
            if (!is_valid()) {
                return std::unexpected(make_error_code(error_code::invalid_argument));
            }

            unmap_internal();

            if (is_file_backed()) {
                auto map_result = detail::posix_backend::map_file<Mode>(
                    file_handle_,
                    new_offset,
                    new_length
                );

                if (!map_result) {
                    return std::unexpected(map_result.error());
                }

                map_res_ = *map_result;
            }
            else {
                auto map_result = detail::posix_backend::map_anonymous<Mode>(new_length, kind_);

                if (!map_result) {
                    return std::unexpected(map_result.error());
                }

                map_res_ = *map_result;
            }

#ifndef NDEBUG
            increment_generation();
#endif

            return {};
        }

        [[nodiscard]] result<void> remap_with_options(const remap_options& opts) {
            return remap(opts.new_offset, opts.new_length);
        }

        [[nodiscard]] result<void> resize_and_remap(const std::size_t new_size) {
            if (!is_valid() || !is_file_backed()) {
                return std::unexpected(make_error_code(error_code::unsupported_operation));
            }

            auto resize_result = detail::posix_backend::resize_file(file_handle_, new_size);
            if (!resize_result) {
                return std::unexpected(make_error_code(error_code::resize_failed));
            }

            return remap(0, new_size);
        }

    private:
        [[nodiscard]] static result<mapping> create_anonymous_internal(const std::size_t size, map_kind kind) {
            auto map_result = detail::posix_backend::map_anonymous<Mode>(size, kind);

            if (!map_result) {
                return std::unexpected(map_result.error());
            }

            mapping m;
            m.map_res_ = *map_result;
            m.kind_ = kind;

            return m;
        }

        void unmap_internal() noexcept {
            if (map_res_.base_address != nullptr) {
                // Intentionally ignore result in destructor - nothing we can do on failure
                (void)detail::posix_backend::unmap(map_res_.base_address, map_res_.physical_size);
                map_res_ = {};
            }
        }

        detail::map_result map_res_{};
        detail::file_handle file_handle_{};
        map_kind kind_ = map_kind::file_backed;

#ifndef NDEBUG

    public:
        [[nodiscard]] std::uint64_t current_generation() const noexcept { return generation_; }

    private:
        mutable uint64_t generation_ = 1;

        uint64_t generation() const noexcept { return generation_; }
        void increment_generation() const noexcept { ++generation_; }
#endif
    };

    // ============================================================================
    // Region View
    // ============================================================================

    class region_view {
    public:
        region_view() = default;

        template <std::size_t PageSize>
        friend class page_view;

        region_view(const std::byte* base, const std::size_t size, const bool writable = false) noexcept
            : base_(base)
              , size_(size)
              , writable_(writable)
#ifndef NDEBUG
              , generation_(nullptr)
#endif
        {}

        region_view(const std::byte* base, const std::size_t size) noexcept
            : base_(base)
              , size_(size)
              , writable_(true)
#ifndef NDEBUG
              , generation_(nullptr)
#endif
        {}

#ifndef NDEBUG
        region_view(const std::byte* base, const std::size_t size, const bool writable, const uint64_t* gen) noexcept
            : base_(base)
              , size_(size)
              , writable_(writable)
              , generation_(gen)
              , captured_generation_(gen ? *gen : 0) {}
#endif

        [[nodiscard]] bool is_valid() const noexcept {
#ifndef NDEBUG
            validate_generation();
#endif
            return base_ != nullptr;
        }

        explicit operator bool() const noexcept {
            return is_valid();
        }

        [[nodiscard]] std::size_t size() const noexcept {
            return size_;
        }

        [[nodiscard]] bool writable() const noexcept {
            return writable_;
        }

        [[nodiscard]] const std::byte* data() const noexcept {
            return base_;
        }

        std::byte* data() noexcept {
            return writable_ ? const_cast<std::byte*>(base_) : nullptr;
        }

        [[nodiscard]] std::span<const std::byte> as_bytes() const noexcept {
#ifndef NDEBUG
            validate_generation();
#endif
            if (!is_valid()) return {};
            return {base_, size_};
        }

        std::span<std::byte> as_bytes() noexcept {
#ifndef NDEBUG
            validate_generation();
#endif
            if (!is_valid() || !writable_) return {};
            return {const_cast<std::byte*>(base_), size_};
        }

        [[nodiscard]] result<region_view> subregion(const std::size_t offset, const std::size_t length) const {
#ifndef NDEBUG
            validate_generation();
#endif
            if (!is_valid()) {
                return std::unexpected(make_error_code(error_code::invalid_argument));
            }

            if (!detail::is_valid_range(offset, length, size_)) {
                return std::unexpected(make_error_code(error_code::out_of_bounds));
            }

#ifndef NDEBUG
            return region_view(base_ + offset, length, writable_, generation_);
#else
            return region_view(base_ + offset, length, writable_);
#endif
        }

        [[nodiscard]] result<region_view> subregion_from(const std::size_t offset) const {
#ifndef NDEBUG
            validate_generation();
#endif
            if (!is_valid()) {
                return std::unexpected(make_error_code(error_code::invalid_argument));
            }

            if (offset > size_) {
                return std::unexpected(make_error_code(error_code::out_of_bounds));
            }

#ifndef NDEBUG
            return region_view(base_ + offset, size_ - offset, writable_, generation_);
#else
            return region_view(base_ + offset, size_ - offset, writable_);
#endif
        }

        [[nodiscard]] region_view slice_unchecked(const std::size_t offset, const std::size_t length) const noexcept {
#ifndef NDEBUG
            return region_view(base_ + offset, length, writable_, generation_);
#else
            return region_view(base_ + offset, length, writable_);
#endif
        }

        template <mappable_type T>
        [[nodiscard]] result<const T*> read_at(const std::size_t offset) const {
            if (!is_valid()) {
                return std::unexpected(make_error_code(error_code::invalid_argument));
            }

            if (auto err = detail::validate_single_typed_access<T>(base_, size_, offset)) {
                return std::unexpected(err);
            }

            return reinterpret_cast<const T*>(base_ + offset);
        }

        template <mappable_type T>
        [[nodiscard]] result<T*> write_at(const std::size_t offset) {
            if (!is_valid() || !writable_) {
                return std::unexpected(make_error_code(error_code::invalid_argument));
            }

            if (auto err = detail::validate_single_typed_access<T>(base_, size_, offset)) {
                return std::unexpected(err);
            }

            return reinterpret_cast<T*>(const_cast<std::byte*>(base_ + offset));
        }

        template <mappable_type T>
        [[nodiscard]] result<std::span<const T>> read_array(const std::size_t offset, std::size_t count) const {
            if (!is_valid()) {
                return std::unexpected(make_error_code(error_code::invalid_argument));
            }

            if (auto err = detail::validate_array_typed_access<T>(base_, size_, offset, count)) {
                return std::unexpected(err);
            }

            if (count == 0) {
                return std::span<const T>{};
            }

            return std::span<const T>(reinterpret_cast<const T*>(base_ + offset), count);
        }

        template <mappable_type T>
        [[nodiscard]] result<std::span<T>> write_array(const std::size_t offset, std::size_t count) {
            if (!is_valid() || !writable_) {
                return std::unexpected(make_error_code(error_code::invalid_argument));
            }

            if (auto err = detail::validate_array_typed_access<T>(base_, size_, offset, count)) {
                return std::unexpected(err);
            }

            if (count == 0) {
                return std::span<T>{};
            }

            return std::span<T>(reinterpret_cast<T*>(const_cast<std::byte*>(base_ + offset)), count);
        }

        template <std::size_t PageSize>
        [[nodiscard]] constexpr std::size_t page_count() const noexcept {
            static_assert(PageSize > 0, "PageSize must be greater than zero");
            if (size_ < PageSize) {
                return 0;
            }
            return size_ / PageSize;
        }

        template <std::size_t PageSize>
        [[nodiscard]] page_view<PageSize> page_at(std::size_t index) const noexcept;

        template <std::size_t PageSize>
        [[nodiscard]] constexpr std::optional<std::size_t>
        page_index_to_offset(const std::size_t index) const noexcept {
            static_assert(PageSize > 0, "PageSize must be greater than zero");

            if (index > SIZE_MAX / PageSize) {
                return std::nullopt;
            }

            const std::size_t offset = index * PageSize;

            if (offset > size_ || PageSize > size_ - offset) {
                return std::nullopt;
            }

            return offset;
        }

        template <std::size_t PageSize>
        [[nodiscard]] constexpr bool is_valid_page_index(std::size_t index) const noexcept {
            static_assert(PageSize > 0, "PageSize must be greater than zero");
            return index < page_count<PageSize>();
        }

    private:
#ifndef NDEBUG
        void validate_generation() const noexcept {
            if (generation_ && *generation_ != captured_generation_) {
                std::fprintf(stderr,
                             "SETU: Stale region_view detected! View generation %" PRIu64 " != mapping generation %"
                             PRIu64 "\n"
                             "This view was created before a remap/resize operation and is now invalid.\n"
                             "Create a new view from the mapping after remap/resize.\n",
                             captured_generation_, *generation_);
                std::abort();
            }
        }
#endif

        const std::byte* base_ = nullptr;
        std::size_t size_ = 0;
        bool writable_ = false;

#ifndef NDEBUG
        const uint64_t* generation_ = nullptr;
        uint64_t captured_generation_ = 0;
#endif
    };

    namespace page {
        inline std::size_t page_count(const region_view& region, const std::size_t page_size) noexcept {
            return page_count(region.size(), page_size);
        }
    }

    template <access_mode Mode>
    region_view mapping<Mode>::full_region() const noexcept {
        if (!is_valid()) {
            return {};
        }

#ifndef NDEBUG
        if constexpr (is_writable) {
            return region_view(logical_base(), size(), true, &generation_);
        }
        else {
            return region_view(logical_base(), size(), false, &generation_);
        }
#else
        if constexpr (is_writable) {
            return region_view(const_cast<const std::byte*>(const_cast<std::byte*>(logical_base())), size(), true);
        }
        else {
            return region_view(logical_base(), size(), false);
        }
#endif
    }

    template <access_mode Mode>
    result<region_view> mapping<Mode>::subregion(const std::size_t offset, const std::size_t length) const {
        if (!is_valid()) {
            return std::unexpected(make_error_code(error_code::invalid_argument));
        }

        if (!detail::is_valid_range(offset, length, size())) {
            return std::unexpected(make_error_code(error_code::out_of_bounds));
        }

        const std::byte* base = logical_base() + offset;

#ifndef NDEBUG
        if constexpr (is_writable) {
            return region_view(base, length, true, &generation_);
        }
        else {
            return region_view(base, length, false, &generation_);
        }
#else
        if constexpr (is_writable) {
            return region_view(const_cast<const std::byte*>(const_cast<std::byte*>(base)), length, true);
        }
        else {
            return region_view(base, length, false);
        }
#endif
    }

    // ============================================================================
    // Page View
    // ============================================================================

    template <std::size_t PageSize>
    class page_view {
        // Power-of-two enforcement is not required for Setu's general-purpose design.
        // While power-of-two page sizes (4096, 8192, etc.) are common in OS page caches
        // and hardware, non-power-of-two sizes can be useful for logical data structures
        // (e.g., 5000-byte records, variable-length blocks). Setu remains flexible.
        static_assert(PageSize > 0, "PageSize must be greater than zero");

    public:
        static constexpr std::size_t page_size = PageSize;

        page_view() = default;

        page_view(const region_view& region, const std::size_t page_index)
            : base_(region.data())
              , writable_(region.writable()) {
            // Overflow-safe offset calculation
            if (page_index > SIZE_MAX / PageSize) {
                base_ = nullptr;
                valid_ = false;
                return;
            }

            const std::size_t offset = page_index * PageSize;
            if (offset > region.size() || PageSize > region.size() - offset) {
                base_ = nullptr;
                valid_ = false;
                return;
            }

            base_ += offset;
            valid_ = true;

#ifndef NDEBUG
            copy_generation_from(region);
#endif
        }

        [[nodiscard]] static result<page_view> at_offset(const region_view& region, const std::size_t byte_offset) {
            if (byte_offset % PageSize != 0) {
                return std::unexpected(make_error_code(error_code::misaligned_access));
            }

            if (byte_offset > region.size() || PageSize > region.size() - byte_offset) {
                return std::unexpected(make_error_code(error_code::out_of_bounds));
            }

            page_view view;
            view.base_ = region.data() + byte_offset;
            view.writable_ = region.writable();
            view.valid_ = true;

#ifndef NDEBUG
            view.copy_generation_from(region);
#endif

            return view;
        }

        [[nodiscard]] bool is_valid() const noexcept {
#ifndef NDEBUG
            validate_generation();
#endif
            return valid_ && base_ != nullptr;
        }

        explicit operator bool() const noexcept {
            return is_valid();
        }

        [[nodiscard]] bool writable() const noexcept {
            return writable_;
        }

        [[nodiscard]] constexpr std::size_t size() const noexcept {
            return PageSize;
        }

        [[nodiscard]] const std::byte* data() const noexcept {
            return base_;
        }

        std::byte* data() noexcept {
            return writable_ ? const_cast<std::byte*>(base_) : nullptr;
        }

        [[nodiscard]] std::span<const std::byte> page_bytes() const noexcept {
#ifndef NDEBUG
            validate_generation();
#endif
            if (!is_valid()) return {};
            return {base_, PageSize};
        }

        std::span<std::byte> page_bytes() noexcept {
#ifndef NDEBUG
            validate_generation();
#endif
            if (!is_valid() || !writable_) return {};
            return {const_cast<std::byte*>(base_), PageSize};
        }

        template <mappable_type T>
        [[nodiscard]] result<const T*> read_header() const {
#ifndef NDEBUG
            validate_generation();
#endif
            return read_at<T>(0);
        }

        template <mappable_type T>
        [[nodiscard]] result<T*> write_header() {
#ifndef NDEBUG
            validate_generation();
#endif
            return write_at<T>(0);
        }

        template <mappable_type T>
        [[nodiscard]] result<const T*> read_at(const std::size_t page_offset) const {
#ifndef NDEBUG
            validate_generation();
#endif
            if (!is_valid()) {
                return std::unexpected(make_error_code(error_code::invalid_argument));
            }

            if (auto err = detail::validate_single_typed_access<T>(base_, PageSize, page_offset)) {
                return std::unexpected(err);
            }

            return reinterpret_cast<const T*>(base_ + page_offset);
        }

        template <mappable_type T>
        [[nodiscard]] result<T*> write_at(const std::size_t page_offset) {
#ifndef NDEBUG
            validate_generation();
#endif
            if (!is_valid() || !writable_) {
                return std::unexpected(make_error_code(error_code::invalid_argument));
            }

            if (auto err = detail::validate_single_typed_access<T>(base_, PageSize, page_offset)) {
                return std::unexpected(err);
            }

            return reinterpret_cast<T*>(const_cast<std::byte*>(base_ + page_offset));
        }

        template <mappable_type T>
        [[nodiscard]] result<std::span<const T>> read_array(const std::size_t page_offset, std::size_t count) const {
#ifndef NDEBUG
            validate_generation();
#endif
            if (!is_valid()) {
                return std::unexpected(make_error_code(error_code::invalid_argument));
            }

            if (auto err = detail::validate_array_typed_access<T>(base_, PageSize, page_offset, count)) {
                return std::unexpected(err);
            }

            if (count == 0) {
                return std::span<const T>{};
            }

            return std::span<const T>(reinterpret_cast<const T*>(base_ + page_offset), count);
        }

        template <mappable_type T>
        [[nodiscard]] result<std::span<T>> write_array(const std::size_t page_offset, std::size_t count) {
#ifndef NDEBUG
            validate_generation();
#endif
            if (!is_valid() || !writable_) {
                return std::unexpected(make_error_code(error_code::invalid_argument));
            }

            if (auto err = detail::validate_array_typed_access<T>(base_, PageSize, page_offset, count)) {
                return std::unexpected(err);
            }

            if (count == 0) {
                return std::span<T>{};
            }

            return std::span<T>(reinterpret_cast<T*>(const_cast<std::byte*>(base_ + page_offset)), count);
        }

        [[nodiscard]] result<region_view> subrange(const std::size_t page_offset, const std::size_t length) const {
#ifndef NDEBUG
            validate_generation();
#endif
            if (!is_valid()) {
                return std::unexpected(make_error_code(error_code::invalid_argument));
            }

            if (!detail::is_valid_range(page_offset, length, PageSize)) {
                return std::unexpected(make_error_code(error_code::out_of_bounds));
            }

#ifndef NDEBUG
            return region_view(base_ + page_offset, length, writable_, generation_);
#else
            return region_view(base_ + page_offset, length, writable_);
#endif
        }

    private:
#ifndef NDEBUG
        void copy_generation_from(const region_view& region) noexcept {
            generation_ = region.generation_;
            captured_generation_ = region.captured_generation_;
        }

        void validate_generation() const noexcept {
            if (generation_ && *generation_ != captured_generation_) {
                std::fprintf(stderr,
                             "SETU: Stale page_view detected! View generation %" PRIu64 " != mapping generation %"
                             PRIu64 "\n"
                             "This page view was created before a remap/resize operation and is now invalid.\n"
                             "Create a new page view from the mapping after remap/resize.\n",
                             captured_generation_, *generation_);
                std::abort();
            }
        }

        friend class region_view;
#endif

        const std::byte* base_ = nullptr;
        bool writable_ = false;
        bool valid_ = false;

#ifndef NDEBUG
        const uint64_t* generation_ = nullptr;
        uint64_t captured_generation_ = 0;
#endif
    };

    template <access_mode Mode>
    template <std::size_t PageSize>
    inline page_view<PageSize> mapping<Mode>::page_at(std::size_t index) const noexcept {
        static_assert(PageSize > 0, "PageSize must be greater than zero");

        if (index >= page_count<PageSize>()) {
            return page_view<PageSize>{};
        }

        return page_view<PageSize>(full_region(), index);
    }

    template <std::size_t PageSize>
    page_view<PageSize> region_view::page_at(std::size_t index) const noexcept {
        static_assert(PageSize > 0, "PageSize must be greater than zero");

        if (index >= page_count<PageSize>()) {
            return page_view<PageSize>{};
        }

        return page_view<PageSize>(*this, index);
    }

    // Page iteration helpers

    template <std::size_t PageSize>
    class page_iterator {
    public:
        page_iterator(const region_view& region, const std::size_t index)
            : region_(region), index_(index) {}

        page_view<PageSize> operator*() const {
            return page_view<PageSize>(region_, index_);
        }

        page_iterator& operator++() {
            ++index_;
            return *this;
        }

        page_iterator operator++(int) {
            page_iterator tmp = *this;
            ++index_;
            return tmp;
        }

        bool operator==(const page_iterator& other) const {
            // Compare source identity and index
            return region_.data() == other.region_.data() &&
                region_.size() == other.region_.size() &&
                index_ == other.index_;
        }

        bool operator!=(const page_iterator& other) const {
            return !(*this == other);
        }

    private:
        region_view region_;
        std::size_t index_;
    };

    template <std::size_t PageSize>
    class page_range {
    public:
        page_range(const region_view& region)
            : region_(region)
              , count_(region.size() / PageSize) {}

        page_iterator<PageSize> begin() const {
            return page_iterator<PageSize>(region_, 0);
        }

        page_iterator<PageSize> end() const {
            return page_iterator<PageSize>(region_, count_);
        }

        [[nodiscard]] std::size_t size() const {
            return count_;
        }

    private:
        region_view region_;
        std::size_t count_;
    };

    template <std::size_t PageSize>
    page_range<PageSize> pages(const region_view& region) {
        return page_range<PageSize>(region);
    }

    // ============================================================================
    // Offset Pointer
    // ============================================================================

    template <typename T>
    class offset_ptr {
        static_assert(mappable_type<T>, "T must satisfy mappable_type constraints");

    public:
        using element_type = T;
        using offset_type = std::size_t;

        constexpr offset_ptr() noexcept = default;

        constexpr explicit offset_ptr(const offset_type byte_offset) noexcept
            : offset_(byte_offset) {}

        constexpr offset_ptr(std::nullptr_t) noexcept
            : offset_(null_sentinel) {}

        [[nodiscard]] constexpr bool is_null() const noexcept {
            return offset_ == null_sentinel;
        }

        [[nodiscard]] constexpr explicit operator bool() const noexcept {
            return !is_null();
        }

        [[nodiscard]] constexpr offset_type offset() const noexcept {
            return offset_;
        }

        [[nodiscard]] const T* get(const std::byte* base) const noexcept {
            if (is_null()) return nullptr;
            return reinterpret_cast<const T*>(base + offset_);
        }

        [[nodiscard]] T* get_mut(std::byte* base) const noexcept {
            if (is_null()) return nullptr;
            return reinterpret_cast<T*>(base + offset_);
        }

        [[nodiscard]] result<const T*> try_get(const region_view& region) const {
            if (is_null()) {
                return std::unexpected(make_error_code(error_code::invalid_argument));
            }

            if (auto err = detail::validate_single_typed_access<T>(
                region.data(), region.size(), offset_)) {
                return std::unexpected(err);
            }

            return reinterpret_cast<const T*>(region.data() + offset_);
        }

        [[nodiscard]] result<T*> try_get_mut(const region_view& region) const {
            if (!region.writable()) {
                return std::unexpected(make_error_code(error_code::invalid_argument));
            }

            if (is_null()) {
                return std::unexpected(make_error_code(error_code::invalid_argument));
            }

            if (auto err = detail::validate_single_typed_access<T>(
                region.data(), region.size(), offset_)) {
                return std::unexpected(err);
            }

            return reinterpret_cast<T*>(const_cast<std::byte*>(region.data() + offset_));
        }

        [[nodiscard]] constexpr bool operator==(const offset_ptr& other) const noexcept {
            return offset_ == other.offset_;
        }

        [[nodiscard]] constexpr bool operator!=(const offset_ptr& other) const noexcept {
            return offset_ != other.offset_;
        }

        [[nodiscard]] constexpr bool operator==(std::nullptr_t) const noexcept {
            return is_null();
        }

        [[nodiscard]] constexpr bool operator!=(std::nullptr_t) const noexcept {
            return !is_null();
        }

        [[nodiscard]] constexpr auto operator<=>(const offset_ptr& other) const noexcept {
            return offset_ <=> other.offset_;
        }

    private:
        static constexpr offset_type null_sentinel = SIZE_MAX;
        offset_type offset_ = null_sentinel;
    };

    // ============================================================================
    // Layout Helpers
    // ============================================================================

    namespace layout {
        template <typename T>
        T load(const std::byte* ptr, const std::endian byte_order) noexcept {
            static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");

            T value;
            std::memcpy(&value, ptr, sizeof(T));

            if (byte_order != std::endian::native) {
                if constexpr (std::is_integral_v<T>) {
                    static_assert(sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8,
                                  "Endianness conversion only supported for 1, 2, 4, or 8-byte integral types");
                    value = std::byteswap(value);
                }
            }

            return value;
        }

        template <typename T>
        void store(std::byte* ptr, T value, const std::endian byte_order) noexcept {
            static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");

            if (byte_order != std::endian::native) {
                if constexpr (std::is_integral_v<T>) {
                    static_assert(sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8,
                                  "Endianness conversion only supported for 1, 2, 4, or 8-byte integral types");
                    value = std::byteswap(value);
                }
            }

            std::memcpy(ptr, &value, sizeof(T));
        }

        template <std::size_t N>
        struct magic {
            std::array<std::byte, N> bytes;

            constexpr magic(const char (&str)[N + 1]) noexcept {
                for (std::size_t i = 0; i < N; ++i) {
                    bytes[i] = static_cast<std::byte>(str[i]);
                }
            }

            bool matches(const std::byte* ptr) const noexcept {
                return std::memcmp(ptr, bytes.data(), N) == 0;
            }

            [[nodiscard]] bool matches(const region_view& region) const noexcept {
                if (region.size() < N) return false;
                return matches(region.data());
            }
        };

        template <mappable_type HeaderT>
        struct header_validator {
            using header_type = HeaderT;

            [[nodiscard]] static result<const HeaderT*> validate(const region_view& region) {
                if (region.size() < sizeof(HeaderT)) {
                    return std::unexpected(make_error_code(error_code::file_too_small));
                }

                auto header_result = region.read_at<HeaderT>(0);
                if (!header_result) {
                    return std::unexpected(header_result.error());
                }

                return *header_result;
            }

            [[nodiscard]] static result<HeaderT*> validate_mut(region_view& region) {
                if (!region.writable()) {
                    return std::unexpected(make_error_code(error_code::invalid_argument));
                }

                if (region.size() < sizeof(HeaderT)) {
                    return std::unexpected(make_error_code(error_code::file_too_small));
                }

                auto header_result = region.write_at<HeaderT>(0);
                if (!header_result) {
                    return std::unexpected(header_result.error());
                }

                return *header_result;
            }
        };

        constexpr uint32_t fnv1a_32(const std::byte* data, const std::size_t length) noexcept {
            uint32_t hash = 2166136261u;
            for (std::size_t i = 0; i < length; ++i) {
                hash ^= std::to_integer<uint32_t>(data[i]);
                hash *= 16777619u;
            }
            return hash;
        }

        constexpr uint64_t fnv1a_64(const std::byte* data, const std::size_t length) noexcept {
            uint64_t hash = 14695981039346656037ull;
            for (std::size_t i = 0; i < length; ++i) {
                hash ^= std::to_integer<uint64_t>(data[i]);
                hash *= 1099511628211ull;
            }
            return hash;
        }

        inline uint32_t checksum32(const region_view& region) noexcept {
            auto bytes = region.as_bytes();
            return fnv1a_32(bytes.data(), bytes.size());
        }

        inline uint64_t checksum64(const region_view& region) noexcept {
            auto bytes = region.as_bytes();
            return fnv1a_64(bytes.data(), bytes.size());
        }
    } // namespace layout
} // namespace setu

namespace std {
    template <>
    struct is_error_code_enum<setu::error_code> : true_type {};
}

// ============================================================================
// USAGE EXAMPLES
// ============================================================================

/*

Example 1: Basic File Mapping
------------------------------

struct FileHeader {
    uint32_t magic;
    uint32_t version;
    uint64_t record_count;
};

void example_basic_mapping() {
    // Open existing file
    auto map = setu::mapping<setu::read_only>::open_existing("data.bin");
    if (!map) {
        // Handle error
        return;
    }

    // Get full region view
    auto region = map->full_region();

    // Read typed header
    auto header = region.read_at<FileHeader>(0);
    if (header) {
        std::cout << "Version: " << (*header)->version << "\n";
    }
}


Example 2: Page-Based Access
-----------------------------

struct PageHeader {
    uint32_t page_type;
    uint32_t record_count;
};

void example_page_access() {
    auto map = setu::mapping<setu::read_write>::open_or_create("db.dat", 4096 * 100);
    if (!map) return;

    auto region = map->full_region();

    // Access specific page
    auto page = map->page_at<4096>(0);
    if (page) {
        auto header = page.read_header<PageHeader>();
        if (header) {
            std::cout << "Page type: " << (*header)->page_type << "\n";
        }
    }

    // Iterate all pages
    for (auto pg : setu::pages<4096>(region)) {
        if (pg.is_valid()) {
            // Process page
        }
    }

    map->flush(setu::flush_mode::sync);
}


Example 3: Remap-Safe Data Structures
--------------------------------------

struct Node {
    uint64_t value;
    setu::offset_ptr<Node> left;
    setu::offset_ptr<Node> right;
};

void example_offset_ptr() {
    auto map = setu::mapping<setu::read_write>::open_or_create("tree.dat", 65536);
    if (!map) return;

    auto region = map->full_region();

    // Create root at offset 0
    auto root = region.write_at<Node>(0);
    if (root) {
        (*root)->value = 42;
        (*root)->left = setu::offset_ptr<Node>(sizeof(Node));
        (*root)->right = setu::offset_ptr<Node>(sizeof(Node) * 2);
    }

    // Navigate safely
    if (!(*root)->left.is_null()) {
        auto left = (*root)->left.try_get(region);
        if (left) {
            std::cout << "Left value: " << (*left)->value << "\n";
        }
    }

    // After remap, offset_ptr still works
    map->remap(0, 131072);
    region = map->full_region();  // Must recreate views

    root = region.write_at<Node>(0);
    // offset_ptr resolution still works with new base
}


Example 4: Sliding Window Processing
-------------------------------------

void example_sliding_window() {
    auto map = setu::mapping<setu::read_only>::open_existing("huge.dat");
    if (!map) return;

    const size_t window = 64 * 1024 * 1024;  // 64MB
    const size_t file_size = map->size();

    for (size_t offset = 0; offset < file_size; offset += window) {
        size_t length = std::min(window, file_size - offset);

        if (auto result = map->remap(offset, length); !result) {
            continue;
        }

        map->advise(setu::advice_mode::sequential);

        auto region = map->full_region();
        // Process window...

        map->advise(setu::advice_mode::dont_need);
    }
}

*/

