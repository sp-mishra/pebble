#pragma once
// ============================================================================
// SmallBitSet<InlineBits, Alloc>
// ============================================================================
// A small-buffer-optimised bitset container for C++23/26.
//
// Features:
// - Inline storage for the first InlineBits bits (default: 64, exactly one uint64_t).
// - Dynamic spill into Alloc when size exceeds InlineBits.
// - Compatible with Smriti and PMR allocators.
// - Fast branchless bitwise operations (union, intersect, difference, xor, not).
// - Fast population count (std::popcount), find_first, find_next.
// - Zero virtual dispatch, zero macros, zero RTTI.
// ============================================================================

#include <algorithm>
#include <bit>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <new>
#include <string>
#include <type_traits>
#include <utility>

namespace containers::dynamic {

    template <std::size_t InlineBits = 64, typename Alloc = std::allocator<std::uint64_t>>
    class SmallBitSet {
    public:
        using word_type = std::uint64_t;
        using allocator_type = Alloc;
        using size_type = std::size_t;

        static constexpr size_type kBitsPerWord = 64;
        static constexpr size_type kInlineWords = (InlineBits + kBitsPerWord - 1) / kBitsPerWord;
        static constexpr size_type kActualInlineWords = kInlineWords > 0 ? kInlineWords : 1;

    private:
        using AllocTraits = std::allocator_traits<Alloc>;

        static constexpr size_type words_for_bits(size_type bits) noexcept {
            return (bits + kBitsPerWord - 1) / kBitsPerWord;
        }

        size_type num_bits_{0};
        size_type num_words_{0};
        size_type capacity_words_{kActualInlineWords};
        word_type* words_{nullptr};
        [[no_unique_address]] allocator_type alloc_{};
        word_type inline_storage_[kActualInlineWords]{};

        [[nodiscard]] constexpr bool is_inline() const noexcept {
            return words_ == inline_storage_;
        }

        constexpr void sanitize_unused_bits() noexcept {
            if (num_bits_ == 0 || num_words_ == 0) return;
            const size_type rem = num_bits_ % kBitsPerWord;
            if (rem != 0) {
                const word_type mask = (word_type{1} << rem) - 1;
                words_[num_words_ - 1] &= mask;
            }
        }

        void allocate_words(size_type req_words) {
            if (req_words <= kActualInlineWords) {
                words_ = inline_storage_;
                capacity_words_ = kActualInlineWords;
            } else {
                words_ = AllocTraits::allocate(alloc_, req_words);
                capacity_words_ = req_words;
            }
        }

        void deallocate_words() noexcept {
            if (!is_inline() && words_ != nullptr) {
                AllocTraits::deallocate(alloc_, words_, capacity_words_);
                words_ = inline_storage_;
                capacity_words_ = kActualInlineWords;
            }
        }

    public:
        // ---- Reference Proxy ----
        class reference {
            friend class SmallBitSet;
            word_type* word_;
            size_type bit_idx_;

            constexpr reference(word_type* w, size_type bit) noexcept
                : word_(w), bit_idx_(bit) {}

        public:
            constexpr reference& operator=(bool val) noexcept {
                const word_type mask = word_type{1} << bit_idx_;
                if (val) {
                    *word_ |= mask;
                } else {
                    *word_ &= ~mask;
                }
                return *this;
            }

            constexpr reference& operator=(const reference& other) noexcept {
                return *this = static_cast<bool>(other);
            }

            [[nodiscard]] constexpr operator bool() const noexcept {
                return (*word_ & (word_type{1} << bit_idx_)) != 0;
            }

            [[nodiscard]] constexpr bool operator~() const noexcept {
                return !static_cast<bool>(*this);
            }

            constexpr reference& flip() noexcept {
                *word_ ^= (word_type{1} << bit_idx_);
                return *this;
            }
        };

        // ---- Constructors & Destructor ----
        constexpr SmallBitSet() noexcept : words_(inline_storage_) {
            std::fill_n(inline_storage_, kActualInlineWords, word_type{0});
        }

