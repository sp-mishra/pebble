#pragma once
#include "genome.hpp"
#include <functional>

namespace manas {

using CrossoverOperator = std::function<BrainGenome(const BrainGenome&, const BrainGenome&)>;

} // namespace manas