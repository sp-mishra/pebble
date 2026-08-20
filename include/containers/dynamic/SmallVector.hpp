#pragma once
// ============================================================================
// SmallVector<T, InlineBytes, Alloc>
// ============================================================================
// A small-buffer-optimised dynamic array.
//
// Inline storage
// --------------
//   InlineBytes is a byte budget, not an element count.
//   kInlineCap = InlineBytes / sizeof(T)  (≥1 when T fits, else 0)
//   Examples:
//     SmallVector<int,   64>  → 16 ints inline  (1 cache line)
//     SmallVector<float, 32>  →  8 floats inline
//     SmallVector<int,    2>  →  0 ints inline (T > budget → heap-only)
//
//   data_ always points to the live elements: either into the inline_ buffer
//   or into allocator-owned heap storage.  No separate "spilled" flag — a
//   pointer comparison with inline_ptr() is sufficient and branch-free.
//
// Allocator
// ---------
//   Alloc defaults to std::allocator<T> so the class works with no smriti
//   setup at all.  Pass SmritiAllocator<T, ResourceT> to spill into a
//   smriti pool instead of the global heap.
//
//   Fully allocator_traits-correct: POCCA / POCMA / POCS propagation,
//   construct / destroy routed through traits.
//
// API
// ---
//   std::vector-compatible subset:
//     push_back, emplace_back, pop_back
//     operator[], at, front, back
//     begin/end/cbegin/cend (raw pointer iterators — zero overhead)
//     size, empty, capacity
//     reserve, resize, clear, shrink_to_fit
//     erase(pos), erase(first, last), insert(pos, val)
//     copy & move ctor/assign, swap, get_allocator
//     std::initializer_list ctor
//
// Guarantees
// ----------
//   - Strong guarantee on push_back / emplace_back
//   - Basic guarantee on resize / assign
//   - noexcept move ctor / move assign when T is nothrow-move-constructible
//   - No dynamic allocation while size ≤ kInlineCap
//   - No virtual, no RTTI, no macros
//
// Usage
//   SmallVector<int, 64> v;               // 16 ints inline, std::allocator overflow
//   v.push_back(1);                       // no heap alloc
//
//   BumpPool<SystemRAMDomain> pool{4096};
//   SmallVector<int, 64, SmritiAllocator<int, decltype(pool)>> sv{
//       SmritiAllocator<int, decltype(pool)>{pool}};
// ============================================================================

#include <algorithm>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace containers::dynamic::detail {
    // Minimum element capacity computed from a byte budget.
    // Returns 0 when T does not fit (heap-only mode).
    template <typename T, std::size_t InlineBytes>
    inline constexpr std::size_t inline_cap =
        (InlineBytes >= sizeof(T)) ? (InlineBytes / sizeof(T)) : 0;
} // namespace containers::dynamic::detail

namespace containers::dynamic {
    template <
        typename T,
        std::size_t InlineBytes = 64,
        typename Alloc = std::allocator<T>>
    class SmallVector {
        static_assert(!std::is_const_v<T>, "SmallVector<const T> is ill-formed");
        static_assert(!std::is_reference_v<T>, "SmallVector<T&> is ill-formed");
        static_assert(std::is_destructible_v<T>);

        using AllocTraits = std::allocator_traits<Alloc>;

    public:
        // ---- types -------------------------------------------------------
        using value_type = T;
        using allocator_type = Alloc;
        using size_type = std::size_t;
        using difference_type = std::ptrdiff_t;
        using reference = T&;
        using const_reference = const T&;
        using pointer = T*;
        using const_pointer = const T*;
        using iterator = T*;
        using const_iterator = const T*;
        using reverse_iterator = std::reverse_iterator<iterator>;
        using const_reverse_iterator = std::reverse_iterator<const_iterator>;

        static constexpr size_type kInlineCap = detail::inline_cap<T, InlineBytes>;

