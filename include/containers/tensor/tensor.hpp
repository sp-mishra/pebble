#pragma once
// ============================================================================
// Tensor.hpp — Pebble High-Performance Multidimensional Tensor Library
// ============================================================================
// C++23 / C++26, header-only, zero virtual dispatch, zero runtime overhead.
// Separation of Storage Policy (Memory) and Computation Policy (Execution).
// Expression Templates with "Deducing this" for lazy zero-allocation evaluation.
// ============================================================================

#ifndef PEBBLE_CONTAINERS_TENSOR_HPP
#define PEBBLE_CONTAINERS_TENSOR_HPP

#include <algorithm>
#include <array>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <numeric>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

// Pebble Subsystem Integrations
#include <containers/dynamic/SmallVector.hpp>
#include <mem/smriti.hpp>
#include <meta/meta.hpp>
#include <observability/nadi.hpp>

namespace ts {

    struct Greater;

    // --- Core Typedefs and Small-Buffer Optimized Dimensions ---
    // Use SmallVector<size_t, 32> for rank 1-4 shapes/strides without heap allocations
    using TensorShape = containers::dynamic::SmallVector<size_t, 32>;
    using TensorStrides = containers::dynamic::SmallVector<size_t, 32>;
    using tensor_shape = TensorShape;
    using tensor_strides = TensorStrides;

    struct all_slice_t {};
    inline constexpr all_slice_t all;
    using AllSlice = all_slice_t;

    using Slice = std::variant<size_t, std::pair<size_t, size_t>, all_slice_t>;
    using slice = Slice;

    template<class>
    inline constexpr bool always_false = false;

    // --- Forward Declarations ---
    struct DefaultStoragePolicy;
    struct DefaultComputationPolicy;

    template<typename E, typename T, typename StoragePolicy, typename CompPolicy>
    class TensorExpression;

    template<typename T, typename SP, typename CP>
    class ScalarWrapper;

    // --- Expression Storage Traits ---
    // Determines whether to store an expression operand by value or by reference
    template<typename T>
    struct expression_storage_traits {
        using type = const T&; // Default: store by reference
    };

    // Specialize for ScalarWrapper: store by value (lightweight, zero heap allocation)
    template<typename T, typename SP, typename CP>
    struct expression_storage_traits<ScalarWrapper<T, SP, CP>> {
        using type = ScalarWrapper<T, SP, CP>;
    };

    template<typename T>
    using expression_storage_t = typename expression_storage_traits<T>::type;

    template<typename T, typename StoragePolicy = DefaultStoragePolicy, typename CompPolicy = DefaultComputationPolicy>
    class DynamicTensor;

    template<typename T, typename StoragePolicy, typename CompPolicy, size_t... Dims>
    class StaticTensor;

    template<typename Op, typename Lhs, typename Rhs>
    class BinaryExpression;

    template<typename Op, typename E>
    class UnaryExpression;

    struct Add;
    struct Subtract;
    struct Multiply;
    struct Divide;
    struct Greater;
    struct Less;
    struct Equal;
    struct NotEqual;
    struct Power;
    struct AbsOp;
    struct SqrtOp;
    struct ExpOp;
    struct LogOp;
    struct SinOp;
    struct CosOp;
    struct TanOp;
    struct SquareOp;

    inline TensorShape broadcast_shapes_unif(const TensorShape &lhs, const TensorShape &rhs);

    template<typename E>
    TensorShape get_shape(
        const TensorExpression<E, typename E::value_type, typename E::storage_policy, typename E::computation_policy> &expr);

    inline size_t calculate_size_dyn(const TensorShape &shape);
    inline TensorStrides calculate_strides_dyn(const TensorShape &shape);

    // Result type trait for binary expressions
    template<typename Op, typename Lhs>
    struct binary_expr_result_type {
        using type = typename Lhs::value_type;
    };

    template<typename Lhs>
    struct binary_expr_result_type<Greater, Lhs> {
        using type = bool;
    };

    template<typename Lhs>
    struct binary_expr_result_type<Less, Lhs> {
        using type = bool;
    };

    template<typename Lhs>
    struct binary_expr_result_type<Equal, Lhs> {
        using type = bool;
    };

    template<typename Lhs>
    struct binary_expr_result_type<NotEqual, Lhs> {
        using type = bool;
    };

    // --- Core Expression Base Class (C++23 Deducing This) ---
    template<typename E, typename T, typename StoragePolicy, typename CompPolicy>
    class TensorExpression {
    public:
        using value_type = T;
        using storage_policy = StoragePolicy;
        using computation_policy = CompPolicy;

        template <typename Self>
        constexpr auto&& self(this Self&& self) noexcept {
            if constexpr (std::is_const_v<std::remove_reference_t<Self>>) {
                return static_cast<const E&>(self);
            } else {
                return static_cast<E&>(self);
            }
        }
    };

    // --- ScalarWrapper: Lightweight scalar representation ---
    template<typename T, typename SP = DefaultStoragePolicy, typename CP = DefaultComputationPolicy>
    class ScalarWrapper : public TensorExpression<ScalarWrapper<T, SP, CP>, T, SP, CP> {
    public:
        using value_type = T;
        using storage_policy = SP;
        using computation_policy = CP;

        explicit constexpr ScalarWrapper(T v) : value_(v) {}

        [[nodiscard]] TensorShape shape() const { return {}; }

        T operator()(const std::vector<size_t>&) const { return value_; }
        template<typename... Is> T operator()(Is...) const { return value_; }

        const T* data() const { return nullptr; }
        T* data() { return nullptr; }

        [[nodiscard]] constexpr T value() const { return value_; }

    private:
        T value_;
    };

    // --- Arrow-Style String Storage (Zero-copy string_view access) ---
    class ArrowStringStorage {
    public:
        ArrowStringStorage() = default;

        ArrowStringStorage(const std::vector<std::string>& strings) {
            if (offsets_.empty()) offsets_.push_back(0);
            buffer_.reserve(total_chars(strings));
            offsets_.reserve(strings.size() + 1);
            for (const auto& s : strings) {
                buffer_.insert(buffer_.end(), s.begin(), s.end());
                offsets_.push_back(buffer_.size());
            }
        }

        ArrowStringStorage(std::vector<std::string>&& strings) {
            if (offsets_.empty()) offsets_.push_back(0);
            buffer_.reserve(total_chars(strings));
            offsets_.reserve(strings.size() + 1);
            for (const auto& s : strings) {
                buffer_.insert(buffer_.end(), s.begin(), s.end());
                offsets_.push_back(buffer_.size());
            }
        }

        ArrowStringStorage(std::initializer_list<std::string> strings) {
            if (offsets_.empty()) offsets_.push_back(0);
            buffer_.reserve(total_chars(strings));
            offsets_.reserve(strings.size() + 1);
            for (const auto& s : strings) {
                buffer_.insert(buffer_.end(), s.begin(), s.end());
                offsets_.push_back(buffer_.size());
            }
        }

        void reserve(size_t n) {
            offsets_.reserve(n + 1);
            if (offsets_.empty()) offsets_.push_back(0);
        }

        void push_back(const std::string& s) {
            if (offsets_.empty()) offsets_.push_back(0);
            buffer_.insert(buffer_.end(), s.begin(), s.end());
            offsets_.push_back(buffer_.size());
        }

        void push_back(std::string_view sv) {
            if (offsets_.empty()) offsets_.push_back(0);
            buffer_.insert(buffer_.end(), sv.begin(), sv.end());
            offsets_.push_back(buffer_.size());
        }

        void push_back(const char* cstr) {
            if (!cstr) { push_back(std::string_view{}); return; }
            push_back(std::string_view(cstr));
        }

        [[nodiscard]] std::string_view operator[](size_t i) const {
            if (i >= size()) return {};
            size_t start = offsets_[i];
            size_t end = offsets_[i + 1];
            return std::string_view(buffer_.data() + start, end - start);
        }

        [[nodiscard]] std::string get(size_t i) const {
            return std::string((*this)[i]);
        }

        [[nodiscard]] size_t size() const {
            return offsets_.empty() ? 0 : offsets_.size() - 1;
        }

        [[nodiscard]] bool empty() const { return size() == 0; }

