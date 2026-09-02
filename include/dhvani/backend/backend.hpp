#pragma once
// dhvani/backend/backend.hpp — AudioBackend concept: sample_rate, start/stop, is_running.

#include "../synth/buffer.hpp"
#include <concepts>
#include <cstdint>
#include <functional>
#include <span>

namespace pebble::dhvani::backend {
    template <typename T>
    concept AudioBackend = requires(T& b,
                                    std::function<void(std::span<synth::SampleFrame>)> callback) {
        { b.sample_rate() } -> std::convertible_to<uint32_t>;
        { b.start(callback) } -> std::same_as<bool>;
        { b.stop() } -> std::same_as<void>;
        { b.is_running() } -> std::convertible_to<bool>;
    };
} // namespace pebble::dhvani::backend
