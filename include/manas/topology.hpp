#pragma once

namespace manas {
    enum class TopologyType {
        Reactive, // Input → Output
        FeedForward, // Input → Hidden → Output
        Recurrent, // Hidden ↻ connections
        Evolvable // NEAT-inspired
    };

    // Common topology interface
    template <TopologyType T>
    struct Topology {
        static constexpr auto type = T;

        // Type-specific implementation
        // ...
    };
} // namespace manas