        void clear() {
            buffer_.clear();
            offsets_.clear();
        }

        class Iterator {
        public:
            using iterator_category = std::random_access_iterator_tag;
            using value_type = std::string_view;
            using difference_type = std::ptrdiff_t;
            using pointer = const std::string_view*;
            using reference = std::string_view;

            Iterator(const ArrowStringStorage* storage, size_t index)
                : storage_(storage), index_(index) {}

            reference operator*() const { return (*storage_)[index_]; }
            Iterator& operator++() { ++index_; return *this; }
            Iterator operator++(int) { Iterator tmp = *this; ++index_; return tmp; }
            bool operator==(const Iterator& other) const { return index_ == other.index_; }
            bool operator!=(const Iterator& other) const { return index_ != other.index_; }

        private:
            const ArrowStringStorage* storage_;
            size_t index_;
        };

        [[nodiscard]] Iterator begin() const { return Iterator(this, 0); }
        [[nodiscard]] Iterator end() const { return Iterator(this, size()); }

    private:
        std::vector<char> buffer_;
        std::vector<size_t> offsets_;

        static size_t total_chars(const std::vector<std::string>& strings) {
            size_t n = 0; for (const auto& s : strings) n += s.size(); return n;
        }
        static size_t total_chars(std::initializer_list<std::string> strings) {
            size_t n = 0; for (const auto& s : strings) n += s.size(); return n;
        }
    };

    // --- Policy Definitions ---
    struct DefaultStoragePolicy {
        template<typename T>
        using DynamicStorage = std::vector<T>;

        template<typename T, size_t Size>
        using StaticStorage = std::array<T, Size>;

        using StringStorage = ArrowStringStorage;
    };

    // Small-Buffer Optimized Storage Policy (e.g. 64 or 128 bytes inline buffer)
    template<size_t InlineBytes = 64>
    struct SmallTensorStoragePolicy {
        template<typename T>
        using DynamicStorage = containers::dynamic::SmallVector<T, InlineBytes>;

        template<typename T, size_t Size>
        using StaticStorage = std::array<T, Size>;

        using StringStorage = ArrowStringStorage;
    };

    // Smriti Arena / Pool Storage Policy
    template<typename ResourceT>
    struct SmritiStoragePolicy {
        template<typename T>
        using DynamicStorage = std::vector<T, smriti::SmritiAllocator<T, ResourceT>>;

        using StringStorage = ArrowStringStorage;
    };

    // Structure-of-Arrays (SoA) Storage Policy using Pebble's reflection system
    template<typename StructT, size_t InlineCapacity = 64>
        requires meta::Reflectable<StructT>
    struct SoAStoragePolicy {
        template<typename T>
        using DynamicStorage = meta::soa_storage<StructT, InlineCapacity>;

        template<typename T, size_t Size>
        using StaticStorage = meta::soa_storage<StructT, Size>;

        using StringStorage = ArrowStringStorage;
    };

    template<typename B, typename T>
    concept TensorBackend = requires(B b, const B cb, size_t i) {
        typename B::value_type;
        requires std::same_as<typename B::value_type, T>;
        { b.size() } -> std::convertible_to<size_t>;
        { b.data() } -> std::same_as<T *>;
        { cb.data() } -> std::same_as<const T *>;
        { b[i] } -> std::same_as<T &>;
        requires std::constructible_from<B, size_t>;
    };

    inline size_t calculate_size_dyn(const TensorShape &shape) {
        if (shape.empty()) return 1;
        return std::accumulate(shape.begin(), shape.end(), static_cast<size_t>(1), std::multiplies<size_t>());
    }

    inline TensorStrides calculate_strides_dyn(const TensorShape &shape) {
        if (shape.empty()) return {};
        TensorStrides strides(shape.size());
        strides.back() = 1;
        for (int i = static_cast<int>(shape.size()) - 2; i >= 0; --i) {
            strides[i] = strides[i + 1] * shape[i + 1];
        }
        return strides;
    }

    template<typename E>
    TensorShape get_shape(
        const TensorExpression<E, typename E::value_type, typename E::storage_policy, typename E::computation_policy> &expr) {
        const auto &s = expr.self().shape();
        return TensorShape(s.begin(), s.end());
    }

    // --- View Classes ---
    template<typename T>
    class TensorView : public TensorExpression<TensorView<T>, T, DefaultStoragePolicy, DefaultComputationPolicy> {
    public:
        TensorView(T *data, TensorShape shape, TensorStrides strides, const size_t offset = 0)
            : data_(data), shape_(std::move(shape)), strides_(std::move(strides)), offset_(offset) {}

        [[nodiscard]] const TensorShape &shape() const { return shape_; }

        T &operator()(const std::vector<size_t> &indices) const {
            size_t flat_index = offset_;
            for (size_t i = 0; i < indices.size(); ++i) flat_index += indices[i] * strides_[i];
            return data_[flat_index];
        }

        template<std::integral... Is>
        T &operator()(Is... idx) const {
            return (*this)({static_cast<size_t>(idx)...});
        }

        template<std::integral... Is>
        T &operator[](Is... idx) const {
            return (*this)({static_cast<size_t>(idx)...});
        }

    private:
        T *data_;
        TensorShape shape_;
        TensorStrides strides_;
        size_t offset_;
    };

    template<typename T>
    class StaticTensorView : public TensorExpression<StaticTensorView<T>, T, DefaultStoragePolicy, DefaultComputationPolicy> {
    public:
        StaticTensorView(T *data, TensorShape shape, TensorStrides strides, const size_t offset = 0)
            : data_(data), shape_(std::move(shape)), strides_(std::move(strides)), offset_(offset) {}

        [[nodiscard]] const TensorShape &shape() const { return shape_; }

        T &operator()(const std::vector<size_t> &indices) const {
            if (indices.size() != shape_.size()) throw std::out_of_range("StaticTensorView: wrong number of indices");
            size_t flat = offset_;
            for (size_t i = 0; i < shape_.size(); ++i) flat += indices[i] * strides_[i];
            return data_[flat];
        }

        template<std::integral... Is>
        T &operator()(Is... idx) const {
            return (*this)({static_cast<size_t>(idx)...});
        }

        template<std::integral... Is>
        T &operator[](Is... idx) const {
            return (*this)({static_cast<size_t>(idx)...});
        }

    private:
        T *data_;
        TensorShape shape_;
        TensorStrides strides_;
        size_t offset_;
    };

    // --- Expression Template Classes ---
    template<typename Op, typename Lhs, typename Rhs>
    class BinaryExpression : public TensorExpression<BinaryExpression<Op, Lhs, Rhs>,
                typename binary_expr_result_type<Op, Lhs>::type,
                typename Lhs::storage_policy, typename Lhs::computation_policy> {
    public:
        BinaryExpression(const Lhs &lhs, const Rhs &rhs)
            : lhs_(lhs), rhs_(rhs),
              shape_(broadcast_shapes_unif(get_shape(lhs), get_shape(rhs))) {}

        [[nodiscard]] const TensorShape &shape() const { return shape_; }

        auto operator()(const std::vector<size_t> &indices) const {
            return Op::apply(get_value(lhs_, indices), get_value(rhs_, indices));
        }

    private:
        typename expression_storage_traits<std::remove_cvref_t<Lhs>>::type lhs_;
        typename expression_storage_traits<std::remove_cvref_t<Rhs>>::type rhs_;
        TensorShape shape_;

        template<typename E>
        auto get_value(const E &expr, const std::vector<size_t> &indices) const {
            auto expr_shape = get_shape(expr);
            if (expr_shape.empty()) {
                return expr({});
            }
            if (expr_shape.size() > shape_.size()) {
                throw std::logic_error("Broadcasting error: expression has more dimensions than broadcast shape.");
            }

            std::vector<size_t> expr_indices(expr_shape.size());
            const size_t dim_diff = shape_.size() - expr_shape.size();

            for (size_t i = 0; i < expr_shape.size(); ++i) {
                size_t broadcast_idx = indices[i + dim_diff];
                expr_indices[i] = expr_shape[i] == 1 ? 0 : broadcast_idx;
            }

            return expr(expr_indices);
        }
    };

