#pragma once
// ============================================================================
// gati/input.hpp — Device-Agnostic Input Snapshot & Action Mapping
// ============================================================================
// Captures button bitsets and analog axes for deterministic simulation input.
// ============================================================================

#include "math.hpp"
#include "containers/static/static_vector.hpp"

#include <cstdint>
#include <string_view>

namespace gati {
    inline constexpr std::size_t kMaxButtons = 64;
    inline constexpr std::size_t kMaxAxes = 8;
    inline constexpr std::size_t kMaxActions = 32;

    struct InputState {
        std::uint64_t buttons = 0; // bit b set => button b down
        Scalar axes[kMaxAxes] = {};

        [[nodiscard]] bool down(std::uint32_t b) const noexcept {
            return (buttons >> b) & 1u;
        }

        void set(std::uint32_t b, bool v) noexcept {
            buttons = v
                          ? (buttons | (std::uint64_t{1} << b))
                          : (buttons & ~(std::uint64_t{1} << b));
        }

        [[nodiscard]] Scalar axis(std::uint32_t a) const noexcept {
            return a < kMaxAxes ? axes[a] : Scalar(0);
        }

        void set_axis(std::uint32_t a, Scalar v) noexcept {
            if (a < kMaxAxes) axes[a] = v;
        }
    };

    struct ActionMap {
        struct Binding {
            std::string_view name;
            std::uint32_t button;
        };

        containers::static_vector<Binding, kMaxActions> bindings;

        void bind(std::string_view name, std::uint32_t button) {
            (void)bindings.push_back({name, button});
        }

        [[nodiscard]] bool active(std::string_view name, const InputState& s) const noexcept {
            for (const auto& b : bindings) {
                if (b.name == name) return s.down(b.button);
            }
            return false;
        }

        [[nodiscard]] bool pressed(std::string_view name, const InputState& cur,
                                   const InputState& prev) const noexcept {
            for (const auto& b : bindings) {
                if (b.name == name) return cur.down(b.button) && !prev.down(b.button);
            }
            return false;
        }
    };
} // namespace gati
