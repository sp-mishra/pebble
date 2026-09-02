#pragma once
#include <string_view>
#include "genome.hpp"

namespace manas {
    template <typename Renderer>
    struct NetworkVisualizer {
        void export_graphviz(const BrainGenome& genome, std::string_view path);
    };
} // namespace manas