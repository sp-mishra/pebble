#pragma once

// containers/static/static_vector.hpp — Constexpr fixed-capacity vector.
//
// C++23, header-only, no virtual, no macros. Namespace: containers
//
// static_vector<T, N> — std::array-backed, no heap, constexpr-compatible.
//   push_back / emplace_back return bool (false on overflow — sticky flag set).
//   All operations callable in consteval context.
//   Overflow is recorded and queryable; no throw, no UB on capacity breach.
//
// Sizing: sizeof(static_vector<T,N>) == sizeof(std::array<T,N>) + sizeof(std::size_t).

#include <array>
#include <cstddef>
#include <utility>

namespace containers {
    template <class T, std::size_t N>
    class static_vector {
    public:
        using value_type = T;
        using size_type = std::size_t;
        using reference = T&;
        using const_reference = const T&;
        using iterator = T*;
        using const_iterator = const T*;

        static constexpr size_type static_capacity = N;

        // ---- Capacity / size ------------------------------------------------

        [[nodiscard]] constexpr size_type size() const noexcept { return size_; }
        [[nodiscard]] constexpr size_type capacity() const noexcept { return N; }
        [[nodiscard]] constexpr bool empty() const noexcept { return size_ == 0; }
        [[nodiscard]] constexpr bool overflow() const noexcept { return overflow_; }

        // ---- Modification ---------------------------------------------------

        [[nodiscard]] constexpr bool push_back(const T& v) noexcept {
            if (size_ >= N) {
                overflow_ = true;
                return false;
            }
            buf_[size_++] = v;
            return true;
        }

        [[nodiscard]] constexpr bool push_back(T&& v) noexcept {
            if (size_ >= N) {
                overflow_ = true;
                return false;
            }
            buf_[size_++] = std::move(v);
            return true;
        }

        template <class... Args>
        [[nodiscard]] constexpr bool emplace_back(Args&&... args) noexcept {
            if (size_ >= N) {
                overflow_ = true;
                return false;
            }
            buf_[size_++] = T(std::forward<Args>(args)...);
            return true;
        }

        constexpr void pop_back() noexcept {
            if (size_ > 0) --size_;
        }

        constexpr void clear() noexcept {
            size_ = 0;
            overflow_ = false;
        }

        // ---- Element access -------------------------------------------------

        [[nodiscard]] constexpr reference operator[](size_type i) noexcept { return buf_[i]; }
        [[nodiscard]] constexpr const_reference operator[](size_type i) const noexcept { return buf_[i]; }

        [[nodiscard]] constexpr reference back() noexcept { return buf_[size_ - 1]; }
        [[nodiscard]] constexpr const_reference back() const noexcept { return buf_[size_ - 1]; }

        [[nodiscard]] constexpr T* data() noexcept { return buf_.data(); }
        [[nodiscard]] constexpr const T* data() const noexcept { return buf_.data(); }

        // ---- Iteration ------------------------------------------------------

        [[nodiscard]] constexpr iterator begin() noexcept { return buf_.data(); }
        [[nodiscard]] constexpr iterator end() noexcept { return buf_.data() + size_; }
        [[nodiscard]] constexpr const_iterator begin() const noexcept { return buf_.data(); }
        [[nodiscard]] constexpr const_iterator end() const noexcept { return buf_.data() + size_; }
        [[nodiscard]] constexpr const_iterator cbegin() const noexcept { return buf_.data(); }
        [[nodiscard]] constexpr const_iterator cend() const noexcept { return buf_.data() + size_; }

    private:
        std::array<T, N> buf_{};
        size_type size_ = 0;
        bool overflow_ = false;
    };
} // namespace containers