    private:
        // ---- storage -----------------------------------------------------
        // Declaration order matters: C++ initializes in this order.
        // Inline buffer — always at least 1 byte to avoid zero-length array
        // (zero-length arrays are a GCC extension, not standard C++).
        // When kInlineCap == 0 this wastes sizeof(T)-1 bytes at most, which
        // is acceptable as a degenerate edge case.
        static constexpr size_type kBufBytes = kInlineCap > 0 ? InlineBytes : 1;
        alignas(T) std::byte inline_[kBufBytes];

        T* data_{};
        size_type size_{};
        size_type cap_{};
        [[no_unique_address]] Alloc alloc_{};

        // ---- inline helpers ---------------------------------------------
        [[nodiscard]] T* inline_ptr() noexcept {
            return std::launder(reinterpret_cast<T*>(inline_));
        }

        [[nodiscard]] const T* inline_ptr() const noexcept {
            return std::launder(reinterpret_cast<const T*>(inline_));
        }

        [[nodiscard]] bool is_inline() const noexcept {
            return data_ == inline_ptr();
        }

        // ---- allocation helpers -----------------------------------------

        // Allocate raw storage for n elements; does not construct.
        [[nodiscard]] T* alloc_n(size_type n) {
            return AllocTraits::allocate(alloc_, n);
        }

        // Free raw storage for n elements; does not destroy.
        void free_n(T* p, size_type n) noexcept {
            AllocTraits::deallocate(alloc_, p, n);
        }

        // Destroy elements in [first, last).
        void destroy_range(T* first, T* last) noexcept {
            if constexpr (!std::is_trivially_destructible_v<T>) {
                for (; first != last; ++first)
                    AllocTraits::destroy(alloc_, first);
            }
        }

        // Uninitialized move [src, src+n) into dst; src elements are left
        // in a valid but unspecified state.
        void uninit_move(T* src, const size_type n, T* dst)
            noexcept(std::is_nothrow_move_constructible_v<T>) {
            if constexpr (std::is_trivially_copyable_v<T>) {
                std::memcpy(dst, src, n * sizeof(T));
            }
            else {
                for (size_type i = 0; i < n; ++i)
                    AllocTraits::construct(alloc_, dst + i, std::move(src[i]));
            }
        }

        // Uninitialized copy [src, src+n) into dst.
        void uninit_copy(const T* src, const size_type n, T* dst) {
            if constexpr (std::is_trivially_copyable_v<T>) {
                std::memcpy(dst, src, n * sizeof(T));
            }
            else {
                size_type i = 0;
                try {
                    for (; i < n; ++i)
                        AllocTraits::construct(alloc_, dst + i, src[i]);
                }
                catch (...) {
                    destroy_range(dst, dst + i);
                    throw;
                }
            }
        }

        // Grow to hold at least min_cap elements.
        // Growth policy: 1.5x + 1, overflow-safe near SIZE_MAX.
        void grow_to(const size_type min_cap) {
            constexpr size_type kMax = std::numeric_limits<size_type>::max();
            const size_type new_cap = [&]() noexcept -> size_type {
                if (cap_ >= kMax / 2)
                    return min_cap > cap_ ? min_cap : (cap_ < kMax ? cap_ + 1 : cap_);
                const size_type grown = cap_ + cap_ / 2 + 1;
                return grown > min_cap ? grown : min_cap;
            }();
            T* new_data = alloc_n(new_cap);
            try {
                uninit_move(data_, size_, new_data);
            }
            catch (...) {
                free_n(new_data, new_cap);
                throw;
            }
            destroy_range(data_, data_ + size_);
            if (!is_inline()) free_n(data_, cap_);
            data_ = new_data;
            cap_ = new_cap;
        }

        void ensure_capacity(const size_type extra = 1) {
            if (extra > std::numeric_limits<size_type>::max() - size_)
                throw std::length_error{"SmallVector: size overflow"};
            if (size_ + extra > cap_) grow_to(size_ + extra);
        }

