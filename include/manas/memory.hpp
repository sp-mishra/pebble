#pragma once
#include "brain.hpp"

namespace manas {
    template <typename Impl>
    struct NeuralMemory { // CRTP pattern
        void store(const NeuronIndex& neuron, float value) {
            static_cast<Impl*>(this)->store_impl(neuron, value);
        }

        float retrieve(const NeuronIndex& neuron) const {
            return static_cast<const Impl*>(this)->retrieve_impl(neuron);
        }

        void tick() {
            static_cast<Impl*>(this)->tick_impl();
        }
    };
} // namespace manas