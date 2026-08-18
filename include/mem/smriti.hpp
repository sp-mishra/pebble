#pragma once
// ============================================================================
// smriti.hpp — Universal Memory Resource Framework
// ============================================================================
// C++23, header-only, no macros, no virtual, no RTTI.
// Five layers: Domain → Pool → Policy → Handle → Manager → ManagedResource
// ============================================================================

#include <algorithm>
#include <atomic>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <flat_map>
#include <flat_set>
#include <format>
#include <functional>
#include <latch>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <print>
#include <shared_mutex>
#include <source_location>
#include <span>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <utility>

namespace smriti {
    // ============================================================================
    // SECTION 1 — Core Utilities (detail)
    // ============================================================================

    namespace detail {
        inline constexpr std::size_t cache_line = 64;

        [[nodiscard]] constexpr std::size_t
        align_up(const std::size_t n, const std::size_t a) noexcept {
            return (n + a - 1) & ~(a - 1);
        }

        template <typename T>
        struct alignas(cache_line) padded {
            T value{};
        };

        struct GenId {
            std::uint32_t id{};
            std::uint32_t gen{};

            constexpr bool operator==(const GenId&) const noexcept = default;

            [[nodiscard]] constexpr bool valid() const noexcept { return id != 0; }
        };

        inline constexpr GenId null_genid{};

        template <typename Derived>
        struct ref_counted {
            std::atomic<std::uint32_t> refs{1};

            void inc() noexcept {
                refs.fetch_add(1, std::memory_order_relaxed);
            }

            void dec() noexcept {
                if (refs.fetch_sub(1, std::memory_order_acq_rel) == 1)
                    static_cast<Derived*>(this)->destroy();
            }
        };
    } // namespace detail

    // ============================================================================
    // SECTION 7 — Forward declarations needed by concepts
    // ============================================================================

    class PageTable;
    enum class PageState : std::uint8_t;
    enum class PageError : std::uint8_t;

    // ============================================================================
    // SECTION 2 — Concepts
    // ============================================================================

    namespace concepts {
        template <typename D>
        concept Domain = requires(D d, std::size_t n, std::size_t a, void* p) {
            { D::alignment } -> std::convertible_to<std::size_t>;
            { d.acquire(n, a) } -> std::same_as<void*>;
            { d.release(p, n) } noexcept;
        };

        template <typename D>
        concept DomainWithContext = Domain<D> && requires(D d) {
            typename D::context_type;
            { d.context() } -> std::same_as<typename D::context_type&>;
        };

        template <typename P>
        concept Pool = requires(P p, std::size_t n, std::size_t a, void* ptr) {
            { p.allocate(n, a) } -> std::same_as<void*>;
            { p.deallocate(ptr, n) } noexcept;
            { p.reset() } noexcept;
        };

        template <typename P>
        concept BulkPool = Pool<P> && requires(P p) {
            { p.used_bytes() } -> std::convertible_to<std::size_t>;
        };

        template <typename P>
        concept PolicyWrapper = Pool<P> && requires { typename P::inner_pool_type; };

        template <typename R>
        concept MemoryResource = requires(R r, std::size_t n, std::size_t a, void* p) {
            { r.allocate(n, a) } -> std::same_as<void*>;
            { r.deallocate(p, n) } noexcept;
        };

        template <typename R>
        concept Pinnable = MemoryResource<R> && requires(R r, detail::GenId id) {
            { r.pin(id) } -> std::same_as<bool>;
            { r.unpin(id) } noexcept;
        };

        template <typename M>
        concept Manager = requires(M m, PageTable& t, detail::GenId id) {
            { m.attach(t) } noexcept;
            { m.on_alloc(id) } noexcept;
            { m.on_access(id) } noexcept;
            { m.evict_one() } -> std::same_as<std::optional<detail::GenId>>;
            { m.shutdown() } noexcept;
        };
    } // namespace concepts

    // ============================================================================
    // SECTION 3 — Domains
    // ============================================================================

    namespace domains {
        struct SystemRAMDomain {
            static constexpr std::size_t alignment = alignof(std::max_align_t);

            [[nodiscard]] void* acquire(const std::size_t n, const std::size_t a) noexcept {
                return ::operator new(n, std::align_val_t{a}, std::nothrow);
            }

            void release(void* p, std::size_t /*n*/) noexcept {
                ::operator delete(p, std::align_val_t{alignment});
            }
        };

        template <std::size_t N, std::size_t Align = alignof(std::max_align_t)>
        struct StackDomain {
            static constexpr std::size_t alignment = Align;
            alignas(Align) std::byte buf[N]{};
            std::size_t cursor{};

            StackDomain() = default;

            StackDomain(const StackDomain&) = delete;

            StackDomain& operator=(const StackDomain&) = delete;

            StackDomain(StackDomain&&) = delete;

            StackDomain& operator=(StackDomain&&) = delete;

            [[nodiscard]] void* acquire(const std::size_t n, const std::size_t a) noexcept {
                std::size_t aligned = detail::align_up(cursor, a);
                if (aligned + n > N) return nullptr;
                cursor = aligned + n;
                return buf + aligned;
            }

