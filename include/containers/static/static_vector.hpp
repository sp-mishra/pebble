#pragma once

#include <cstddef>
#include <memory>
#include <type_traits>
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

        constexpr static_vector() noexcept = default;

        constexpr ~static_vector()
            requires (!std::is_trivially_destructible_v<T>) {
            clear();
        }

        constexpr ~static_vector()
            requires std::is_trivially_destructible_v<T> = default;

        constexpr static_vector(const static_vector& other)
            requires (!std::is_trivially_copy_constructible_v<T>) {
            for (size_type i = 0; i < other.size_; ++i) {
                static_cast<void>(push_back(other[i]));
            }
            overflow_ = other.overflow_;
        }

        constexpr static_vector(const static_vector& other)
            requires std::is_trivially_copy_constructible_v<T> = default;

        constexpr static_vector(static_vector&& other) noexcept
            requires (!std::is_trivially_move_constructible_v<T>) {
            for (size_type i = 0; i < other.size_; ++i) {
                static_cast<void>(push_back(std::move(other[i])));
            }
            overflow_ = other.overflow_;
            other.clear();
        }

        constexpr static_vector(static_vector&& other) noexcept
            requires std::is_trivially_move_constructible_v<T> = default;

        constexpr static_vector& operator=(const static_vector& other)
            requires (!std::is_trivially_copy_assignable_v<T>) {
            if (this != &other) {
                clear();
                for (size_type i = 0; i < other.size_; ++i) {
                    static_cast<void>(push_back(other[i]));
                }
                overflow_ = other.overflow_;
            }
            return *this;
        }

        constexpr static_vector& operator=(const static_vector& other)
            requires std::is_trivially_copy_assignable_v<T> = default;

        constexpr static_vector& operator=(static_vector&& other) noexcept
            requires (!std::is_trivially_move_assignable_v<T>) {
            if (this != &other) {
                clear();
                for (size_type i = 0; i < other.size_; ++i) {
                    static_cast<void>(push_back(std::move(other[i])));
                }
                overflow_ = other.overflow_;
                other.clear();
            }
            return *this;
        }

        constexpr static_vector& operator=(static_vector&& other) noexcept
            requires std::is_trivially_move_assignable_v<T> = default;

        // ---- Capacity / size ------------------------------------------------

        [[nodiscard]] constexpr size_type size() const noexcept { return size_; }
        [[nodiscard]] static constexpr size_type capacity() noexcept { return N; }
        [[nodiscard]] constexpr bool empty() const noexcept { return size_ == 0; }
        [[nodiscard]] constexpr bool overflow() const noexcept { return overflow_; }

        // ---- Modification ---------------------------------------------------

        [[nodiscard]] constexpr bool push_back(const T& v) {
            if (size_ >= N) {
                overflow_ = true;
                return false;
            }
            if consteval {
                data()[size_] = v;
            }
            else {
                std::construct_at(data() + size_, v);
            }
            ++size_;
            return true;
        }

        [[nodiscard]] constexpr bool push_back(T&& v) {
            if (size_ >= N) {
                overflow_ = true;
                return false;
            }
            if consteval {
                data()[size_] = std::move(v);
            }
            else {
                std::construct_at(data() + size_, std::move(v));
            }
            ++size_;
            return true;
        }

        template <class... Args>
        [[nodiscard]] constexpr bool emplace_back(Args&&... args) {
            if (size_ >= N) {
                overflow_ = true;
                return false;
            }
            if consteval {
                data()[size_] = T(std::forward<Args>(args)...);
            }
            else {
                std::construct_at(data() + size_, std::forward<Args>(args)...);
            }
            ++size_;
            return true;
        }

        constexpr void pop_back() noexcept {
            if (size_ > 0) {
                --size_;
                if !consteval {
                    std::destroy_at(data() + size_);
                }
            }
        }

        constexpr void clear() noexcept {
            if !consteval {
                for (size_type i = 0; i < size_; ++i) {
                    std::destroy_at(data() + i);
                }
            }
            size_ = 0;
            overflow_ = false;
        }

        // ---- Element access -------------------------------------------------

        [[nodiscard]] constexpr reference operator[](size_type i) noexcept { return *(data() + i); }
        [[nodiscard]] constexpr const_reference operator[](size_type i) const noexcept { return *(data() + i); }

        [[nodiscard]] constexpr reference back() noexcept { return *(data() + size_ - 1); }
        [[nodiscard]] constexpr const_reference back() const noexcept { return *(data() + size_ - 1); }

        [[nodiscard]] constexpr T* data() noexcept {
            if consteval {
                return storage_.elements;
            }
            else {
                return reinterpret_cast<T*>(storage_.raw);
            }
        }

        [[nodiscard]] constexpr const T* data() const noexcept {
            if consteval {
                return storage_.elements;
            }
            else {
                return reinterpret_cast<const T*>(storage_.raw);
            }
        }

        // ---- Iteration ------------------------------------------------------

        [[nodiscard]] constexpr iterator begin() noexcept { return data(); }
        [[nodiscard]] constexpr iterator end() noexcept { return data() + size_; }
        [[nodiscard]] constexpr const_iterator begin() const noexcept { return data(); }
        [[nodiscard]] constexpr const_iterator end() const noexcept { return data() + size_; }
        [[nodiscard]] constexpr const_iterator cbegin() const noexcept { return data(); }
        [[nodiscard]] constexpr const_iterator cend() const noexcept { return data() + size_; }

    private:
        union Storage {
            alignas(alignof(T)) std::byte raw[N * sizeof(T)];
            T elements[N];

            constexpr Storage() noexcept : elements{} {}

            constexpr ~Storage() noexcept
                requires (!std::is_trivially_destructible_v<T>) {}

            constexpr ~Storage() noexcept
                requires std::is_trivially_destructible_v<T> = default;

            constexpr Storage(const Storage&) noexcept
                requires std::is_trivially_copy_constructible_v<T> = default;

            constexpr Storage(const Storage& /*other*/) noexcept
                requires (!std::is_trivially_copy_constructible_v<T>) : elements{} {}

            constexpr Storage(Storage&&) noexcept
                requires std::is_trivially_move_constructible_v<T> = default;

            constexpr Storage(Storage&& /*other*/) noexcept
                requires (!std::is_trivially_move_constructible_v<T>) : elements{} {}

            constexpr Storage& operator=(const Storage&) noexcept
                requires std::is_trivially_copy_assignable_v<T> = default;

            constexpr Storage& operator=(const Storage&) noexcept
                requires (!std::is_trivially_copy_assignable_v<T>) { return *this; }

            constexpr Storage& operator=(Storage&&) noexcept
                requires std::is_trivially_move_assignable_v<T> = default;

            constexpr Storage& operator=(Storage&&) noexcept
                requires (!std::is_trivially_move_assignable_v<T>) { return *this; }
        } storage_;

        size_type size_ = 0;
        bool overflow_ = false;
    };
} // namespace containers

namespace pebble::containers {
    using ::containers::static_vector;
} // namespace pebble::containers
