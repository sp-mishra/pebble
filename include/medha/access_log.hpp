#pragma once
// =============================================================================
// medha/access_log.hpp — optional ordered access log for serial validation
//
// C++23, header-only, no virtual, no macros.
//
// Used in serializable mode (serial-validation variant) to record the
// total order of reads and writes for cross-transaction serialization checks.
// Disabled by default (zero cost when not constructed).
// =============================================================================

#include "medha/key.hpp"
#include "medha/version.hpp"
#include "containers/dynamic/SmallVector.hpp"

#include <cstdint>

namespace medha {
    // ============================================================================
    // access_kind — type of access recorded
    // ============================================================================

    enum class access_kind : std::uint8_t {
        read = 0,
        write = 1,
    };

    // ============================================================================
    // access_record — one entry in the ordered log
    // ============================================================================

    struct access_record {
        canonical_key key{};
        access_kind kind = access_kind::read;
        version_stamp version{};
        std::uint32_t seq = 0; // sequence number within the attempt
    };

    // ============================================================================
    // access_log — SmallVector-backed ordered log (opt-in)
    // ============================================================================

    class access_log {
    public:
        using container_type = containers::dynamic::SmallVector<access_record, 256>;

        void record(canonical_key key, access_kind kind, version_stamp vs) {
            entries_.push_back(access_record{key, kind, vs, seq_++});
        }

        [[nodiscard]] const container_type& entries() const noexcept { return entries_; }
        [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
        [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }

        void clear() noexcept {
            entries_.clear();
            seq_ = 0;
        }

    private:
        container_type entries_;
        std::uint32_t seq_ = 0;
    };
} // namespace medha