    template<typename Op, typename E>
    class UnaryExpression : public TensorExpression<UnaryExpression<Op, E>, typename E::value_type, typename
                E::storage_policy, typename E::computation_policy> {
    public:
        explicit UnaryExpression(const E &expr) : expr_(expr) {}

        const auto &shape() const { return expr_.shape(); }
        auto operator()(const std::vector<size_t> &indices) const { return Op::apply(expr_(indices)); }

    private:
        const E &expr_;
    };

    // --- Op Structs ---
    struct Add {
        static auto apply(auto a, auto b) { return a + b; }
    };

    struct Subtract {
        static auto apply(auto a, auto b) { return a - b; }
    };

    struct Multiply {
        static auto apply(auto a, auto b) { return a * b; }
    };

    struct Divide {
        static auto apply(auto a, auto b) { return a / b; }
    };

    struct Greater {
        static auto apply(auto a, auto b) { return a > b; }
    };

    struct Less {
        static auto apply(auto a, auto b) { return a < b; }
    };

    struct Equal {
        static auto apply(auto a, auto b) { return a == b; }
    };

    struct NotEqual {
        static auto apply(auto a, auto b) { return a != b; }
    };

    struct Power {
        static auto apply(auto a, auto b) { 
            using std::pow;
            return pow(a, b); 
        }
    };

    struct AbsOp {
        static auto apply(auto v) {
            using T = decltype(v);
            if constexpr (std::is_unsigned_v<T>) {
                return v;
            } else {
                using std::abs;
                return abs(v);
            }
        }
    };

    struct SqrtOp {
        static auto apply(auto v) {
            using std::sqrt;
            return sqrt(v);
        }
    };

    struct ExpOp {
        static auto apply(auto v) {
            using std::exp;
            return exp(v);
        }
    };

    struct LogOp {
        static auto apply(auto v) {
            using std::log;
            return log(v);
        }
    };

    struct SinOp {
        static auto apply(auto v) {
            using std::sin;
            return sin(v);
        }
    };

    struct CosOp {
        static auto apply(auto v) {
            using std::cos;
            return cos(v);
        }
    };

    struct TanOp {
        static auto apply(auto v) {
            using std::tan;
            return tan(v);
        }
    };

    struct SquareOp {
        static auto apply(auto v) {
            return v * v;
        }
    };

    inline TensorShape broadcast_shapes_unif(const TensorShape &lhs, const TensorShape &rhs) {
        const size_t max_dim = std::max(lhs.size(), rhs.size());
        TensorShape result_shape(max_dim);
        auto get_dim = [](const auto &shape, size_t i) {
            return i < shape.size() ? shape[shape.size() - 1 - i] : 1;
        };
        for (size_t i = 0; i < max_dim; ++i) {
            size_t lhs_dim = get_dim(lhs, i);
            size_t rhs_dim = get_dim(rhs, i);
            if (lhs_dim != rhs_dim && lhs_dim != 1 && rhs_dim != 1)
                throw std::invalid_argument("Shapes cannot be broadcasted.");
            result_shape[max_dim - 1 - i] = std::max(lhs_dim, rhs_dim);
        }
        return result_shape;
    }

    // --- Static Tensor Implementation ---
    template<typename T, typename StoragePolicy, typename CompPolicy, size_t... Dims>
    class StaticTensor : public TensorExpression<StaticTensor<T, StoragePolicy, CompPolicy, Dims...>, T, StoragePolicy,
                CompPolicy> {
    public:
        using storage_policy = StoragePolicy;
        using computation_policy = CompPolicy;
        static constexpr size_t Rank = sizeof...(Dims);
        static constexpr size_t Size = (Dims * ... * 1);
        using storage_type = typename StoragePolicy::template StaticStorage<T, Size>;

        constexpr StaticTensor() { if constexpr (Size > 0 && requires { data_.fill(T{}); }) data_.fill(T{}); }

        constexpr StaticTensor(std::initializer_list<T> list) {
            if (list.size() != Size) throw std::invalid_argument("Initializer list size does not match tensor shape.");
            if constexpr (Size > 0) std::copy(list.begin(), list.end(), data_.data());
        }

        template<typename... Vals>
            requires (sizeof...(Vals) == Size && (std::is_convertible_v<Vals, T> && ...))
        constexpr explicit StaticTensor(Vals... vals) : data_{static_cast<T>(vals)...} {}

        template<typename E>
        constexpr StaticTensor &operator=(const TensorExpression<E, T, StoragePolicy, CompPolicy> &expr) {
            const auto &expression = expr.self();
            if (auto expr_shape = get_shape(expression); !std::equal(shape().begin(), shape().end(), expr_shape.begin(), expr_shape.end()))
                throw std::runtime_error("Incompatible shape in assignment to static tensor.");

            if constexpr (Rank > 0) {
                std::vector<size_t> idx(Rank);
                for (size_t i = 0; i < Size; ++i) {
                    size_t temp_i = i;
                    for (int d = static_cast<int>(Rank) - 1; d >= 0; --d) {
                        if (shape_[d] > 0) {
                            idx[d] = temp_i % shape_[d];
                            temp_i /= shape_[d];
                        }
                    }
                    data_[i] = expression(idx);
                }
            }
            return *this;
        }

        constexpr const storage_type &storage() const { return data_; }
        constexpr T *data() { 
            if constexpr (std::is_same_v<storage_type, std::vector<bool>>) {
                return nullptr;
            } else {
                return data_.data(); 
            }
        }
        constexpr const T *data() const { 
            if constexpr (std::is_same_v<storage_type, std::vector<bool>>) {
                return nullptr;
            } else {
                return data_.data(); 
            }
        }
        constexpr const std::array<size_t, Rank> &shape() const { return shape_; }

        constexpr T &operator()(const std::vector<size_t> &indices) { return data_[get_flat_index_dyn(indices)]; }
        constexpr const T &operator()(const std::vector<size_t> &indices) const { return data_[get_flat_index_dyn(indices)]; }

        template<std::integral... Is>
        constexpr T &operator()(Is... indices) {
            static_assert(sizeof...(Is) == Rank, "Incorrect number of indices.");
            return data_[get_flat_index({static_cast<size_t>(indices)...})];
        }

        template<std::integral... Is>
        constexpr const T &operator()(Is... indices) const {
            static_assert(sizeof...(Is) == Rank, "Incorrect number of indices.");
            return data_[get_flat_index({static_cast<size_t>(indices)...})];
        }

        // C++23 multidimensional subscript operator
        template<std::integral... Is>
        constexpr T &operator[](Is... indices) {
            static_assert(sizeof...(Is) == Rank, "Incorrect number of indices.");
            return data_[get_flat_index({static_cast<size_t>(indices)...})];
        }

        template<std::integral... Is>
        constexpr const T &operator[](Is... indices) const {
            static_assert(sizeof...(Is) == Rank, "Incorrect number of indices.");
            return data_[get_flat_index({static_cast<size_t>(indices)...})];
        }

    private:
        storage_type data_;
        static constexpr std::array<size_t, Rank> shape_ = {Dims...};
        static constexpr std::array<size_t, Rank> strides_ = [] {
            std::array<size_t, Rank> s{};
            if constexpr (Rank > 0) {
                s[Rank - 1] = 1;
                for (int i = static_cast<int>(Rank) - 2; i >= 0; --i) s[i] = s[i + 1] * shape_[i + 1];
            }
            return s;
        }();

        constexpr size_t get_flat_index(const std::array<size_t, Rank> &indices) const {
            size_t flat_index = 0;
            for (size_t i = 0; i < Rank; ++i) flat_index += indices[i] * strides_[i];
            return flat_index;
        }

        [[nodiscard]] size_t get_flat_index_dyn(const std::vector<size_t> &indices) const {
            size_t flat_index = 0;
            for (size_t i = 0; i < Rank; ++i) flat_index += indices[i] * strides_[i];
            return flat_index;
        }
    };