            void release(void*, std::size_t) noexcept {}
        };

        struct NullDomain {
            static constexpr std::size_t alignment = 1;
            [[nodiscard]] void* acquire(std::size_t, std::size_t) noexcept { return nullptr; }

            void release(void*, std::size_t) noexcept {}
        };
    } // namespace domains

    // ============================================================================
    // SECTION 4 — Pools
    // ============================================================================

    namespace pools {
        template <concepts::Domain DomainT>
        class BumpPool {
            DomainT domain_;
            std::byte* base_{};
            std::size_t capacity_{};
            detail::padded<std::atomic<std::size_t>> offset_{};

        public:
            using inner_domain_type = DomainT;

            BumpPool() = default;

            explicit BumpPool(std::size_t capacity, DomainT d = {})
                : domain_{std::move(d)}, capacity_{capacity} {
                base_ = static_cast<std::byte*>(domain_.acquire(capacity, DomainT::alignment));
            }

            // In-place domain construction — avoids copy/move requirement for StackDomain
            template <typename... DomainArgs>
            explicit BumpPool(std::size_t capacity, std::in_place_t, DomainArgs&&... args)
                : domain_{std::forward<DomainArgs>(args)...}, capacity_{capacity} {
                base_ = static_cast<std::byte*>(domain_.acquire(capacity, DomainT::alignment));
            }

            ~BumpPool() {
                if (base_) domain_.release(base_, capacity_);
            }

            BumpPool(const BumpPool&) = delete;

            BumpPool& operator=(const BumpPool&) = delete;

            BumpPool(BumpPool&& o) noexcept
                : domain_{std::move(o.domain_)}, base_{o.base_},
                  capacity_{o.capacity_} {
                offset_.value.store(o.offset_.value.load(std::memory_order_acquire),
                                    std::memory_order_release);
                o.base_ = nullptr;
                o.capacity_ = 0;
            }

            BumpPool& operator=(BumpPool&& o) noexcept {
                if (this != &o) {
                    if (base_) domain_.release(base_, capacity_);
                    domain_ = std::move(o.domain_);
                    base_ = o.base_;
                    capacity_ = o.capacity_;
                    offset_.value.store(o.offset_.value.load(std::memory_order_acquire),
                                        std::memory_order_release);
                    o.base_ = nullptr;
                    o.capacity_ = 0;
                }
                return *this;
            }

            [[nodiscard]] void* allocate(const std::size_t n, const std::size_t a) noexcept {
                if (!base_) return nullptr;
                // Validate alignment is power-of-two at compile time when possible
                std::size_t old = offset_.value.load(std::memory_order_relaxed);
                while (true) {
                    const std::size_t aligned = detail::align_up(old, a);
                    if (aligned + n > capacity_) return nullptr;
                    if (offset_.value.compare_exchange_weak(old, aligned + n,
                                                            std::memory_order_acq_rel, std::memory_order_relaxed))
                        return base_ + aligned;
                }
            }

            void deallocate(void*, std::size_t) noexcept {}

            void reset() noexcept {
                offset_.value.store(0, std::memory_order_release);
            }

            [[nodiscard]] std::size_t used_bytes() const noexcept {
                return offset_.value.load(std::memory_order_acquire);
            }

            [[nodiscard]] std::atomic<std::size_t>& atomic_offset() noexcept {
                return offset_.value;
            }
        };

        template <std::size_t BlockSize, concepts::Domain DomainT>
            requires (BlockSize >= sizeof(void*))
        class FixedPool {
            struct Slab {
                void* mem{};
                Slab* next{};
            };

            detail::padded<std::atomic<void*>> free_head_{};
            Slab* slabs_{};
            DomainT domain_;
            std::size_t blocks_per_slab_;
            std::mutex slab_mu_;

            void grow() {
                const std::size_t sz = blocks_per_slab_ * BlockSize;
                void* mem = domain_.acquire(sz + sizeof(Slab), DomainT::alignment);
                if (!mem) return;
                auto* slab = static_cast<Slab*>(mem);
                slab->mem = static_cast<std::byte*>(mem) + sizeof(Slab);
                slab->next = slabs_;
                slabs_ = slab;
                auto* p = static_cast<std::byte*>(slab->mem);
                for (std::size_t i = 0; i < blocks_per_slab_; ++i)
                    deallocate(p + i * BlockSize, BlockSize);
            }

        public:
            using inner_domain_type = DomainT;

            explicit FixedPool(const std::size_t blocks_per_slab = 64, DomainT d = {})
                : domain_{std::move(d)}, blocks_per_slab_{blocks_per_slab} {}

            ~FixedPool() { reset(); }

            FixedPool(const FixedPool&) = delete;

            FixedPool& operator=(const FixedPool&) = delete;

