#pragma once
// ============================================================================
// kalpana/paint/paint.hpp — Paint Specifications (Fill, Stroke, Gradients, Procedurals)
// ============================================================================
// Supports solid colors, Linear/Radial gradients, Kubelka-Munk SpectralGradients,
// ProceduralFills (paper, marble, wood, noise), and 12 blend modes.
// ============================================================================

#include "../color/color.hpp"
#include "../color/spectral.hpp"
#include "../fill/procedural.hpp"
#include "containers/numeric/math_vector.hpp"
#include <vector>
#include <optional>

namespace kalpana {
    enum class BlendMode : std::uint8_t {
        SrcOver,
        Multiply,
        Screen,
        Overlay,
        Darken,
        Lighten,
        ColorDodge,
        ColorBurn,
        HardLight,
        SoftLight,
        Difference,
        Exclusion
    };

    enum class LineCap : std::uint8_t {
        Butt,
        Round,
        Square
    };

    enum class LineJoin : std::uint8_t {
        Miter,
        Round,
        Bevel
    };

    struct GradientStop {
        float offset = 0.0f; // [0, 1]
        Color color;
        friend constexpr bool operator==(const GradientStop&, const GradientStop&) = default;
    };

    struct LinearGradient {
        pebble::math::vec2 start{0.0f, 0.0f};
        pebble::math::vec2 end{100.0f, 0.0f};
        std::vector<GradientStop> stops;
        friend bool operator==(const LinearGradient&, const LinearGradient&) = default;
    };

    struct RadialGradient {
        pebble::math::vec2 center{0.0f, 0.0f};
        float radius = 50.0f;
        std::vector<GradientStop> stops;
        friend bool operator==(const RadialGradient&, const RadialGradient&) = default;
    };

    struct Stroke {
        Color color = colors::black();
        float width = 1.0f;
        LineCap cap = LineCap::Round;
        LineJoin join = LineJoin::Round;
        float miter_limit = 4.0f;
        friend constexpr bool operator==(const Stroke&, const Stroke&) = default;
    };

    class Paint {
    public:
        Paint() = default;

        static Paint fill(Color c) noexcept {
            Paint p;
            p.has_fill_ = true;
            p.fill_color_ = c;
            return p;
        }

        static Paint fill(spectral::SpectralColor c) noexcept {
            Paint p;
            p.has_fill_ = true;
            p.fill_color_ = c.to_color();
            p.spectral_color_ = c;
            return p;
        }

        static Paint stroke(Color c, float width = 1.0f) noexcept {
            Paint p;
            p.has_stroke_ = true;
            p.stroke_.color = c;
            p.stroke_.width = width;
            return p;
        }

        static Paint filled_outlined(Color fill_c, Color stroke_c, float width = 1.0f) noexcept {
            Paint p;
            p.has_fill_ = true;
            p.fill_color_ = fill_c;
            p.has_stroke_ = true;
            p.stroke_.color = stroke_c;
            p.stroke_.width = width;
            return p;
        }

        static Paint linear_gradient(LinearGradient grad) noexcept {
            Paint p;
            p.has_fill_ = true;
            p.linear_grad_ = std::move(grad);
            return p;
        }

        static Paint radial_gradient(RadialGradient grad) noexcept {
            Paint p;
            p.has_fill_ = true;
            p.radial_grad_ = std::move(grad);
            return p;
        }

        static Paint spectral_gradient(spectral::SpectralGradient grad) noexcept {
            Paint p;
            p.has_fill_ = true;
            p.spectral_grad_ = std::move(grad);
            return p;
        }

        static Paint procedural(ProceduralFill fill_spec) noexcept {
            Paint p;
            p.has_fill_ = true;
            p.procedural_fill_ = std::move(fill_spec);
            return p;
        }

        [[nodiscard]] bool has_fill() const noexcept { return has_fill_; }
        [[nodiscard]] bool has_stroke() const noexcept { return has_stroke_; }
        [[nodiscard]] Color fill_color() const noexcept { return fill_color_; }
        [[nodiscard]] const Stroke& stroke() const noexcept { return stroke_; }
        [[nodiscard]] Stroke& stroke() noexcept { return stroke_; }

        [[nodiscard]] const std::optional<spectral::SpectralColor>& spectral_color() const noexcept {
            return spectral_color_;
        }

        [[nodiscard]] const std::optional<LinearGradient>& linear_grad() const noexcept { return linear_grad_; }
        [[nodiscard]] const std::optional<RadialGradient>& radial_grad() const noexcept { return radial_grad_; }

        [[nodiscard]] const std::optional<spectral::SpectralGradient>& spectral_grad() const noexcept {
            return spectral_grad_;
        }

        [[nodiscard]] const std::optional<ProceduralFill>& procedural_fill() const noexcept { return procedural_fill_; }

        friend bool operator==(const Paint& a, const Paint& b) noexcept {
            return a.has_fill_ == b.has_fill_ &&
                a.fill_color_ == b.fill_color_ &&
                a.has_stroke_ == b.has_stroke_ &&
                a.stroke_ == b.stroke_ &&
                a.linear_grad_ == b.linear_grad_ &&
                a.radial_grad_ == b.radial_grad_ &&
                a.procedural_fill_ == b.procedural_fill_;
        }

    private:
        bool has_fill_ = false;
        Color fill_color_ = colors::black();
        bool has_stroke_ = false;
        Stroke stroke_{};
        std::optional<spectral::SpectralColor> spectral_color_;
        std::optional<LinearGradient> linear_grad_;
        std::optional<RadialGradient> radial_grad_;
        std::optional<spectral::SpectralGradient> spectral_grad_;
        std::optional<ProceduralFill> procedural_fill_;
    };
} // namespace kalpana