    // --- Dynamic Tensor Implementation ---
    template<typename T, typename StoragePolicy, typename CompPolicy>
    class DynamicTensor : public TensorExpression<DynamicTensor<T, StoragePolicy, CompPolicy>, T, StoragePolicy,
                CompPolicy> {
    public:
        using storage_policy = StoragePolicy;
        using computation_policy = CompPolicy;
        using storage_type = typename StoragePolicy::template DynamicStorage<T>;

        DynamicTensor(const std::initializer_list<size_t> shape)
            : shape_(shape),
              strides_(calculate_strides_dyn(shape_)),
              data_(calculate_size_dyn(shape_)) {}

        DynamicTensor(TensorShape shape = {})
            : shape_(std::move(shape)),
              strides_(calculate_strides_dyn(shape_)),
              data_(calculate_size_dyn(shape_)) {}

        template<typename ResourceOrAlloc>
            requires (requires(ResourceOrAlloc& r, size_t n) { storage_type(n, smriti::SmritiAllocator<T, ResourceOrAlloc>{r}); } ||
                      requires(ResourceOrAlloc& a, size_t n) { storage_type(n, a); })
        DynamicTensor(TensorShape shape, ResourceOrAlloc& res)
            : shape_(std::move(shape)),
              strides_(calculate_strides_dyn(shape_)),
              data_([&]() {
                  const size_t n = calculate_size_dyn(shape_);
                  if constexpr (requires { storage_type(n, smriti::SmritiAllocator<T, ResourceOrAlloc>{res}); }) {
                      return storage_type(n, smriti::SmritiAllocator<T, ResourceOrAlloc>{res});
                  } else {
                      return storage_type(n, res);
                  }
              }()) {}

        DynamicTensor(const DynamicTensor&) = default;
        DynamicTensor(DynamicTensor&&) noexcept = default;
        DynamicTensor& operator=(const DynamicTensor&) = default;
        DynamicTensor& operator=(DynamicTensor&&) noexcept = default;

        template<typename InputIt>
        DynamicTensor(TensorShape shape, InputIt first, InputIt last)
            : shape_(std::move(shape)),
              strides_(calculate_strides_dyn(shape_)),
              data_(first, last) {
            if (data_.size() != calculate_size_dyn(shape_)) {
                throw std::invalid_argument("Iterator range size does not match tensor shape.");
            }
        }

        template<typename OtherStorage, typename OtherComp>
        explicit DynamicTensor(const DynamicTensor<T, OtherStorage, OtherComp> &other)
            : shape_(other.shape()), strides_(calculate_strides_dyn(other.shape())) {
            if constexpr (std::is_same_v<T, bool> && requires { other.data(); } &&
                          std::is_same_v<typename OtherStorage::template DynamicStorage<T>, std::vector<bool>>) {
                const size_t n = calculate_size_dyn(other.shape());
                const auto &other_storage = other.storage();
                std::vector<bool> temp;
                temp.reserve(n);
                for (size_t i = 0; i < n; ++i) {
                    temp.push_back(static_cast<bool>(other_storage[i]));
                }
                data_ = storage_type(temp.begin(), temp.end());
            } else if (other.data() != nullptr) {
                data_ = storage_type(other.data(), other.data() + calculate_size_dyn(other.shape()));
            } else {
                data_ = storage_type(calculate_size_dyn(other.shape()));
                std::vector<size_t> idx(shape_.size(), 0);
                for (size_t i = 0; i < calculate_size_dyn(shape_); ++i) {
                    size_t temp_i = i;
                    for (int d = static_cast<int>(shape_.size()) - 1; d >= 0; --d) {
                        if (shape_[d] > 0) {
                            idx[d] = temp_i % shape_[d];
                            temp_i /= shape_[d];
                        }
                    }
                    data_[i] = other(idx);
                }
            }
        }

        DynamicTensor(TensorShape shape, std::initializer_list<T> list)
            : shape_(std::move(shape)),
              strides_(calculate_strides_dyn(shape_)),
              data_(list.begin(), list.end()) {
            if (data_.size() != calculate_size_dyn(shape_)) {
                throw std::invalid_argument("Initializer list size does not match tensor shape.");
            }
        }

        DynamicTensor(TensorShape shape, storage_type storage)
            : shape_(std::move(shape)),
              strides_(calculate_strides_dyn(shape_)),
              data_(std::move(storage)) {}

        template<typename E>
        explicit DynamicTensor(const TensorExpression<E, T, StoragePolicy, CompPolicy> &expr) {
            const auto &expression = expr.self();
            shape_ = get_shape(expression);
            strides_ = calculate_strides_dyn(shape_);

            if (shape_.empty()) {
                data_ = storage_type(1);
                if (data_.size() > 0) {
                    data_[0] = expression({});
                }
                return;
            }

            data_ = storage_type(calculate_size_dyn(shape_));
            if (data_.size() == 0) return;

            std::vector<size_t> idx(shape_.size(), 0);
            for (size_t i = 0; i < calculate_size_dyn(shape_); ++i) {
                size_t temp_i = i;
                for (int d = static_cast<int>(shape_.size()) - 1; d >= 0; --d) {
                    if (shape_[d] > 0) {
                        idx[d] = temp_i % shape_[d];
                        temp_i /= shape_[d];
                    }
                }
                if constexpr (requires(storage_type &s, size_t j, T v) { s.set(j, v); }) {
                    data_.set(i, expression(idx));
                } else if constexpr (std::is_same_v<T, bool> && requires { data_.get(); }) {
                    if (data_.data() == nullptr) {
                        std::vector<bool> temp_buf;
                        temp_buf.reserve(calculate_size_dyn(shape_));
                        std::vector<size_t> idx2(shape_.size(), 0);
                        for(size_t k = 0; k < calculate_size_dyn(shape_); ++k) {
                            size_t temp_k = k;
                            for (int d = static_cast<int>(shape_.size()) - 1; d >= 0; --d) {
                                if (shape_[d] > 0) {
                                    idx2[d] = temp_k % shape_[d];
                                    temp_k /= shape_[d];
                                }
                            }
                            temp_buf.push_back(expression(idx2));
                        }
                        data_ = storage_type(temp_buf.begin(), temp_buf.end());
                        return;
                    } else {
                        throw std::runtime_error("Direct indexing not supported for MLX bool arrays");
                    }
                } else {
                    data_[i] = expression(idx);
                }
            }
        }

        template<typename E>
        DynamicTensor &operator=(const TensorExpression<E, T, StoragePolicy, CompPolicy> &expr) {
            const auto &expression = expr.self();
            shape_ = get_shape(expression);
            strides_ = calculate_strides_dyn(shape_);
            data_ = storage_type(calculate_size_dyn(shape_));
            if (data_.size() == 0) return *this;
            std::vector<size_t> idx(shape_.size());
            for (size_t i = 0; i < calculate_size_dyn(shape_); ++i) {
                size_t temp_i = i;
                for (int d = static_cast<int>(idx.size()) - 1; d >= 0; --d) {
                    if (shape_[d] > 0) {
                        idx[d] = temp_i % shape_[d];
                        temp_i /= shape_[d];
                    }
                }
                if constexpr (requires(storage_type &s, size_t j, T v) { s.set(j, v); }) {
                    data_.set(i, expression(idx));
                } else if constexpr (std::is_same_v<T, bool> && requires { data_.get(); }) {
                    if (data_.data() == nullptr) {
                        std::vector<bool> temp_buf;
                        temp_buf.reserve(calculate_size_dyn(shape_));
                        for(size_t k = 0; k < calculate_size_dyn(shape_); ++k) {
                            size_t temp_k = k;
                            for (int d = static_cast<int>(idx.size()) - 1; d >= 0; --d) {
                                if (shape_[d] > 0) {
                                    idx[d] = temp_k % shape_[d];
                                    temp_k /= shape_[d];
                                }
                            }
                            temp_buf.push_back(expression(idx));
                        }
                        data_ = storage_type(temp_buf.begin(), temp_buf.end());
                        return *this;
                    } else {
                        throw std::runtime_error("Direct indexing not supported for MLX bool arrays");
                    }
                } else {
                    data_[i] = expression(idx);
                }
            }
            return *this;
        }

        const storage_type &storage() const { return data_; }
        T *data() { 
            if constexpr (std::is_same_v<storage_type, std::vector<bool>>) {
                return nullptr;
            } else {
                return data_.data(); 
            }
        }
        const T *data() const { 
            if constexpr (std::is_same_v<storage_type, std::vector<bool>>) {
                return nullptr;
            } else {
                return data_.data(); 
            }
        }
        [[nodiscard]] const TensorShape &shape() const { return shape_; }

        decltype(auto) operator()(const std::vector<size_t> &indices) {
            const size_t flat = get_flat_index_dyn(indices);
            if constexpr (std::is_same_v<T, bool> && requires(const storage_type &s, size_t i) { s.get_value(i); }) {
                return data_.get_value(flat);
            }
            return data_[flat];
        }
        T operator()(const std::vector<size_t> &indices) const { return data_[get_flat_index_dyn(indices)]; }

        template<std::integral... Is>
        decltype(auto) operator()(Is... indices) {
            const size_t flat = get_flat_index_dyn({static_cast<size_t>(indices)...});
            if constexpr (std::is_same_v<T, bool> && requires(const storage_type &s, size_t i) { s.get_value(i); }) {
                return data_.get_value(flat);
            }
            return data_[flat];
        }

        template<std::integral... Is>
        T operator()(Is... indices) const {
            return data_[get_flat_index_dyn({static_cast<size_t>(indices)...})];
        }

        // C++23 multidimensional subscript operator
        template<std::integral... Is>
        decltype(auto) operator[](Is... indices) {
            const size_t flat = get_flat_index_dyn({static_cast<size_t>(indices)...});
            if constexpr (std::is_same_v<T, bool> && requires(const storage_type &s, size_t i) { s.get_value(i); }) {
                return data_.get_value(flat);
            }
            return data_[flat];
        }

        template<std::integral... Is>
        T operator[](Is... indices) const {
            return data_[get_flat_index_dyn({static_cast<size_t>(indices)...})];
        }

        [[nodiscard]] size_t size() const {
            return calculate_size_dyn(shape_);
        }

        template<typename... Slices>
        auto operator()(const Slices &... slices) const
            requires (sizeof...(Slices) > 0 && (std::is_convertible_v<Slices, Slice> && ...)) {
            if (sizeof...(Slices) != shape_.size())
                throw std::invalid_argument("Number of slices must match tensor rank.");
            std::array<Slice, sizeof...(Slices)> slice_arr{Slice(slices)...};

            TensorShape view_shape;
            TensorStrides view_strides;
            size_t offset = 0;

            for (size_t i = 0; i < slice_arr.size(); ++i) {
                if (const auto &s = slice_arr[i]; std::holds_alternative<size_t>(s)) {
                    const size_t idx = std::get<size_t>(s);
                    if (idx >= shape_[i]) throw std::out_of_range("Slice index out of bounds");
                    offset += idx * strides_[i];
                } else if (std::holds_alternative<std::pair<size_t, size_t>>(s)) {
                    auto [start, stop] = std::get<std::pair<size_t, size_t>>(s);
                    if (start > stop || stop > shape_[i]) throw std::out_of_range("Slice range out of bounds");
                    view_shape.push_back(stop - start);
                    view_strides.push_back(strides_[i]);
                    offset += start * strides_[i];
                } else if (std::holds_alternative<AllSlice>(s)) {
                    view_shape.push_back(shape_[i]);
                    view_strides.push_back(strides_[i]);
                }
            }

            return TensorView<T>(const_cast<T *>(data()), view_shape, view_strides, offset);
        }

    private:
        TensorShape shape_;
        TensorStrides strides_;
        storage_type data_;

        [[nodiscard]] size_t get_flat_index_dyn(const std::vector<size_t> &indices) const {
            if (indices.size() > shape_.size())
                throw std::out_of_range("Incorrect number of indices for DynamicTensor access.");

            std::vector<size_t> full(shape_.size(), 0);
            const size_t dim_diff = shape_.size() - indices.size();
            for (size_t i = 0; i < indices.size(); ++i) {
                full[i + dim_diff] = indices[i];
            }

            size_t flat_index = 0;
            for (size_t i = 0; i < shape_.size(); ++i) flat_index += full[i] * strides_[i];
            return flat_index;
        }
    };

