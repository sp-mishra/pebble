#pragma once

#include <cstdint>

namespace pebble::manas {

using BrainId = std::uint64_t;  // 64-bit stable identifier
using NeuronIndex = uint32_t;
using ConnectionIndex = uint32_t;

// Brain cognition package with topology, weights, biases, etc.
struct BrainGenome {
    bool is_inheritable() const noexcept;
    bool is_mutable() const noexcept;
    void reset_cache() noexcept;
    
    friend bool operator==(const BrainGenome& a, const BrainGenome& b) noexcept;
    friend bool operator!=(const BrainGenome& a, const BrainGenome& b) noexcept;
    
    // Genome data...
};

} // namespace pebble::manas