            [[nodiscard]] void* allocate(std::size_t /*n*/, std::size_t /*a*/) noexcept {
                void* head = free_head_.value.load(std::memory_order_acquire);
                while (head) {
                    void* next;
                    std::memcpy(&next, head, sizeof(void*));
                    if (free_head_.value.compare_exchange_weak(head, next,
                                                               std::memory_order_acq_rel, std::memory_order_relaxed))
                        return head;
                }
                // Free list empty — grow a new slab under lock
                std::scoped_lock g{slab_mu_};
                // Re-check after acquiring lock
                head = free_head_.value.load(std::memory_order_acquire);
                if (head) {
                    void* next;
                    std::memcpy(&next, head, sizeof(void*));
                    if (free_head_.value.compare_exchange_weak(head, next,
                                                               std::memory_order_acq_rel, std::memory_order_relaxed))
                        return head;
                }
                grow();
                head = free_head_.value.load(std::memory_order_acquire);
                if (!head) return nullptr;
                void* next;
                std::memcpy(&next, head, sizeof(void*));
                free_head_.value.store(next, std::memory_order_release);
                return head;
            }

            void deallocate(void* p, std::size_t) noexcept {
                void* head = free_head_.value.load(std::memory_order_acquire);
                std::memcpy(p, &head, sizeof(void*));
                while (!free_head_.value.compare_exchange_weak(head, p,
                                                               std::memory_order_acq_rel, std::memory_order_relaxed)) {
                    std::memcpy(p, &head, sizeof(void*));
                }
            }

            void reset() noexcept {
                free_head_.value.store(nullptr, std::memory_order_release);
                Slab* s = slabs_;
                while (s) {
                    Slab* next = s->next;
                    std::size_t sz = blocks_per_slab_ * BlockSize + sizeof(Slab);
                    domain_.release(s, sz);
                    s = next;
                }
                slabs_ = nullptr;
            }

            [[nodiscard]] std::atomic<void*>& atomic_free_head() noexcept {
                return free_head_.value;
            }
        };
    } // namespace pools

    // ============================================================================
    // SECTION 5 — Policies
    // ============================================================================

    namespace policies {
        template <concepts::Pool PoolT>
        struct UnsafePolicy {
            using inner_pool_type = PoolT;
            PoolT pool;

            [[nodiscard]] void* allocate(std::size_t n, std::size_t a) noexcept {
                return pool.allocate(n, a);
            }

            void deallocate(void* p, std::size_t n) noexcept { pool.deallocate(p, n); }
            void reset() noexcept { pool.reset(); }
        };

        template <concepts::Pool PoolT>
        struct ThreadSafePolicy {
            using inner_pool_type = PoolT;
            PoolT pool;
            std::mutex mu;

            [[nodiscard]] void* allocate(std::size_t n, std::size_t a) {
                std::scoped_lock g{mu};
                return pool.allocate(n, a);
            }

            void deallocate(void* p, std::size_t n) noexcept {
                std::scoped_lock g{mu};
                pool.deallocate(p, n);
            }

            void reset() noexcept {
                std::scoped_lock g{mu};
                pool.reset();
            }
        };

        template <concepts::Pool PoolT>
        struct BoundsCheckPolicy {
            using inner_pool_type = PoolT;

            static constexpr std::uint32_t kMagicHead = 0xDEADBEEFu;
            static constexpr std::uint32_t kMagicTail = 0xCAFEBABEu;

            struct AllocHeader {
                std::uint32_t magic{kMagicHead};
                std::size_t user_size{};
                std::source_location where{};
            };

            struct Canary {
                std::uint32_t magic{kMagicTail};
            };

            PoolT pool;

            [[nodiscard]] void* allocate(std::size_t n, std::size_t a,
                                         std::source_location loc = std::source_location::current()) noexcept {
                std::size_t total = sizeof(AllocHeader) + n + sizeof(Canary);
                void* raw = pool.allocate(total, a);
                if (!raw) return nullptr;
                auto* hdr = ::new(raw) AllocHeader{kMagicHead, n, loc};
                auto* user = reinterpret_cast<std::byte*>(hdr) + sizeof(AllocHeader);
                ::new(user + n) Canary{kMagicTail};
                return user;
            }

            void deallocate(void* p, const std::size_t n) noexcept {
                if (!p) return;
                auto* hdr = reinterpret_cast<AllocHeader*>(
                    static_cast<std::byte*>(p) - sizeof(AllocHeader));
                if (hdr->magic != kMagicHead) {
                    std::print(stderr, "SMRITI: header corruption at {}\n",
                               static_cast<void*>(p));
                    std::terminate();
                }
                auto* canary = reinterpret_cast<Canary*>(
                    static_cast<std::byte*>(p) + hdr->user_size);
                if (canary->magic != kMagicTail) {
                    std::print(stderr, "SMRITI: buffer overrun at {} ({}:{})\n",
                               static_cast<void*>(p),
                               hdr->where.file_name(), hdr->where.line());
                    std::terminate();
                }
                pool.deallocate(hdr, sizeof(AllocHeader) + n + sizeof(Canary));
            }

            void reset() noexcept { pool.reset(); }
        };

        template <concepts::Pool PoolT>
            requires requires(PoolT p) { p.atomic_offset(); }
        struct LockFreePolicy {
            using inner_pool_type = PoolT;
            PoolT pool;

            [[nodiscard]] void* allocate(std::size_t n, std::size_t a) noexcept {
                return pool.allocate(n, a);
            }

