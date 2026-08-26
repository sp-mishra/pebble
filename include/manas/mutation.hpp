#pragma once
#include "genome.hpp"
#include "../../containers/dynamic/SmallVector.hpp"
#include <functional>

namespace manas {

using MutationOperator = std::function<void(BrainGenome&)>;
using MutationOperators = tsa::SmallVector<MutationOperator, 4>;

} // namespace manas