    public:
        // ---- constructors -----------------------------------------------

        SmallVector() noexcept(std::is_nothrow_default_constructible_v<Alloc>)
            : inline_{}, data_{inline_ptr()}, cap_{kInlineCap} {}

        explicit SmallVector(const Alloc& a)
            noexcept(std::is_nothrow_copy_constructible_v<Alloc>)
            : inline_{}, data_{inline_ptr()}, cap_{kInlineCap}, alloc_{a} {}

        explicit SmallVector(const size_type n, const Alloc& a = Alloc{})
            : inline_{}, data_{inline_ptr()}, cap_{kInlineCap}, alloc_{a} {
            resize(n);
        }

        SmallVector(const size_type n, const T& val, const Alloc& a = Alloc{})
            : inline_{}, data_{inline_ptr()}, cap_{kInlineCap}, alloc_{a} {
            resize(n, val);
        }

        SmallVector(std::initializer_list<T> il, const Alloc& a = Alloc{})
            : inline_{}, data_{inline_ptr()}, cap_{kInlineCap}, alloc_{a} {
            reserve(il.size());
            uninit_copy(il.begin(), il.size(), data_);
            size_ = il.size();
        }

        // Range constructor
        template <std::input_iterator It>
        SmallVector(It first, It last, const Alloc& a = Alloc{})
            : inline_{}, data_{inline_ptr()}, cap_{kInlineCap}, alloc_{a} {
            if constexpr (std::forward_iterator < It >) {
                reserve(static_cast<size_type>(std::distance(first, last)));
            }
            for (; first != last; ++first) emplace_back(*first);
        }

        // Copy constructor
        SmallVector(const SmallVector& o)
            : inline_{},
              data_{inline_ptr()},
              cap_{kInlineCap},
              alloc_{AllocTraits::select_on_container_copy_construction(o.alloc_)} {
            reserve(o.size_);
            uninit_copy(o.data_, o.size_, data_);
            size_ = o.size_;
        }

        // Copy constructor with explicit allocator
        SmallVector(const SmallVector& o, const Alloc& a)
            : inline_{}, data_{inline_ptr()}, cap_{kInlineCap}, alloc_{a} {
            reserve(o.size_);
            uninit_copy(o.data_, o.size_, data_);
            size_ = o.size_;
        }

        // Move constructor
        SmallVector(SmallVector&& o)
            noexcept(std::is_nothrow_move_constructible_v<T> &&
                std::is_nothrow_move_constructible_v<Alloc>)
            : inline_{}, data_{inline_ptr()}, cap_{kInlineCap}, alloc_{std::move(o.alloc_)} {
            move_from(std::move(o));
        }

        // Move constructor with explicit allocator
        SmallVector(SmallVector&& o, const Alloc& a)
            : inline_{}, data_{inline_ptr()}, cap_{kInlineCap}, alloc_{a} {
            if (a == o.alloc_) {
                move_from(std::move(o));
            }
            else {
                // Different allocator: cannot steal heap storage; element-move.
                reserve(o.size_);
                uninit_move(o.data_, o.size_, data_);
                size_ = o.size_;
                o.destroy_range(o.data_, o.data_ + o.size_);
                o.size_ = 0;
                if (!o.is_inline()) {
                    o.free_n(o.data_, o.cap_);
                    o.data_ = o.inline_ptr();
                    o.cap_ = kInlineCap;
                }
            }
        }

        ~SmallVector() {
            destroy_range(data_, data_ + size_);
            if (!is_inline()) free_n(data_, cap_);
        }

        // ---- assignment -------------------------------------------------

        SmallVector& operator=(const SmallVector& o) {
            if (this == &o) return *this;
            if constexpr (AllocTraits::propagate_on_container_copy_assignment::value) {
                if (alloc_ != o.alloc_) {
                    // Must reallocate with new allocator — clear first.
                    clear();
                    if (!is_inline()) {
                        free_n(data_, cap_);
                        data_ = inline_ptr();
                        cap_ = kInlineCap;
                    }
                    alloc_ = o.alloc_;
                }
            }
            assign_from(o.data_, o.size_);
            return *this;
        }

