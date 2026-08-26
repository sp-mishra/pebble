#pragma once
#include <vector>
#include <span>
#include <cstdint>
#include "genome.hpp"

namespace manas {

template<typename EncodingPolicy>
struct GenomeSerializer {
    std::vector<uint8_t> serialize(const BrainGenome& genome);
    BrainGenome deserialize(std::span<const uint8_t> data);
};

} // namespace manas