        explicit constexpr SmallBitSet(const Alloc& alloc) noexcept
            : words_(inline_storage_), alloc_(alloc) {
            std::fill_n(inline_storage_, kActualInlineWords, word_type{0});
        }

        explicit SmallBitSet(size_type num_bits, bool init_val = false, const Alloc& alloc = Alloc())
            : alloc_(alloc) {
            num_bits_ = num_bits;
            num_words_ = words_for_bits(num_bits);
            allocate_words(std::max(num_words_, kActualInlineWords));
            const word_type fill_pattern = init_val ? ~word_type{0} : word_type{0};
            std::fill_n(words_, num_words_, fill_pattern);
            sanitize_unused_bits();
        }

        SmallBitSet(std::initializer_list<bool> init, const Alloc& alloc = Alloc())
            : SmallBitSet(init.size(), false, alloc) {
            size_type idx = 0;
            for (bool b : init) {
                if (b) set(idx);
                ++idx;
            }
        }

        SmallBitSet(const SmallBitSet& other)
            : num_bits_(other.num_bits_),
              num_words_(other.num_words_),
              alloc_(AllocTraits::select_on_container_copy_construction(other.alloc_)) {
            allocate_words(std::max(num_words_, kActualInlineWords));
            std::copy_n(other.words_, num_words_, words_);
        }

        SmallBitSet(SmallBitSet&& other) noexcept
            : num_bits_(other.num_bits_),
              num_words_(other.num_words_),
              alloc_(std::move(other.alloc_)) {
            if (other.is_inline()) {
                words_ = inline_storage_;
                capacity_words_ = kActualInlineWords;
                std::copy_n(other.inline_storage_, kActualInlineWords, inline_storage_);
            } else {
                words_ = other.words_;
                capacity_words_ = other.capacity_words_;
                other.words_ = other.inline_storage_;
                other.capacity_words_ = kActualInlineWords;
            }
            other.num_bits_ = 0;
            other.num_words_ = 0;
            std::fill_n(other.inline_storage_, kActualInlineWords, word_type{0});
        }

        SmallBitSet& operator=(const SmallBitSet& other) {
            if (this == &other) return *this;
            if constexpr (AllocTraits::propagate_on_container_copy_assignment::value) {
                if (alloc_ != other.alloc_) {
                    deallocate_words();
                    alloc_ = other.alloc_;
                }
            }
            resize(other.num_bits_);
            std::copy_n(other.words_, other.num_words_, words_);
            return *this;
        }

        SmallBitSet& operator=(SmallBitSet&& other) noexcept {
            if (this == &other) return *this;
            deallocate_words();
            alloc_ = std::move(other.alloc_);
            num_bits_ = other.num_bits_;
            num_words_ = other.num_words_;
            if (other.is_inline()) {
                words_ = inline_storage_;
                capacity_words_ = kActualInlineWords;
                std::copy_n(other.inline_storage_, kActualInlineWords, inline_storage_);
            } else {
                words_ = other.words_;
                capacity_words_ = other.capacity_words_;
                other.words_ = other.inline_storage_;
                other.capacity_words_ = kActualInlineWords;
            }
            other.num_bits_ = 0;
            other.num_words_ = 0;
            std::fill_n(other.inline_storage_, kActualInlineWords, word_type{0});
            return *this;
        }

        ~SmallBitSet() {
            deallocate_words();
        }

        // ---- Capacity & Size ----
        [[nodiscard]] constexpr size_type size() const noexcept { return num_bits_; }
        [[nodiscard]] constexpr size_type num_words() const noexcept { return num_words_; }
        [[nodiscard]] constexpr bool empty() const noexcept { return num_bits_ == 0; }
        [[nodiscard]] constexpr size_type capacity() const noexcept { return capacity_words_ * kBitsPerWord; }
        [[nodiscard]] constexpr allocator_type get_allocator() const noexcept { return alloc_; }

