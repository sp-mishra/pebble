#pragma once
#include <cstdint>

namespace manas {

struct BrainId {
    uint64_t value;
    constexpr bool operator==(const BrainId& other) const noexcept { return value == other.value; }
    constexpr bool operator!=(const BrainId& other) const noexcept { return value != other.value; }
};

struct NeuronIndex {
    uint32_t value;
    constexpr bool operator==(const NeuronIndex& other) const noexcept { return value == other.value; }
    constexpr bool operator!=(const NeuronIndex& other) const noexcept { return value != other.value; }
};

struct ConnectionIndex {
    uint32_t value;
    constexpr bool operator==(const ConnectionIndex& other) const noexcept { return value == other.value; }
    constexpr bool operator!=(const ConnectionIndex& other) const noexcept { return value != other.value; }
};

} // namespace manas