    // --- Default Computation Policy ---
    struct DefaultComputationPolicy {
    private:
        template<typename T, typename SP, typename CP, typename E1, typename E2>
        static auto vv_dot(const E1 &A, const E2 &B, const TensorShape &ashape, const TensorShape &bshape) {
            if (ashape[0] != bshape[0]) throw std::invalid_argument("dot: Vectors must have the same length");

            T sum_val = T{0};
            for (size_t k = 0; k < ashape[0]; ++k) {
                sum_val += A({k}) * B({k});
            }
            return DynamicTensor<T, SP, CP>({}, {sum_val});
        }

        template<typename T, typename SP, typename CP, typename E1, typename E2>
        static auto mv_dot(const E1 &A, const E2 &B, const TensorShape &ashape, const TensorShape &bshape) {
            if (ashape[1] != bshape[0]) throw std::invalid_argument("dot: Inner dimensions must match");

            DynamicTensor<T, SP, CP> result({ashape[0]});
            for (size_t i = 0; i < ashape[0]; ++i) {
                T sum_val = T{0};
                for (size_t k = 0; k < ashape[1]; ++k) {
                    sum_val += A({i, k}) * B({k});
                }
                result({i}) = sum_val;
            }
            return result;
        }

        template<typename T, typename SP, typename CP, typename E1, typename E2>
        static auto mm_dot(const E1 &A, const E2 &B, const TensorShape &ashape, const TensorShape &bshape) {
            if (ashape[1] != bshape[0]) throw std::invalid_argument("dot: Inner dimensions must match");

            DynamicTensor<T, SP, CP> result({ashape[0], bshape[1]});
            const T *a_ptr = A.data();
            const T *b_ptr = B.data();
            T *r_ptr = result.data();
            const size_t M = ashape[0];
            const size_t K = ashape[1];
            const size_t N = bshape[1];

            if (a_ptr && b_ptr && r_ptr) {
                for (size_t i = 0; i < M; ++i) {
                    for (size_t j = 0; j < N; ++j) {
                        T sum_val = T{0};
                        for (size_t k = 0; k < K; ++k) {
                            sum_val += a_ptr[i * K + k] * b_ptr[k * N + j];
                        }
                        r_ptr[i * N + j] = sum_val;
                    }
                }
            } else {
                for (size_t i = 0; i < M; ++i) {
                    for (size_t j = 0; j < N; ++j) {
                        T sum_val = T{0};
                        for (size_t k = 0; k < K; ++k) {
                            sum_val += A({i, k}) * B({k, j});
                        }
                        result({i, j}) = sum_val;
                    }
                }
            }
            return result;
        }

        template<typename T, typename SP, typename CP, typename E1, typename E2>
        static auto generic_dot(const E1 &A, const E2 &B, const TensorShape &ashape, const TensorShape &bshape) {
            if (ashape.size() == 1 && bshape.size() == 1) {
                return vv_dot<T, SP, CP>(A, B, ashape, bshape);
            }
            if (ashape.size() == 2 && bshape.size() == 1) {
                return mv_dot<T, SP, CP>(A, B, ashape, bshape);
            }
            if (ashape.size() == 2 && bshape.size() == 2) {
                return mm_dot<T, SP, CP>(A, B, ashape, bshape);
            }
            throw std::invalid_argument("Unsupported tensor ranks for dot product. Supported are (1,1), (2,1), (2,2).");
        }

    public:
        template<typename E1, typename E2>
        static auto add(const E1 &a, const E2 &b) { return BinaryExpression<Add, E1, E2>(a, b); }

        template<typename E1, typename E2>
        static auto subtract(const E1 &a, const E2 &b) { return BinaryExpression<Subtract, E1, E2>(a, b); }

        template<typename E1, typename E2>
        static auto multiply(const E1 &a, const E2 &b) { return BinaryExpression<Multiply, E1, E2>(a, b); }

        template<typename E1, typename E2>
        static auto divide(const E1 &a, const E2 &b) { return BinaryExpression<Divide, E1, E2>(a, b); }

        template<typename E>
        static auto abs(const E &e) { return UnaryExpression<AbsOp, E>(e); }