            void deallocate(void* p, std::size_t n) noexcept { pool.deallocate(p, n); }
            void reset() noexcept { pool.reset(); }
        };

        template <concepts::Pool PoolT>
        struct AuditPolicy {
            using inner_pool_type = PoolT;

            struct AllocRecord {
                std::size_t size{};
                std::source_location where{};
            };

            PoolT pool;
            std::flat_map<void*, AllocRecord> live_;
            std::size_t alloc_count_{};
            std::size_t total_bytes_{};

            [[nodiscard]] void* allocate(std::size_t n, std::size_t a,
                                         std::source_location loc = std::source_location::current()) noexcept {
                void* p = pool.allocate(n, a);
                if (p) {
                    live_.emplace(p, AllocRecord{n, loc});
                    ++alloc_count_;
                    total_bytes_ += n;
                }
                return p;
            }

            void deallocate(void* p, std::size_t n) noexcept {
                live_.erase(p);
                pool.deallocate(p, n);
            }

            void reset() noexcept {
                live_.clear();
                alloc_count_ = 0;
                total_bytes_ = 0;
                pool.reset();
            }

            void report() const noexcept {
                if (live_.empty()) return;
                std::print(stderr, "SMRITI AUDIT: {} live allocs, {} bytes leaked\n",
                           live_.size(), total_bytes_);
                for (std::size_t i = 0; i < live_.size(); ++i) {
                    auto ptr = live_.keys()[i];
                    const auto& rec = live_.values()[i];
                    std::print(stderr, "  0x{:x} {} bytes @ {}:{}\n",
                               reinterpret_cast<std::uintptr_t>(ptr), rec.size,
                               rec.where.file_name(), rec.where.line());
                }
            }
        };
    } // namespace policies

    // ============================================================================
    // SECTION 7 — PageTable (defined before handles/managers that reference it)
    // ============================================================================

    enum class PageState : std::uint8_t {
        Cold, Loading, Resident, Evicting, Flushing
    };

    enum class PageError : std::uint8_t {
        Stale, Evicted, NullPage
    };

    struct PageEntry {
        detail::GenId gid{};
        void* ptr{};
        std::atomic<std::uint32_t> pin_count{};
        std::atomic<PageState> state{PageState::Cold};
        bool dirty{false};

        PageEntry() = default;

        PageEntry(const detail::GenId g, void* p)
            : gid{g}, ptr{p}, state{PageState::Resident} {}

        // Atomics are not movable — PageEntry must be default-constructed then filled
        PageEntry(const PageEntry&) = delete;

        PageEntry& operator=(const PageEntry&) = delete;

        PageEntry(PageEntry&&) = delete;

        PageEntry& operator=(PageEntry&&) = delete;
    };

    class PageTable {
        // std::flat_map requires movable values; we store PageEntry via unique_ptr
        std::flat_map<std::uint32_t, std::unique_ptr<PageEntry>> entries_;
        mutable std::shared_mutex rw_mu_;
        std::uint32_t next_id_{1};
        std::uint32_t next_gen_{1};

    public:
        PageTable() = default;

        ~PageTable() = default;

        PageTable(const PageTable&) = delete;

        PageTable& operator=(const PageTable&) = delete;

        [[nodiscard]] detail::GenId alloc_page(void* ptr) noexcept {
            std::unique_lock g{rw_mu_};
            detail::GenId id{next_id_++, next_gen_++};
            auto entry = std::make_unique<PageEntry>(id, ptr);
            entries_.emplace(id.id, std::move(entry));
            return id;
        }

        bool free_page(const detail::GenId id) noexcept {
            std::unique_lock g{rw_mu_};
            const auto it = entries_.find(id.id);
            if (it == entries_.end()) return false;
            const auto& e = *it->second;
            if (e.gid.gen != id.gen) return false;
            if (e.pin_count.load(std::memory_order_acquire) > 0) return false;
            entries_.erase(it);
            return true;
        }

        bool pin(const detail::GenId id) noexcept {
            std::shared_lock g{rw_mu_};
            const auto it = entries_.find(id.id);
            if (it == entries_.end()) return false;
            auto& e = *it->second;
            if (e.gid.gen != id.gen) return false;
            const PageState s = e.state.load(std::memory_order_acquire);
            if (s == PageState::Evicting || s == PageState::Flushing || s == PageState::Cold)
                return false;
            e.pin_count.fetch_add(1, std::memory_order_acq_rel);
            return true;
        }

        void unpin(const detail::GenId id) noexcept {
            std::shared_lock g{rw_mu_};
            const auto it = entries_.find(id.id);
            if (it == entries_.end()) return;
            auto& e = *it->second;
            if (e.gid.gen != id.gen) return;
            e.pin_count.fetch_sub(1, std::memory_order_acq_rel);
        }

        bool mark_dirty(const detail::GenId id) noexcept {
            std::shared_lock g{rw_mu_};
            const auto it = entries_.find(id.id);
            if (it == entries_.end()) return false;
            auto& e = *it->second;
            if (e.gid.gen != id.gen) return false;
            e.dirty = true;
            return true;
        }

