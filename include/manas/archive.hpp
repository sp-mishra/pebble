#pragma once
#include "brain.hpp"
#include "genome.hpp"

namespace manas {

template<typename MemoryBackend>
class GenomeArchive {
public:
    void save(const BrainGenome& genome);
    BrainGenome load(BrainId id);
};

} // namespace manas