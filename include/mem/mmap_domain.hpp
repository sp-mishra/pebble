#pragma once
// ============================================================================
// mmap_domain.hpp — mmap-backed and NUMA-aware domains for smriti
// ============================================================================
// MappedFileDomain: MAP_ANON or MAP_SHARED file-backed domain
// MappedRegion:     non-owning view into a MappedFileDomain sub-range
// NumaDomain:       NUMA/memory-tier aware allocation (macOS + Linux)
//                   Falls back to SystemRAMDomain on unsupported platforms.
// ============================================================================

#include "smriti.hpp"
#include <cerrno>
#include <cstring>
#include <string_view>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

namespace smriti::domains {
    struct MmapContext {
        int fd{-1};
        void* base{MAP_FAILED};
        std::size_t mapped_size{};
        std::size_t cursor{}; // bump cursor within mapped region
    };

    // MappedFileDomain — mmap-backed; satisfies DomainWithContext
    struct MappedFileDomain {
        using context_type = MmapContext;
        static constexpr std::size_t alignment = 4096; // OS page size

        MmapContext ctx_;

        MappedFileDomain() = default;

        MappedFileDomain(const MappedFileDomain&) = delete;

        MappedFileDomain& operator=(const MappedFileDomain&) = delete;

        MappedFileDomain(MappedFileDomain&& o) noexcept : ctx_{o.ctx_} {
            o.ctx_ = {};
            o.ctx_.fd = -1;
            o.ctx_.base = MAP_FAILED;
        }

        MappedFileDomain& operator=(MappedFileDomain&& o) noexcept {
            if (this != &o) {
                cleanup();
                ctx_ = o.ctx_;
                o.ctx_ = {};
                o.ctx_.fd = -1;
                o.ctx_.base = MAP_FAILED;
            }
            return *this;
        }

        ~MappedFileDomain() noexcept { cleanup(); }

        // Anonymous mapping (no backing file)
        [[nodiscard]] static MappedFileDomain anonymous(const std::size_t size) noexcept {
            MappedFileDomain d;
            void* p = ::mmap(nullptr, size,
                             PROT_READ | PROT_WRITE,
                             MAP_ANON | MAP_PRIVATE, -1, 0);
            if (p == MAP_FAILED) return d;
            d.ctx_.base = p;
            d.ctx_.mapped_size = size;
            return d;
        }

        // File-backed mapping
        [[nodiscard]] static MappedFileDomain from_file(
            const std::string_view path, const std::size_t size, const bool writeable = true) noexcept {
            MappedFileDomain d;
            const int flags = writeable ? O_RDWR | O_CREAT : O_RDONLY;
            const int fd = ::open(std::string{path}.c_str(), flags, 0644);
            if (fd < 0) return d;
            if (writeable && ::ftruncate(fd, static_cast<off_t>(size)) != 0) {
                ::close(fd);
                return d;
            }
            const int prot = PROT_READ | (writeable ? PROT_WRITE : 0);
            void* p = ::mmap(nullptr, size, prot, MAP_SHARED, fd, 0);
            if (p == MAP_FAILED) {
                ::close(fd);
                return d;
            }
            d.ctx_.fd = fd;
            d.ctx_.base = p;
            d.ctx_.mapped_size = size;
            return d;
        }

        [[nodiscard]] bool valid() const noexcept {
            return ctx_.base != MAP_FAILED && ctx_.base != nullptr;
        }

        // Sub-allocates within the mapped region (bump style)
        [[nodiscard]] void* acquire(const std::size_t n, const std::size_t a) noexcept {
            if (!valid()) return nullptr;
            const std::size_t aligned = detail::align_up(ctx_.cursor, a);
            if (aligned + n > ctx_.mapped_size) return nullptr;
            ctx_.cursor = aligned + n;
            return static_cast<std::byte*>(ctx_.base) + aligned;
        }

        // No per-pointer unmap — the entire region is released in the destructor.
        // msync on the affected range for durability.
        void release(void* p, const std::size_t n) const noexcept {
            if (!valid() || !p) return;
            ::msync(p, n, MS_ASYNC);
        }

        [[nodiscard]] MmapContext& context() noexcept { return ctx_; }

    private:
        void cleanup() noexcept {
            if (valid()) {
                ::msync(ctx_.base, ctx_.mapped_size, MS_SYNC);
                ::munmap(ctx_.base, ctx_.mapped_size);
                ctx_.base = MAP_FAILED;
            }
            if (ctx_.fd >= 0) {
                ::close(ctx_.fd);
                ctx_.fd = -1;
            }
        }
    };

    // MappedRegion — non-owning view into a sub-range of a MappedFileDomain.
    // The domain must outlive all regions derived from it.
    struct MappedRegion {
        std::span<std::byte> bytes{};
        bool msync_on_flush{false};

        void flush() const noexcept {
            if (bytes.empty()) return;
            if (msync_on_flush)
                ::msync(bytes.data(), bytes.size(), MS_SYNC);
            else
                ::msync(bytes.data(), bytes.size(), MS_ASYNC);
        }

        [[nodiscard]] std::byte* data() const noexcept { return bytes.data(); }
        [[nodiscard]] std::size_t size() const noexcept { return bytes.size(); }
    };

    // NumaDomain — memory-tier aware allocation.
    // macOS arm64: falls back gracefully to mmap (no libnuma).
    // Linux:       mbind() can be added; for now same mmap path.
    // When NUMA APIs are not available, degrades to SystemRAMDomain semantics.

#if defined(__APPLE__) || defined(__linux__)
    struct NumaContext {
        int node_id{0};
        bool prefer_local{true};
    };

    struct NumaDomain {
        using context_type = NumaContext;
        static constexpr std::size_t alignment = alignof(std::max_align_t);
        NumaContext ctx_;

        [[nodiscard]] static NumaDomain local() noexcept {
            NumaDomain d;
            d.ctx_ = {0, true};
            return d;
        }

        [[nodiscard]] static NumaDomain node(const int id) noexcept {
            NumaDomain d;
            d.ctx_ = {id, false};
            return d;
        }

        [[nodiscard]] void* acquire(const std::size_t n, const std::size_t a) noexcept {
            // macOS: use mmap for page-aligned allocation (closest to NUMA semantics)
            const std::size_t sz = detail::align_up(n, 4096);
            void* p = ::mmap(nullptr, sz,
                             PROT_READ | PROT_WRITE,
                             MAP_ANON | MAP_PRIVATE, -1, 0);
            if (p == MAP_FAILED) {
                // Fallback: plain operator new
                return ::operator new(n, std::align_val_t{a}, std::nothrow);
            }
            return p;
        }

        void release(void* p, const std::size_t n) noexcept {
            if (!p) return;
            // Attempt munmap; if it fails (not an mmap'd address) fall back to delete
            const std::size_t sz = detail::align_up(n, 4096);
            if (::munmap(p, sz) != 0)
                ::operator delete(p, std::align_val_t{alignment});
        }

        [[nodiscard]] NumaContext& context() noexcept { return ctx_; }
    };

#else
    // Platform fallback: NumaDomain = SystemRAMDomain
    using NumaDomain = SystemRAMDomain;
#endif
} // namespace smriti::domains