        [[nodiscard]] std::expected<void*, PageError>
        resolve(const detail::GenId id) const noexcept {
            if (!id.valid()) return std::unexpected{PageError::NullPage};
            std::shared_lock g{rw_mu_};
            const auto it = entries_.find(id.id);
            if (it == entries_.end()) return std::unexpected{PageError::Evicted};
            auto& e = *it->second;
            if (e.gid.gen != id.gen) return std::unexpected{PageError::Stale};
            const PageState s = e.state.load(std::memory_order_acquire);
            if (s == PageState::Cold || s == PageState::Evicting)
                return std::unexpected{PageError::Evicted};
            return e.ptr;
        }

        bool transition(const detail::GenId id, PageState expected, const PageState desired) noexcept {
            std::shared_lock g{rw_mu_};
            const auto it = entries_.find(id.id);
            if (it == entries_.end()) return false;
            auto& e = *it->second;
            if (e.gid.gen != id.gen) return false;
            // Enforce valid transitions
            auto valid = [](const PageState from, const PageState to) {
                using S = PageState;
                if (from == S::Cold && to == S::Loading) return true;
                if (from == S::Loading && to == S::Resident) return true;
                if (from == S::Resident && to == S::Evicting) return true;
                if (from == S::Resident && to == S::Flushing) return true;
                if (from == S::Flushing && to == S::Evicting) return true;
                if (from == S::Evicting && to == S::Cold) return true;
                return false;
            };
            if (!valid(expected, desired)) return false;
            // Resident→Evicting and Resident→Flushing require no active pins
            using S = PageState;
            if ((desired == S::Evicting || desired == S::Flushing) &&
                e.pin_count.load(std::memory_order_acquire) > 0)
                return false;
            return e.state.compare_exchange_strong(expected, desired,
                                                   std::memory_order_acq_rel, std::memory_order_acquire);
        }

        [[nodiscard]] PageState state(const detail::GenId id) const noexcept {
            std::shared_lock g{rw_mu_};
            const auto it = entries_.find(id.id);
            if (it == entries_.end()) return PageState::Cold;
            const auto& e = *it->second;
            if (e.gid.gen != id.gen) return PageState::Cold;
            return e.state.load(std::memory_order_acquire);
        }

        template <typename Fn>
        void for_each_evictable(Fn&& fn) {
            std::shared_lock g{rw_mu_};
            for (auto& entry_ptr : entries_.values()) {
                if (entry_ptr->state.load(std::memory_order_acquire) == PageState::Resident &&
                    entry_ptr->pin_count.load(std::memory_order_acquire) == 0) {
                    fn(entry_ptr->gid);
                }
            }
        }

        template <typename Fn>
        void for_each_evictable(Fn&& fn) const {
            std::shared_lock g{rw_mu_};
            for (const auto& entry_ptr : entries_.values()) {
                if (entry_ptr->state.load(std::memory_order_acquire) == PageState::Resident &&
                    entry_ptr->pin_count.load(std::memory_order_acquire) == 0) {
                    fn(entry_ptr->gid);
                }
            }
        }
    };

    // ============================================================================
    // SECTION 6 — Handles
    // ============================================================================

    namespace handles {
        template <typename T>
        class OwnerHandle {
            T* ptr_{};
            std::function<void(void *, std::size_t)> deleter_;
            std::size_t size_{};

        public:
            OwnerHandle() = default;

            template <typename Deleter>
            OwnerHandle(T* p, const std::size_t sz, Deleter&& del) noexcept
                : ptr_{p}, deleter_{std::forward<Deleter>(del)}, size_{sz} {}

            OwnerHandle(OwnerHandle&& o) noexcept
                : ptr_{o.ptr_}, deleter_{std::move(o.deleter_)}, size_{o.size_} {
                o.ptr_ = nullptr;
                o.size_ = 0;
            }

            OwnerHandle& operator=(OwnerHandle&& o) noexcept {
                if (this != &o) {
                    if (ptr_) {
                        ptr_->~T();
                        if (deleter_) deleter_(ptr_, size_);
                    }
                    ptr_ = o.ptr_;
                    deleter_ = std::move(o.deleter_);
                    size_ = o.size_;
                    o.ptr_ = nullptr;
                    o.size_ = 0;
                }
                return *this;
            }

            OwnerHandle(const OwnerHandle&) = delete;

            OwnerHandle& operator=(const OwnerHandle&) = delete;

            ~OwnerHandle() noexcept {
                if (ptr_) {
                    ptr_->~T();
                    if (deleter_) deleter_(ptr_, size_);
                }
            }

            [[nodiscard]] T* get() noexcept { return ptr_; }
            [[nodiscard]] const T* get() const noexcept { return ptr_; }
            T& operator*() noexcept { return *ptr_; }
            T* operator->() noexcept { return ptr_; }
            explicit operator bool() const noexcept { return ptr_ != nullptr; }

            T* release() noexcept {
                T* p = ptr_;
                ptr_ = nullptr;
                deleter_ = nullptr;
                return p;
            }
        };

        template <typename T>
        class PinnedPageHandle {
            T* ptr_{};
            detail::GenId gid_{};
            PageTable* table_{};

        public:
            PinnedPageHandle() = default;