        void resize(size_type new_bits, bool val = false) {
            const size_type new_words = words_for_bits(new_bits);
            if (new_words > capacity_words_) {
                size_type new_cap = std::max(capacity_words_ * 2, new_words);
                word_type* new_storage = AllocTraits::allocate(alloc_, new_cap);
                std::copy_n(words_, num_words_, new_storage);
                std::fill(new_storage + num_words_, new_storage + new_cap, word_type{0});
                deallocate_words();
                words_ = new_storage;
                capacity_words_ = new_cap;
            }

            if (new_bits > num_bits_) {
                if (val) {
                    // Set all new bits from num_bits_ to new_bits - 1
                    for (size_type i = num_bits_; i < new_bits; ++i) {
                        const size_type w = i / kBitsPerWord;
                        const size_type b = i % kBitsPerWord;
                        words_[w] |= (word_type{1} << b);
                    }
                } else {
                    // Zero out extra words if any
                    if (new_words > num_words_) {
                        std::fill(words_ + num_words_, words_ + new_words, word_type{0});
                    }
                }
            }
            num_bits_ = new_bits;
            num_words_ = new_words;
            sanitize_unused_bits();
        }

        void reserve(size_type bit_capacity) {
            const size_type req_words = words_for_bits(bit_capacity);
            if (req_words > capacity_words_) {
                word_type* new_storage = AllocTraits::allocate(alloc_, req_words);
                std::copy_n(words_, num_words_, new_storage);
                std::fill(new_storage + num_words_, new_storage + req_words, word_type{0});
                deallocate_words();
                words_ = new_storage;
                capacity_words_ = req_words;
            }
        }

        void clear() noexcept {
            num_bits_ = 0;
            num_words_ = 0;
            if (words_) {
                std::fill_n(words_, capacity_words_, word_type{0});
            }
        }

        // ---- Element Access ----
        [[nodiscard]] constexpr bool test(size_type pos) const noexcept {
            assert(pos < num_bits_);
            const size_type w = pos / kBitsPerWord;
            const size_type b = pos % kBitsPerWord;
            return (words_[w] & (word_type{1} << b)) != 0;
        }

        [[nodiscard]] constexpr bool operator[](size_type pos) const noexcept {
            return test(pos);
        }

        [[nodiscard]] constexpr reference operator[](size_type pos) noexcept {
            assert(pos < num_bits_);
            return reference(&words_[pos / kBitsPerWord], pos % kBitsPerWord);
        }

        // ---- Modifiers ----
        SmallBitSet& set() noexcept {
            std::fill_n(words_, num_words_, ~word_type{0});
            sanitize_unused_bits();
            return *this;
        }

        SmallBitSet& set(size_type pos, bool val = true) noexcept {
            assert(pos < num_bits_);
            const size_type w = pos / kBitsPerWord;
            const size_type b = pos % kBitsPerWord;
            const word_type mask = word_type{1} << b;
            if (val) words_[w] |= mask;
            else words_[w] &= ~mask;
            return *this;
        }

        SmallBitSet& reset() noexcept {
            std::fill_n(words_, num_words_, word_type{0});
            return *this;
        }

        SmallBitSet& reset(size_type pos) noexcept {
            return set(pos, false);
        }

        SmallBitSet& flip() noexcept {
            for (size_type i = 0; i < num_words_; ++i) {
                words_[i] = ~words_[i];
            }
            sanitize_unused_bits();
            return *this;
        }

        SmallBitSet& flip(size_type pos) noexcept {
            assert(pos < num_bits_);
            words_[pos / kBitsPerWord] ^= (word_type{1} << (pos % kBitsPerWord));
            return *this;
        }

        void push_back(bool val) {
            const size_type pos = num_bits_;
            resize(num_bits_ + 1);
            set(pos, val);
        }