        template<typename E>
        static auto sqrt(const E &e) { return UnaryExpression<SqrtOp, E>(e); }

        template<typename E>
        static auto exp(const E &e) { return UnaryExpression<ExpOp, E>(e); }

        template<typename E>
        static auto log(const E &e) { return UnaryExpression<LogOp, E>(e); }

        template<typename E>
        static auto sum(const E &expr) {
            using T = typename E::value_type;
            const auto &tensor = expr.self();
            auto dyn_shape = get_shape(tensor);
            const size_t N = calculate_size_dyn(dyn_shape);
            T total = T{0};
            if (N == 0) return total;
            std::vector<size_t> idx(dyn_shape.size(), 0);
            for (size_t i = 0; i < N; ++i) {
                size_t temp_i = i;
                for (int d = static_cast<int>(idx.size()) - 1; d >= 0; --d) {
                    if (dyn_shape[d] > 0) {
                        idx[d] = temp_i % dyn_shape[d];
                        temp_i /= dyn_shape[d];
                    }
                }
                total += tensor(idx);
            }
            return total;
        }

        template<typename E>
        static auto mean(const E &expr) {
            const auto &tensor = expr.self();
            auto dyn_shape = get_shape(tensor);
            const size_t size = calculate_size_dyn(dyn_shape);
            return size > 0 ? DefaultComputationPolicy::sum(expr) / static_cast<double>(size) : 0.0;
        }

        template<typename E>
        static auto max(const E &expr) {
            using T = typename E::value_type;
            const auto &tensor = expr.self();
            auto dyn_shape = get_shape(tensor);
            const size_t N = calculate_size_dyn(dyn_shape);
            if (N == 0) throw std::runtime_error("max() on empty tensor not supported");
            T result = tensor(std::vector<size_t>(dyn_shape.size(), 0));
            std::vector<size_t> idx(dyn_shape.size(), 0);
            for (size_t i = 1; i < N; ++i) {
                size_t temp_i = i;
                for (int d = static_cast<int>(idx.size()) - 1; d >= 0; --d) {
                    if (dyn_shape[d] > 0) {
                        idx[d] = temp_i % dyn_shape[d];
                        temp_i /= dyn_shape[d];
                    }
                }
                result = std::max(result, tensor(idx));
            }
            return result;
        }

        template<typename E1, typename E2>
        static auto dot(const E1 &a, const E2 &b) {
            const auto &A = a.self();
            const auto &B = b.self();
            auto ashape = get_shape(A);
            auto bshape = get_shape(B);
            using T = typename E1::value_type;
            using StoragePolicy = typename E1::storage_policy;
            using CompPolicy = typename E1::computation_policy;

            return generic_dot<T, StoragePolicy, CompPolicy>(A, B, ashape, bshape);
        }

        template<typename E1, typename E2>
        static auto greater(const E1 &a, const E2 &b) {
            return BinaryExpression<Greater, E1, E2>(a, b);
        }

        template<typename E>
        static auto min(const E &expr) {
            using T = typename E::value_type;
            const auto &tensor = expr.self();
            auto dyn_shape = get_shape(tensor);
            const size_t N = calculate_size_dyn(dyn_shape);
            if (N == 0) throw std::runtime_error("min() on empty tensor not supported");
            T result = tensor(std::vector<size_t>(dyn_shape.size(), 0));
            std::vector<size_t> idx(dyn_shape.size(), 0);
            for (size_t i = 1; i < N; ++i) {
                size_t temp_i = i;
                for (int d = static_cast<int>(idx.size()) - 1; d >= 0; --d) {
                    if (dyn_shape[d] > 0) {
                        idx[d] = temp_i % dyn_shape[d];
                        temp_i /= dyn_shape[d];
                    }
                }
                result = std::min(result, tensor(idx));
            }
            return result;
        }

        template<typename E>
        static auto variance(const E &expr) {
            const auto &tensor = expr.self();
            auto dyn_shape = get_shape(tensor);
            const size_t size = calculate_size_dyn(dyn_shape);
            if (size <= 1) return 0.0;
            
            auto mean_val = DefaultComputationPolicy::mean(expr);
            double var_sum = 0.0;
            std::vector<size_t> idx(dyn_shape.size(), 0);
            for (size_t i = 0; i < size; ++i) {
                size_t temp_i = i;
                for (int d = static_cast<int>(idx.size()) - 1; d >= 0; --d) {
                    if (dyn_shape[d] > 0) {
                        idx[d] = temp_i % dyn_shape[d];
                        temp_i /= dyn_shape[d];
                    }
                }
                double diff = static_cast<double>(tensor(idx)) - mean_val;
                var_sum += diff * diff;
            }
            return var_sum / static_cast<double>(size - 1);
        }

        template<typename E>
        static auto std_dev(const E &expr) {
            return std::sqrt(DefaultComputationPolicy::variance(expr));
        }

        template<typename E>
        static auto normalize(const E &expr) {
            using T = typename E::value_type;
            using StoragePolicy = typename E::storage_policy;
            using CompPolicy = typename E::computation_policy;
            
            const auto &tensor = expr.self();
            auto shape = get_shape(tensor);
            auto mean_val = DefaultComputationPolicy::mean(expr);
            auto std_val = DefaultComputationPolicy::std_dev(expr);
            
            if (std_val == 0.0) {
                return DynamicTensor<T, StoragePolicy, CompPolicy>(shape, std::vector<T>(calculate_size_dyn(shape), T{0}));
            }
            
            DynamicTensor<T, StoragePolicy, CompPolicy> result(shape);
            std::vector<size_t> idx(shape.size(), 0);
            const size_t size = calculate_size_dyn(shape);
            
            for (size_t i = 0; i < size; ++i) {
                size_t temp_i = i;
                for (int d = static_cast<int>(idx.size()) - 1; d >= 0; --d) {
                    if (shape[d] > 0) {
                        idx[d] = temp_i % shape[d];
                        temp_i /= shape[d];
                    }
                }
                result.data()[i] = static_cast<T>((static_cast<double>(tensor(idx)) - mean_val) / std_val);
            }
            return result;
        }

        template<typename E>
        static auto reshape(const E &expr, const TensorShape &new_shape) {
            using T = typename E::value_type;
            using StoragePolicy = typename E::storage_policy;
            using CompPolicy = typename E::computation_policy;
            
            const auto &tensor = expr.self();
            auto old_shape = get_shape(tensor);
            auto old_size = calculate_size_dyn(old_shape);
            auto new_size = calculate_size_dyn(new_shape);
            
            if (old_size != new_size) {
                throw std::invalid_argument("reshape: new shape must have same number of elements");
            }
            
            DynamicTensor<T, StoragePolicy, CompPolicy> result(new_shape);
            
            if (const T* data_ptr = tensor.data(); data_ptr != nullptr) {
                std::copy(data_ptr, data_ptr + old_size, result.data());
            } else {
                std::vector<size_t> idx(old_shape.size(), 0);
                for (size_t i = 0; i < old_size; ++i) {
                    size_t temp_i = i;
                    for (int d = static_cast<int>(idx.size()) - 1; d >= 0; --d) {
                        if (old_shape[d] > 0) {
                            idx[d] = temp_i % old_shape[d];
                            temp_i /= old_shape[d];
                        }
                    }
                    result.data()[i] = tensor(idx);
                }
            }
            return result;
        }

        template<typename E>
        static auto flatten(const E &expr) {
            auto shape = get_shape(expr.self());
            auto size = calculate_size_dyn(shape);
            return DefaultComputationPolicy::reshape(expr, TensorShape{size});
        }

        template<typename E>
        static auto transpose(const E &expr) {
            using T = typename E::value_type;
            using StoragePolicy = typename E::storage_policy;
            using CompPolicy = typename E::computation_policy;
            
            const auto &tensor = expr.self();
            auto shape = get_shape(tensor);
            
            if (shape.size() != 2) {
                throw std::invalid_argument("transpose: only 2D tensors are supported");
            }
            
            TensorShape new_shape = {shape[1], shape[0]};
            DynamicTensor<T, StoragePolicy, CompPolicy> result(new_shape);
            
            for (size_t i = 0; i < shape[0]; ++i) {
                for (size_t j = 0; j < shape[1]; ++j) {
                    result({j, i}) = tensor({i, j});
                }
            }
            return result;
        }