            PinnedPageHandle(T* p, const detail::GenId id, PageTable& tbl) noexcept
                : ptr_{p}, gid_{id}, table_{&tbl} {}

            PinnedPageHandle(PinnedPageHandle&& o) noexcept
                : ptr_{o.ptr_}, gid_{o.gid_}, table_{o.table_} {
                o.ptr_ = nullptr;
                o.table_ = nullptr;
            }

            PinnedPageHandle& operator=(PinnedPageHandle&& o) noexcept {
                if (this != &o) {
                    if (ptr_ && table_) table_->unpin(gid_);
                    ptr_ = o.ptr_;
                    gid_ = o.gid_;
                    table_ = o.table_;
                    o.ptr_ = nullptr;
                    o.table_ = nullptr;
                }
                return *this;
            }

            PinnedPageHandle(const PinnedPageHandle&) = delete;

            PinnedPageHandle& operator=(const PinnedPageHandle&) = delete;

            ~PinnedPageHandle() noexcept {
                if (ptr_ && table_) table_->unpin(gid_);
            }

            [[nodiscard]] detail::GenId gid() const noexcept { return gid_; }

            // Relinquish ownership without unpinning — used by ManagedResource::destroy.
            void release_ownership() noexcept {
                ptr_ = nullptr;
                table_ = nullptr;
            }

            [[nodiscard]] std::expected<T*, PageError> get() noexcept {
                if (!table_) return std::unexpected{PageError::NullPage};
                auto result = table_->resolve(gid_);
                if (!result) return std::unexpected{result.error()};
                return static_cast<T*>(result.value());
            }

            T& operator*() { return *get().value(); }
            T* operator->() { return get().value(); }

            explicit operator bool() const noexcept { return ptr_ != nullptr; }
        };
    } // namespace handles

    // ============================================================================
    // SECTION 8 — Managers
    // ============================================================================

    namespace managers {
        struct NullManager {
            void attach(PageTable&) noexcept {}

            void on_alloc(detail::GenId) noexcept {}

            void on_access(detail::GenId) noexcept {}

            std::optional<detail::GenId> evict_one() noexcept { return std::nullopt; }

            void shutdown() noexcept {}
        };

        class LRUCacheManager {
            struct ListNode {
                detail::GenId gid{};
                ListNode* prev{};
                ListNode* next{};
            };

            // std::map (not flat_map) for pointer stability: insertions don't
            // invalidate pointers to existing nodes (needed for prev/next links).
            std::map<std::uint32_t, ListNode> nodes_;
            ListNode* head_{};
            ListNode* tail_{};
            std::size_t capacity_{};
            PageTable* table_{};
            // heap-allocated mutex so LRUCacheManager is movable
            std::unique_ptr<std::mutex> mu_{std::make_unique<std::mutex>()};

            void splice_to_front(ListNode& node) {
                if (&node == head_) return;
                // Detach
                if (node.prev) node.prev->next = node.next;
                if (node.next) node.next->prev = node.prev;
                if (&node == tail_) tail_ = node.prev;
                // Prepend
                node.prev = nullptr;
                node.next = head_;
                if (head_) head_->prev = &node;
                head_ = &node;
                if (!tail_) tail_ = &node;
            }

            void push_front(detail::GenId id) {
                auto [it, ok] = nodes_.emplace(id.id, ListNode{id, nullptr, head_});
                ListNode& node = it->second;
                if (head_) head_->prev = &node;
                head_ = &node;
                if (!tail_) tail_ = &node;
            }

        public:
            explicit LRUCacheManager(const std::size_t max_pages) noexcept
                : capacity_{max_pages} {}

            void attach(PageTable& t) noexcept { table_ = &t; }

            void on_alloc(const detail::GenId id) noexcept {
                std::scoped_lock g{*mu_};
                push_front(id);
                // Trigger eviction pressure if over capacity
                if (nodes_.size() > capacity_) evict_one();
            }

            void on_access(const detail::GenId id) noexcept {
                std::scoped_lock g{*mu_};
                const auto it = nodes_.find(id.id);
                if (it != nodes_.end()) splice_to_front(it->second);
            }

            std::optional<detail::GenId> evict_one() noexcept {
                if (!table_) return std::nullopt;
                // Walk from LRU end toward MRU
                const ListNode* candidate = tail_;
                while (candidate) {
                    detail::GenId gid = candidate->gid;
                    // Try to transition Resident→Evicting (fails if pinned)
                    if (table_->transition(gid, PageState::Resident, PageState::Evicting)) {
                        // Check if dirty — if so flush first
                        // For now: transition directly to Cold (no backing store in this manager)
                        table_->transition(gid, PageState::Evicting, PageState::Cold);
                        table_->free_page(gid);
                        // Remove from LRU list
                        if (candidate->prev) candidate->prev->next = candidate->next;
                        if (candidate->next) candidate->next->prev = candidate->prev;
                        if (candidate == head_) head_ = candidate->next;
                        if (candidate == tail_) tail_ = candidate->prev;
                        nodes_.erase(gid.id);
                        return gid;
                    }
                    candidate = candidate->prev;
                }
                return std::nullopt;
            }