        // ---- Bitwise Reductions & Queries ----
        [[nodiscard]] bool all() const noexcept {
            if (num_bits_ == 0) return true;
            for (size_type i = 0; i + 1 < num_words_; ++i) {
                if (words_[i] != ~word_type{0}) return false;
            }
            const size_type rem = num_bits_ % kBitsPerWord;
            const word_type expected_last = rem == 0 ? ~word_type{0} : ((word_type{1} << rem) - 1);
            return words_[num_words_ - 1] == expected_last;
        }

        [[nodiscard]] bool any() const noexcept {
            for (size_type i = 0; i < num_words_; ++i) {
                if (words_[i] != 0) return true;
            }
            return false;
        }

        [[nodiscard]] bool none() const noexcept {
            return !any();
        }

        [[nodiscard]] size_type count() const noexcept {
            size_type cnt = 0;
            for (size_type i = 0; i < num_words_; ++i) {
                cnt += static_cast<size_type>(std::popcount(words_[i]));
            }
            return cnt;
        }

        // ---- Bit Scanning ----
        [[nodiscard]] size_type find_first() const noexcept {
            return find_next(static_cast<size_type>(-1));
        }

        [[nodiscard]] size_type find_next(size_type prev) const noexcept {
            size_type next_bit = prev + 1;
            if (next_bit >= num_bits_) return num_bits_;

            size_type w = next_bit / kBitsPerWord;
            size_type b = next_bit % kBitsPerWord;

            word_type mask = words_[w] & (~word_type{0} << b);
            if (mask != 0) {
                size_type res = w * kBitsPerWord + static_cast<size_type>(std::countr_zero(mask));
                return std::min(res, num_bits_);
            }

            for (++w; w < num_words_; ++w) {
                if (words_[w] != 0) {
                    size_type res = w * kBitsPerWord + static_cast<size_type>(std::countr_zero(words_[w]));
                    return std::min(res, num_bits_);
                }
            }
            return num_bits_;
        }

        // ---- Bitwise Operations ----
        SmallBitSet& operator&=(const SmallBitSet& other) noexcept {
            const size_type common = std::min(num_words_, other.num_words_);
            for (size_type i = 0; i < common; ++i) {
                words_[i] &= other.words_[i];
            }
            for (size_type i = common; i < num_words_; ++i) {
                words_[i] = 0;
            }
            return *this;
        }

        SmallBitSet& operator|=(const SmallBitSet& other) {
            if (other.num_bits_ > num_bits_) resize(other.num_bits_);
            const size_type common = std::min(num_words_, other.num_words_);
            for (size_type i = 0; i < common; ++i) {
                words_[i] |= other.words_[i];
            }
            return *this;
        }

        SmallBitSet& operator^=(const SmallBitSet& other) {
            if (other.num_bits_ > num_bits_) resize(other.num_bits_);
            const size_type common = std::min(num_words_, other.num_words_);
            for (size_type i = 0; i < common; ++i) {
                words_[i] ^= other.words_[i];
            }
            sanitize_unused_bits();
            return *this;
        }

        [[nodiscard]] SmallBitSet operator~() const {
            SmallBitSet copy(*this);
            copy.flip();
            return copy;
        }

        [[nodiscard]] friend SmallBitSet operator&(SmallBitSet lhs, const SmallBitSet& rhs) {
            lhs &= rhs;
            return lhs;
        }

        [[nodiscard]] friend SmallBitSet operator|(SmallBitSet lhs, const SmallBitSet& rhs) {
            lhs |= rhs;
            return lhs;
        }

        [[nodiscard]] friend SmallBitSet operator^(SmallBitSet lhs, const SmallBitSet& rhs) {
            lhs ^= rhs;
            return lhs;
        }

        [[nodiscard]] bool operator==(const SmallBitSet& other) const noexcept {
            if (num_bits_ != other.num_bits_) return false;
            for (size_type i = 0; i < num_words_; ++i) {
                if (words_[i] != other.words_[i]) return false;
            }
            return true;
        }

        [[nodiscard]] word_type* data() noexcept { return words_; }
        [[nodiscard]] const word_type* data() const noexcept { return words_; }
    };

} // namespace containers::dynamic