        template<typename E1, typename E2>
        static auto less(const E1 &a, const E2 &b) { return BinaryExpression<Less, E1, E2>(a, b); }

        template<typename E1, typename E2>
        static auto equal(const E1 &a, const E2 &b) { return BinaryExpression<Equal, E1, E2>(a, b); }

        template<typename E1, typename E2>
        static auto not_equal(const E1 &a, const E2 &b) { return BinaryExpression<NotEqual, E1, E2>(a, b); }

        template<typename E>
        static auto sin(const E &e) { return UnaryExpression<SinOp, E>(e); }

        template<typename E>
        static auto cos(const E &e) { return UnaryExpression<CosOp, E>(e); }

        template<typename E>
        static auto tan(const E &e) { return UnaryExpression<TanOp, E>(e); }

        template<typename E>
        static auto square(const E &e) { return UnaryExpression<SquareOp, E>(e); }

        template<typename E1, typename E2>
        static auto power(const E1 &a, const E2 &b) { return BinaryExpression<Power, E1, E2>(a, b); }

        template<typename E, typename T>
        static auto clip(const E &expr, T min_val, T max_val) {
            using StoragePolicy = typename E::storage_policy;
            using CompPolicy = typename E::computation_policy;
            
            const auto &tensor = expr.self();
            auto shape = get_shape(tensor);
            DynamicTensor<T, StoragePolicy, CompPolicy> result(shape);
            
            std::vector<size_t> idx(shape.size(), 0);
            const size_t size = calculate_size_dyn(shape);
            
            for (size_t i = 0; i < size; ++i) {
                size_t temp_i = i;
                for (int d = static_cast<int>(idx.size()) - 1; d >= 0; --d) {
                    if (shape[d] > 0) {
                        idx[d] = temp_i % shape[d];
                        temp_i /= shape[d];
                    }
                }
                T val = tensor(idx);
                result.data()[i] = std::clamp(val, min_val, max_val);
            }
            return result;
        }
    };

    // --- Scalar-Tensor Dispatchers ---
    template<typename E, typename T, typename SP, typename CP, typename S>
        requires std::is_arithmetic_v<S>
    auto operator+(const TensorExpression<E, T, SP, CP> &expr, const S &scalar) {
        auto rhs = ScalarWrapper<std::decay_t<S>, SP, CP>(scalar);
        auto lazy_expr = CP::add(expr.self(), rhs);
        return DynamicTensor<T, SP, CP>(lazy_expr);
    }

    template<typename S, typename E, typename T, typename SP, typename CP>
        requires std::is_arithmetic_v<S>
    auto operator+(const S &scalar, const TensorExpression<E, T, SP, CP> &expr) {
        auto lhs = ScalarWrapper<std::decay_t<S>, SP, CP>(scalar);
        auto lazy_expr = CP::add(lhs, expr.self());
        return DynamicTensor<T, SP, CP>(lazy_expr);
    }

    template<typename E, typename T, typename SP, typename CP, typename S>
        requires std::is_arithmetic_v<S>
    auto operator-(const TensorExpression<E, T, SP, CP> &expr, const S &scalar) {
        auto rhs = ScalarWrapper<std::decay_t<S>, SP, CP>(scalar);
        auto lazy_expr = CP::subtract(expr.self(), rhs);
        return DynamicTensor<T, SP, CP>(lazy_expr);
    }

    template<typename S, typename E, typename T, typename SP, typename CP>
        requires std::is_arithmetic_v<S>
    auto operator-(const S &scalar, const TensorExpression<E, T, SP, CP> &expr) {
        auto lhs = ScalarWrapper<std::decay_t<S>, SP, CP>(scalar);
        auto lazy_expr = CP::subtract(lhs, expr.self());
        return DynamicTensor<T, SP, CP>(lazy_expr);
    }

    template<typename E, typename T, typename SP, typename CP, typename S>
        requires std::is_arithmetic_v<S>
    auto operator*(const TensorExpression<E, T, SP, CP> &expr, const S &scalar) {
        auto rhs = ScalarWrapper<std::decay_t<S>, SP, CP>(scalar);
        auto lazy_expr = CP::multiply(expr.self(), rhs);
        return DynamicTensor<T, SP, CP>(lazy_expr);
    }

    template<typename S, typename E, typename T, typename SP, typename CP>
        requires std::is_arithmetic_v<S>
    auto operator*(const S &scalar, const TensorExpression<E, T, SP, CP> &expr) {
        auto lhs = ScalarWrapper<std::decay_t<S>, SP, CP>(scalar);
        auto lazy_expr = CP::multiply(lhs, expr.self());
        return DynamicTensor<T, SP, CP>(lazy_expr);
    }

    template<typename E, typename T, typename SP, typename CP, typename S>
        requires std::is_arithmetic_v<S>
    auto operator/(const TensorExpression<E, T, SP, CP> &expr, const S &scalar) {
        auto rhs = ScalarWrapper<std::decay_t<S>, SP, CP>(scalar);
        auto lazy_expr = CP::divide(expr.self(), rhs);
        return DynamicTensor<T, SP, CP>(lazy_expr);
    }

    template<typename S, typename E, typename T, typename SP, typename CP>
        requires std::is_arithmetic_v<S>
    auto operator/(const S &scalar, const TensorExpression<E, T, SP, CP> &expr) {
        auto lhs = ScalarWrapper<std::decay_t<S>, SP, CP>(scalar);
        auto lazy_expr = CP::divide(lhs, expr.self());
        return DynamicTensor<T, SP, CP>(lazy_expr);
    }

    template<typename E, typename T, typename SP, typename CP, typename S>
        requires std::is_arithmetic_v<S>
    auto operator>(const TensorExpression<E, T, SP, CP> &expr, const S &scalar) {
        auto rhs = ScalarWrapper<std::decay_t<S>, SP, CP>(scalar);
        auto lazy_expr = CP::greater(expr.self(), rhs);
        return DynamicTensor<bool, SP, CP>(lazy_expr);
    }

    template<typename S, typename E, typename T, typename SP, typename CP>
        requires std::is_arithmetic_v<S>
    auto operator>(const S &scalar, const TensorExpression<E, T, SP, CP> &expr) {
        auto lhs = ScalarWrapper<std::decay_t<S>, SP, CP>(scalar);
        auto lazy_expr = CP::greater(lhs, expr.self());
        return DynamicTensor<bool, SP, CP>(lazy_expr);
    }

    // --- Tensor-Tensor Binary Operators ---
    template<typename E1, typename T1, typename SP1, typename CP1,
        typename E2, typename T2, typename SP2, typename CP2>
    auto operator+(const TensorExpression<E1, T1, SP1, CP1> &lhs,
                   const TensorExpression<E2, T2, SP2, CP2> &rhs) {
        static_assert(std::is_same_v<CP1, CP2>, "Computation policies must match.");
        auto lazy_expr = CP1::add(lhs.self(), rhs.self());
        return DynamicTensor<T1, SP1, CP1>(lazy_expr);
    }

    template<typename E1, typename T1, typename SP1, typename CP1,
        typename E2, typename T2, typename SP2, typename CP2>
    auto operator-(const TensorExpression<E1, T1, SP1, CP1> &lhs,
                   const TensorExpression<E2, T2, SP2, CP2> &rhs) {
        static_assert(std::is_same_v<CP1, CP2>, "Computation policies must match.");
        auto lazy_expr = CP1::subtract(lhs.self(), rhs.self());
        return DynamicTensor<T1, SP1, CP1>(lazy_expr);
    }

    template<typename E1, typename T1, typename SP1, typename CP1,
        typename E2, typename T2, typename SP2, typename CP2>
    auto operator*(const TensorExpression<E1, T1, SP1, CP1> &lhs,
                   const TensorExpression<E2, T2, SP2, CP2> &rhs) {
        static_assert(std::is_same_v<CP1, CP2>, "Computation policies must match.");
        auto lazy_expr = CP1::multiply(lhs.self(), rhs.self());
        return DynamicTensor<T1, SP1, CP1>(lazy_expr);
    }