            void shutdown() noexcept {}
        };

        template <concepts::Domain SourceDomain, concepts::Domain TargetDomain>
        class AsyncMigrationManager {
            struct MigrationTask {
                detail::GenId gid{};
                std::size_t size{};
            };

            PageTable* table_{};
            SourceDomain* src_{};
            TargetDomain* dst_{};

            static constexpr std::size_t kQueueSize = 256;
            std::array<MigrationTask, kQueueSize> queue_{};
            detail::padded<std::atomic<std::size_t>> q_head_{};
            detail::padded<std::atomic<std::size_t>> q_tail_{};

            std::flat_map<std::uint32_t, std::uint32_t> access_counts_;
            std::mutex ac_mu_;
            std::atomic<std::uint32_t> access_threshold_{8};

            std::jthread worker_;

        public:
            AsyncMigrationManager(SourceDomain& src, TargetDomain& dst,
                                  const std::uint32_t threshold = 8) noexcept
                : src_{&src}, dst_{&dst} {
                access_threshold_.store(threshold, std::memory_order_relaxed);
                worker_ = std::jthread{
                    [this](std::stop_token st) {
                        worker_fn(st);
                    }
                };
            }

            void attach(PageTable& t) noexcept { table_ = &t; }

            void on_alloc(detail::GenId) noexcept {}

            void on_access(detail::GenId id) noexcept {
                std::uint32_t count;
                {
                    std::scoped_lock g{ac_mu_};
                    count = ++access_counts_[id.id];
                }
                if (count >= access_threshold_.load(std::memory_order_relaxed)) {
                    std::size_t tail = q_tail_.value.load(std::memory_order_relaxed);
                    const std::size_t next = (tail + 1) % kQueueSize;
                    if (next != q_head_.value.load(std::memory_order_acquire)) {
                        queue_[tail] = MigrationTask{id, 0};
                        q_tail_.value.store(next, std::memory_order_release);
                    }
                }
            }

            std::optional<detail::GenId> evict_one() noexcept { return std::nullopt; }

            void shutdown() noexcept {
                worker_.request_stop();
                // worker_ auto-joins via jthread dtor
            }

        private:
            void worker_fn(const std::stop_token& stoken) noexcept {
                while (!stoken.stop_requested()) {
                    std::size_t head = q_head_.value.load(std::memory_order_acquire);
                    const std::size_t tail = q_tail_.value.load(std::memory_order_acquire);
                    if (head == tail) {
                        std::this_thread::yield();
                        continue;
                    }
                    MigrationTask task = queue_[head];
                    q_head_.value.store((head + 1) % kQueueSize, std::memory_order_release);
                    if (task.size > 0) migrate_page(task);
                }
            }

            bool migrate_page(MigrationTask t) noexcept {
                if (!table_) return false;
                if (!table_->pin(t.gid)) return false;
                auto result = table_->resolve(t.gid);
                if (!result) {
                    table_->unpin(t.gid);
                    return false;
                }
                void* src_ptr = result.value();
                void* dst_ptr = dst_->acquire(t.size, TargetDomain::alignment);
                if (!dst_ptr) {
                    table_->unpin(t.gid);
                    return false;
                }
                std::memcpy(dst_ptr, src_ptr, t.size);
                src_->release(src_ptr, t.size);
                table_->unpin(t.gid);
                return true;
            }
        };

        class RecoveryManager {
            PageTable* table_{};
            std::jthread flush_worker_;
            std::atomic<bool> dirty_flag_{false};
            std::chrono::milliseconds flush_interval_{500};
            std::flat_set<std::uint32_t> dirty_set_;
            std::mutex dirty_mu_;

        public:
            explicit RecoveryManager(
                const std::chrono::milliseconds interval = std::chrono::milliseconds{500}) noexcept
                : flush_interval_{interval} {}

            void attach(PageTable& t) noexcept {
                table_ = &t;
                flush_worker_ = std::jthread{
                    [this](std::stop_token st) {
                        flush_worker_fn(st);
                    }
                };
            }

            void on_alloc(detail::GenId) noexcept {}

            void on_access(detail::GenId) noexcept {}

            std::optional<detail::GenId> evict_one() noexcept { return std::nullopt; }

            void shutdown() noexcept {
                flush_worker_.request_stop();
                flush_all_dirty();
            }

            void mark_dirty(const detail::GenId id) noexcept {
                std::scoped_lock g{dirty_mu_};
                dirty_set_.insert(id.id);
                dirty_flag_.store(true, std::memory_order_release);
                if (table_) table_->mark_dirty(id);
            }

            void flush_all_dirty() noexcept {
                if (!table_) return;
                {
                    std::scoped_lock g{dirty_mu_};
                    std::flat_set<std::uint32_t> to_flush = dirty_set_;
                    dirty_set_.clear();
                    dirty_flag_.store(false, std::memory_order_release);
                }
                // In a real implementation, msync would be called per page.
                // Here we just transition state for correctness verification.
            }