        SmallVector& operator=(SmallVector&& o)
            noexcept(AllocTraits::propagate_on_container_move_assignment::value &&
                std::is_nothrow_move_constructible_v<T>) {
            if (this == &o) return *this;
            destroy_range(data_, data_ + size_);
            if (!is_inline()) free_n(data_, cap_);
            size_ = 0;
            data_ = inline_ptr();
            cap_ = kInlineCap;

            if constexpr (AllocTraits::propagate_on_container_move_assignment::value) {
                alloc_ = std::move(o.alloc_);
                move_from(std::move(o));
            }
            else if (alloc_ == o.alloc_) {
                move_from(std::move(o));
            }
            else {
                // Cannot steal; element-move.
                reserve(o.size_);
                uninit_move(o.data_, o.size_, data_);
                size_ = o.size_;
                o.destroy_range(o.data_, o.data_ + o.size_);
                o.size_ = 0;
                if (!o.is_inline()) {
                    o.free_n(o.data_, o.cap_);
                    o.data_ = o.inline_ptr();
                    o.cap_ = kInlineCap;
                }
            }
            return *this;
        }

        SmallVector& operator=(std::initializer_list<T> il) {
            assign_from(il.begin(), il.size());
            return *this;
        }

        // ---- element access --------------------------------------------

        [[nodiscard]] reference operator[](size_type i) noexcept { return data_[i]; }
        [[nodiscard]] const_reference operator[](size_type i) const noexcept { return data_[i]; }

        [[nodiscard]] reference at(size_type i) {
            if (i >= size_) throw std::out_of_range{"SmallVector::at"};
            return data_[i];
        }

        [[nodiscard]] const_reference at(size_type i) const {
            if (i >= size_) throw std::out_of_range{"SmallVector::at"};
            return data_[i];
        }

        [[nodiscard]] reference front() noexcept { return data_[0]; }
        [[nodiscard]] const_reference front() const noexcept { return data_[0]; }
        [[nodiscard]] reference back() noexcept { return data_[size_ - 1]; }
        [[nodiscard]] const_reference back() const noexcept { return data_[size_ - 1]; }
        [[nodiscard]] pointer data() noexcept { return data_; }
        [[nodiscard]] const_pointer data() const noexcept { return data_; }

        // ---- iterators -------------------------------------------------

        [[nodiscard]] iterator begin() noexcept { return data_; }
        [[nodiscard]] const_iterator begin() const noexcept { return data_; }
        [[nodiscard]] iterator end() noexcept { return data_ + size_; }
        [[nodiscard]] const_iterator end() const noexcept { return data_ + size_; }
        [[nodiscard]] const_iterator cbegin() const noexcept { return data_; }
        [[nodiscard]] const_iterator cend() const noexcept { return data_ + size_; }

        [[nodiscard]] reverse_iterator rbegin() noexcept { return reverse_iterator{end()}; }
        [[nodiscard]] const_reverse_iterator rbegin() const noexcept { return const_reverse_iterator{end()}; }
        [[nodiscard]] reverse_iterator rend() noexcept { return reverse_iterator{begin()}; }
        [[nodiscard]] const_reverse_iterator rend() const noexcept { return const_reverse_iterator{begin()}; }

        // ---- capacity --------------------------------------------------