    template<typename E1, typename T1, typename SP1, typename CP1,
        typename E2, typename T2, typename SP2, typename CP2>
    auto operator/(const TensorExpression<E1, T1, SP1, CP1> &lhs,
                   const TensorExpression<E2, T2, SP2, CP2> &rhs) {
        static_assert(std::is_same_v<CP1, CP2>, "Computation policies must match.");
        auto lazy_expr = CP1::divide(lhs.self(), rhs.self());
        return DynamicTensor<T1, SP1, CP1>(lazy_expr);
    }

    template<typename E1, typename T1, typename SP1, typename CP1,
        typename E2, typename T2, typename SP2, typename CP2>
    auto operator>(const TensorExpression<E1, T1, SP1, CP1> &lhs,
                   const TensorExpression<E2, T2, SP2, CP2> &rhs) {
        static_assert(std::is_same_v<CP1, CP2>, "Computation policies must match.");
        auto lazy_expr = CP1::greater(lhs.self(), rhs.self());
        return DynamicTensor<bool, SP1, CP1>(lazy_expr);
    }

    // --- Global Tensor Functions ---
    template<typename E, typename T, typename SP, typename CP>
    auto sum(const TensorExpression<E, T, SP, CP> &expr) {
        return CP::sum(expr.self());
    }

    template<typename E, typename T, typename SP, typename CP>
    auto mean(const TensorExpression<E, T, SP, CP> &expr) {
        return CP::mean(expr.self());
    }

    template<typename E, typename T, typename SP, typename CP>
    auto max(const TensorExpression<E, T, SP, CP> &expr) {
        return CP::max(expr.self());
    }

    template<typename E, typename T, typename SP, typename CP>
    auto min(const TensorExpression<E, T, SP, CP> &expr) {
        return CP::min(expr.self());
    }

    template<typename E, typename T, typename SP, typename CP>
    auto abs(const TensorExpression<E, T, SP, CP> &expr) {
        return CP::abs(expr.self());
    }

    template<typename E, typename T, typename SP, typename CP>
    auto sqrt(const TensorExpression<E, T, SP, CP> &expr) {
        return CP::sqrt(expr.self());
    }

    template<typename E, typename T, typename SP, typename CP>
    auto exp(const TensorExpression<E, T, SP, CP> &expr) {
        return CP::exp(expr.self());
    }

    template<typename E, typename T, typename SP, typename CP>
    auto log(const TensorExpression<E, T, SP, CP> &expr) {
        return CP::log(expr.self());
    }

    template<typename E1, typename T1, typename S1, typename C1,
             typename E2, typename T2, typename S2, typename C2>
    auto dot(const TensorExpression<E1, T1, S1, C1> &a,
             const TensorExpression<E2, T2, S2, C2> &b) {
        static_assert(std::is_same_v<C1, C2>, "Computation policies must match.");
        return C1::dot(a.self(), b.self());
    }

    template<typename E, typename T, typename SP, typename CP>
    auto variance(const TensorExpression<E, T, SP, CP> &expr) {
        return CP::variance(expr.self());
    }

    template<typename E, typename T, typename SP, typename CP>
    auto std_dev(const TensorExpression<E, T, SP, CP> &expr) {
        return CP::std_dev(expr.self());
    }

    template<typename E, typename T, typename SP, typename CP>
    auto normalize(const TensorExpression<E, T, SP, CP> &expr) {
        return CP::normalize(expr.self());
    }

    template<typename E, typename T, typename SP, typename CP>
    auto reshape(const TensorExpression<E, T, SP, CP> &expr, const TensorShape &new_shape) {
        return CP::reshape(expr.self(), new_shape);
    }

    template<typename E, typename T, typename SP, typename CP>
    auto flatten(const TensorExpression<E, T, SP, CP> &expr) {
        return CP::flatten(expr.self());
    }

    template<typename E, typename T, typename SP, typename CP>
    auto transpose(const TensorExpression<E, T, SP, CP> &expr) {
        return CP::transpose(expr.self());
    }

    template<typename E, typename T, typename SP, typename CP>
    auto sin(const TensorExpression<E, T, SP, CP> &expr) {
        return CP::sin(expr.self());
    }

    template<typename E, typename T, typename SP, typename CP>
    auto cos(const TensorExpression<E, T, SP, CP> &expr) {
        return CP::cos(expr.self());
    }

    template<typename E, typename T, typename SP, typename CP>
    auto tan(const TensorExpression<E, T, SP, CP> &expr) {
        return CP::tan(expr.self());
    }

    template<typename E, typename T, typename SP, typename CP>
    auto square(const TensorExpression<E, T, SP, CP> &expr) {
        return CP::square(expr.self());
    }

    template<typename E1, typename T1, typename SP1, typename CP1,
             typename E2, typename T2, typename SP2, typename CP2>
    auto power(const TensorExpression<E1, T1, SP1, CP1> &a,
               const TensorExpression<E2, T2, SP2, CP2> &b) {
        static_assert(std::is_same_v<CP1, CP2>, "Computation policies must match.");
        return CP1::power(a.self(), b.self());
    }

    template<typename E, typename T, typename SP, typename CP, typename V>
    auto clip(const TensorExpression<E, T, SP, CP> &expr, V min_val, V max_val) {
        return CP::clip(expr.self(), static_cast<T>(min_val), static_cast<T>(max_val));
    }

    template<typename E1, typename T1, typename SP1, typename CP1,
             typename E2, typename T2, typename SP2, typename CP2>
    auto equal(const TensorExpression<E1, T1, SP1, CP1> &a,
               const TensorExpression<E2, T2, SP2, CP2> &b) {
        static_assert(std::is_same_v<CP1, CP2>, "Computation policies must match.");
        return CP1::equal(a.self(), b.self());
    }

    // --- Convenient Ecosystem Aliases ---
    // Small-Buffer-Optimized Dynamic Tensor (64-byte inline capacity)
    template<typename T, size_t InlineBytes = 64, typename CompPolicy = DefaultComputationPolicy>
    using SmallTensor = DynamicTensor<T, SmallTensorStoragePolicy<InlineBytes>, CompPolicy>;

    // Smriti Arena-Backed Dynamic Tensor
    template<typename T, typename ResourceT, typename CompPolicy = DefaultComputationPolicy>
    using SmritiTensor = DynamicTensor<T, SmritiStoragePolicy<ResourceT>, CompPolicy>;

    // --- Standard C++ Lowercase / snake_case Aliases ---
    template<typename T, typename StoragePolicy = DefaultStoragePolicy, typename CompPolicy = DefaultComputationPolicy>
    using dynamic_tensor = DynamicTensor<T, StoragePolicy, CompPolicy>;

    template<typename T, typename StoragePolicy = DefaultStoragePolicy, typename CompPolicy = DefaultComputationPolicy>
    using tensor = DynamicTensor<T, StoragePolicy, CompPolicy>;

    template<typename T, typename StoragePolicy, typename CompPolicy, size_t... Dims>
    using static_tensor = StaticTensor<T, StoragePolicy, CompPolicy, Dims...>;

    template<typename T, size_t InlineBytes = 64, typename CompPolicy = DefaultComputationPolicy>
    using small_tensor = SmallTensor<T, InlineBytes, CompPolicy>;

    template<typename T, typename ResourceT, typename CompPolicy = DefaultComputationPolicy>
    using smriti_tensor = SmritiTensor<T, ResourceT, CompPolicy>;

    using default_storage_policy = DefaultStoragePolicy;
    using default_computation_policy = DefaultComputationPolicy;
    template<size_t InlineBytes = 64>
    using small_tensor_storage_policy = SmallTensorStoragePolicy<InlineBytes>;
    template<typename ResourceT>
    using smriti_storage_policy = SmritiStoragePolicy<ResourceT>;
    template<typename StructT, size_t InlineCap = 64>
    using soa_storage_policy = SoAStoragePolicy<StructT, InlineCap>;
    using arrow_string_storage = ArrowStringStorage;

} // namespace ts

namespace containers::tensor {
    using namespace ts;
}

#endif // PEBBLE_CONTAINERS_TENSOR_HPP