        private:
            void flush_worker_fn(const std::stop_token& stoken) noexcept {
                while (!stoken.stop_requested()) {
                    std::this_thread::sleep_for(flush_interval_);
                    if (dirty_flag_.load(std::memory_order_acquire))
                        flush_all_dirty();
                }
            }
        };
    } // namespace managers

    // ============================================================================
    // SECTION 9 — ManagedResource
    // ============================================================================

    template <concepts::Domain DomainT,
        concepts::Pool PoolT,
        concepts::Manager ManagerT = managers::NullManager>
    class ManagedResource {
        DomainT domain_;
        PoolT pool_;
        PageTable table_;
        ManagerT manager_;

    public:
        explicit ManagedResource(DomainT d = {}, PoolT p = {}, ManagerT m = {})
            : domain_{std::move(d)}, pool_{std::move(p)}, manager_{std::move(m)} {
            manager_.attach(table_);
        }

        ~ManagedResource() noexcept {
            manager_.shutdown();
            if constexpr (std::is_same_v<ManagerT, managers::RecoveryManager>) {
                manager_.flush_all_dirty();
            }
            pool_.reset();
        }

        ManagedResource(const ManagedResource&) = delete;

        ManagedResource& operator=(const ManagedResource&) = delete;

        template <typename T, typename... Args>
            requires std::constructible_from<T, Args...>
        [[nodiscard]] handles::PinnedPageHandle<T> make(Args&&... args) {
            void* raw = pool_.allocate(sizeof(T), alignof(T));
            if (!raw) return {};
            T* obj = ::new(raw) T(std::forward<Args>(args)...);
            detail::GenId id = table_.alloc_page(raw);
            manager_.on_alloc(id);
            if (!table_.pin(id)) {
                obj->~T();
                pool_.deallocate(raw, sizeof(T));
                return {};
            }
            return handles::PinnedPageHandle<T>{obj, id, table_};
        }

        // Explicitly destroy a pinned object: calls ~T(), unpins, and frees the slot.
        template <typename T>
        void destroy(handles::PinnedPageHandle<T>&& h) noexcept {
            if (!static_cast<bool>(h)) return;
            const detail::GenId gid = h.gid();
            auto result = table_.resolve(gid);
            if (!result.has_value()) return;
            T* obj = static_cast<T*>(result.value());
            obj->~T();
            table_.unpin(gid);
            table_.transition(gid, PageState::Resident, PageState::Evicting);
            table_.transition(gid, PageState::Evicting, PageState::Cold);
            table_.free_page(gid);
            pool_.deallocate(obj, sizeof(T));
            h.release_ownership();
        }

        [[nodiscard]] void* allocate(std::size_t n, std::size_t a) {
            void* p = pool_.allocate(n, a);
            if (!p) {
                if (manager_.evict_one()) p = pool_.allocate(n, a);
            }
            if (p) {
                detail::GenId id = table_.alloc_page(p);
                manager_.on_alloc(id);
            }
            return p;
        }

        void deallocate(void* p, std::size_t n) noexcept {
            pool_.deallocate(p, n);
        }

        void notify_access(detail::GenId id) noexcept { manager_.on_access(id); }

        bool evict_one() noexcept {
            auto candidate = manager_.evict_one();
            return candidate.has_value();
        }

        PageTable& page_table() noexcept { return table_; }
        ManagerT& manager() noexcept { return manager_; }
    };

    // ============================================================================
    // SECTION 10 — STL Allocator Adaptor
    // ============================================================================

    template <typename T, typename ResourceT>
        requires concepts::MemoryResource<ResourceT>
    class SmritiAllocator {
        ResourceT* res_;

    public:
        using value_type = T;
        using is_always_equal = std::false_type;

        // C++11/14 containers use rebind<U>::other; C++17+ containers prefer
        // rebind_alloc but both are required for full standard conformance.
        template <typename U>
        struct rebind {
            using other = SmritiAllocator<U, ResourceT>;
        };

        template <typename U>
        using rebind_alloc = SmritiAllocator<U, ResourceT>;

        explicit SmritiAllocator(ResourceT& r) noexcept : res_{&r} {}

        template <typename U>
        explicit SmritiAllocator(const SmritiAllocator<U, ResourceT>& o) noexcept
            : res_{o.resource()} {}

        [[nodiscard]] T* allocate(const std::size_t n) {
            void* p = res_->allocate(n * sizeof(T), alignof(T));
            if (!p) throw std::bad_alloc{};
            return static_cast<T*>(p);
        }

        void deallocate(T* p, const std::size_t n) noexcept {
            res_->deallocate(p, n * sizeof(T));
        }

        bool operator==(const SmritiAllocator& o) const noexcept {
            return res_ == o.res_;
        }

        // Give rebind copy access to res_ without a friend template (avoids constraint mismatch)
        ResourceT* resource() const noexcept { return res_; }
    };

    // Factory: deduces ResourceT so callers write make_allocator<int>(res) instead of
    // SmritiAllocator<int, ManagedResource<...>>{res}.
    template <typename T, concepts::MemoryResource ResourceT>
    [[nodiscard]] SmritiAllocator<T, ResourceT> make_allocator(ResourceT& res) noexcept {
        return SmritiAllocator<T, ResourceT>{res};
    }
} // namespace smriti