        [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
        [[nodiscard]] size_type size() const noexcept { return size_; }
        [[nodiscard]] size_type capacity() const noexcept { return cap_; }
        [[nodiscard]] bool spilled() const noexcept { return !is_inline(); }

        void reserve(const size_type n) {
            if (n > cap_) grow_to(n);
        }

        // Try to collapse back into inline storage; no-op if T is not
        // nothrow-moveable (we refuse to leave a partially-moved mess).
        void shrink_to_fit() noexcept {
            if (is_inline()) return;
            if constexpr (!std::is_nothrow_move_constructible_v<T>) return;

            if (size_ <= kInlineCap) {
                T* new_data = inline_ptr();
                uninit_move(data_, size_, new_data);
                destroy_range(data_, data_ + size_);
                free_n(data_, cap_);
                data_ = new_data;
                cap_ = kInlineCap;
            }
        }

        // ---- modifiers -------------------------------------------------

        void clear() noexcept {
            destroy_range(data_, data_ + size_);
            size_ = 0;
        }

        void push_back(const T& v) {
            ensure_capacity();
            AllocTraits::construct(alloc_, data_ + size_, v);
            ++size_;
        }

        void push_back(T&& v) {
            ensure_capacity();
            AllocTraits::construct(alloc_, data_ + size_, std::move(v));
            ++size_;
        }

        template <typename... Args>
        reference emplace_back(Args&&... args) {
            ensure_capacity();
            AllocTraits::construct(alloc_, data_ + size_, std::forward<Args>(args)...);
            return data_[size_++];
        }

        void pop_back() noexcept {
            assert(size_ > 0);
            --size_;
            AllocTraits::destroy(alloc_, data_ + size_);
        }

        void resize(size_type n) {
            if (n <= size_) {
                destroy_range(data_ + n, data_ + size_);
                size_ = n;
            }
            else {
                reserve(n);
                for (size_type i = size_; i < n; ++i)
                    AllocTraits::construct(alloc_, data_ + i);
                size_ = n;
            }
        }

        void resize(size_type n, const T& val) {
            if (n <= size_) {
                destroy_range(data_ + n, data_ + size_);
                size_ = n;
            }
            else {
                reserve(n);
                for (size_type i = size_; i < n; ++i)
                    AllocTraits::construct(alloc_, data_ + i, val);
                size_ = n;
            }
        }

        // erase single element; returns iterator to element after erased.
        iterator erase(const_iterator pos) noexcept(std::is_nothrow_move_assignable_v<T>) {
            assert(pos >= cbegin() && pos < cend());
            T* p = data_ + (pos - cbegin());
            std::move(p + 1, data_ + size_, p);
            --size_;
            AllocTraits::destroy(alloc_, data_ + size_);
            return p;
        }

        // erase range [first, last); returns iterator to element after range.
        iterator erase(const_iterator first, const_iterator last)
            noexcept(std::is_nothrow_move_assignable_v<T>) {
            assert(first <= last && first >= cbegin() && last <= cend());
            T* f = data_ + (first - cbegin());
            T* l = data_ + (last - cbegin());
            const auto count = static_cast<size_type>(l - f);
            std::move(l, data_ + size_, f);
            destroy_range(data_ + size_ - count, data_ + size_);
            size_ -= count;
            return f;
        }

        // insert single element before pos; returns iterator to inserted element.
        // Precondition: val must not alias any element in [data_, data_+size_).
        iterator insert(const_iterator pos, const T& val) {
            assert(&val < data_ || &val >= data_ + size_);
            auto idx = static_cast<size_type>(pos - cbegin());
            ensure_capacity();
            T* p = data_ + idx;
            // Shift right by one to make room
            if (idx < size_) {
                AllocTraits::construct(alloc_, data_ + size_, std::move(data_[size_ - 1]));
                std::move_backward(p, data_ + size_ - 1, data_ + size_);
                AllocTraits::destroy(alloc_, p);
            }
            AllocTraits::construct(alloc_, p, val);
            ++size_;
            return p;
        }

        iterator insert(const_iterator pos, T&& val) {
            auto idx = static_cast<size_type>(pos - cbegin());
            ensure_capacity();
            T* p = data_ + idx;
            if (idx < size_) {
                AllocTraits::construct(alloc_, data_ + size_, std::move(data_[size_ - 1]));
                std::move_backward(p, data_ + size_ - 1, data_ + size_);
                AllocTraits::destroy(alloc_, p);
            }
            AllocTraits::construct(alloc_, p, std::move(val));
            ++size_;
            return p;
        }

        void swap(SmallVector& o)
            noexcept(std::is_nothrow_move_constructible_v<T> &&
                std::is_nothrow_swappable_v<Alloc>) {
            if (this == &o) return;

            assert(AllocTraits::propagate_on_container_swap::value || alloc_ == o.alloc_);

            if constexpr (AllocTraits::propagate_on_container_swap::value) {
                std::swap(alloc_, o.alloc_);
            }

            // If both are heap-allocated we can just swap pointers.
            // Otherwise, we need to move elements (possibly back into inline buf).
            if (!is_inline() && !o.is_inline()) {
                std::swap(data_, o.data_);
                std::swap(size_, o.size_);
                std::swap(cap_, o.cap_);
                return;
            }

            // General case: element-by-element exchange.
            SmallVector tmp{std::move(*this)};
            *this = std::move(o);
            o = std::move(tmp);
        }

        [[nodiscard]] Alloc get_allocator() const noexcept { return alloc_; }

        // ---- comparison ------------------------------------------------

        [[nodiscard]] bool operator==(const SmallVector& o) const
            noexcept(noexcept(std::declval<T>() == std::declval<T>()))
            requires std::equality_comparable<T> {
            return size_ == o.size_ && std::ranges::equal(begin(), end(), o.begin(), o.end());
        }

        [[nodiscard]] auto operator<=>(const SmallVector& o) const
            requires std::three_way_comparable<T> {
            return std::lexicographical_compare_three_way(
                begin(), end(), o.begin(), o.end());
        }

    private:
        // ---- private move-from helper -----------------------------------

        // Called only when alloc_ == o.alloc_ (or POCMA propagated).
        void move_from(SmallVector&& o)
            noexcept(std::is_nothrow_move_constructible_v<T>) {
            if (!o.is_inline()) {
                // Steal heap pointer.
                data_ = o.data_;
                size_ = o.size_;
                cap_ = o.cap_;
                o.data_ = o.inline_ptr();
                o.size_ = 0;
                o.cap_ = kInlineCap;
            }
            else {
                // Element-by-element move into our inline buffer.
                uninit_move(o.data_, o.size_, inline_ptr());
                size_ = o.size_;
                cap_ = kInlineCap;
                o.destroy_range(o.data_, o.data_ + o.size_);
                o.size_ = 0;
            }
        }

        // ---- private assign-from helper ---------------------------------
        void assign_from(const T* src, size_type n) {
            if (n <= size_) {
                // Copy into existing, destroy excess.
                if constexpr (std::is_trivially_copy_assignable_v<T>) {
                    std::memcpy(data_, src, n * sizeof(T));
                }
                else {
                    for (size_type i = 0; i < n; ++i) data_[i] = src[i];
                }
                destroy_range(data_ + n, data_ + size_);
            }
            else {
                reserve(n);
                // Copy into existing slots, then construct the new tail.
                const size_type old = size_;
                if constexpr (std::is_trivially_copy_assignable_v<T>) {
                    std::memcpy(data_, src, n * sizeof(T));
                }
                else {
                    for (size_type i = 0; i < old; ++i) data_[i] = src[i];
                    size_type i = old;
                    try {
                        for (; i < n; ++i)
                            AllocTraits::construct(alloc_, data_ + i, src[i]);
                    }
                    catch (...) {
                        size_ = i;
                        throw;
                    }
                }
            }
            size_ = n;
        }
    };

    // ---- free swap --------------------------------------------------------
    template <typename T, std::size_t N, typename A>
    void swap(SmallVector<T, N, A>& a, SmallVector<T, N, A>& b)
        noexcept(noexcept(a.swap(b))) {
        a.swap(b);
    }
} // namespace containers::dynamic
