#pragma once
// ============================================================================
// drishya/any_widget.hpp — SBO type-erased widget holder
// ----------------------------------------------------------------------------
// AnyWidget stores any value type satisfying Widget<W, Metrics> and
// PaintableWith<W, Painter> inline in a fixed byte buffer, dispatching through a
// static-constexpr free-function vtable — no virtual, no RTTI, no heap for
// widgets that fit. This mirrors spandana::BasicAction's erasure pattern.
//
// The holder is parameterized on the (Metrics, Painter) pair it erases against,
// because measure() takes a MeasureCtxT<Metrics> and paint() takes a Painter&.
// A project picks one pair (see DefaultMetrics / DefaultPainter) and aliases
// AnyWidget accordingly.
//
// InlineBytes defaults to 512: every stock widget embeds a full LayoutStyle
// (~336B, the SoA-friendly union of all layout inputs) plus its own state, so the
// largest widgets (TextField, Button) land near 480B. 512 holds them with no
// heap; a project using only small custom widgets can instantiate a narrower
// AnyWidgetT<..., 128> to shrink each tree node.
// ============================================================================

#include "drishya/widget_concept.hpp"

#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

namespace pebble::drishya {
    // Callback — move-only type-erased void(EventCtx&)->EventResult is not needed;
    // widgets carry their own handlers. For click handlers etc. we reuse the
    // reactive Callback (see reactive.hpp). AnyWidget only erases the widget itself.

    template <typename Metrics, typename Painter_, std::size_t InlineBytes = 512,
              std::size_t InlineAlign = alignof(std::max_align_t)>
        requires ITextMetrics<Metrics> && Painter<Painter_>
    class AnyWidgetT {
    public:
        using metrics_type = Metrics;
        using painter_type = Painter_;
        using measure_ctx = MeasureCtxT<Metrics>;

        struct Vtable {
            Size2D (*measure)(const void*, const measure_ctx&) noexcept;
            LayoutStyle (*style)(const void*) noexcept;
            EventResult (*on_event)(void*, EventCtx&) noexcept;
            void (*paint)(const void*, Painter_&, Rect2D);
            void (*move_construct)(void*, void*) noexcept;
            void (*destroy)(void*) noexcept;
        };

        AnyWidgetT() = default;

        template <typename W, typename Decayed = std::remove_cvref_t<W>>
            requires(!std::is_same_v<Decayed, AnyWidgetT> &&
                Widget<Decayed, Metrics> && PaintableWith<Decayed, Painter_>)
        AnyWidgetT(W&& w) noexcept {
            static_assert(sizeof(Decayed) <= InlineBytes,
                          "drishya widget exceeds AnyWidget inline buffer; raise AnyWidgetT<InlineBytes>.");
            static_assert(alignof(Decayed) <= InlineAlign,
                          "drishya widget over-aligned for AnyWidget inline buffer.");
            static_assert(std::is_nothrow_move_constructible_v<Decayed>,
                          "drishya widget must be nothrow move-constructible for SBO storage.");
            ::new(static_cast<void*>(&storage_)) Decayed(std::forward<W>(w));
            vtable_ = vtable_for<Decayed>();
        }

        AnyWidgetT(AnyWidgetT&& other) noexcept {
            if (other.vtable_) {
                other.vtable_->move_construct(&storage_, &other.storage_);
                vtable_ = other.vtable_;
                other.reset();
            }
        }

        AnyWidgetT& operator=(AnyWidgetT&& other) noexcept {
            if (this != &other) {
                reset();
                if (other.vtable_) {
                    other.vtable_->move_construct(&storage_, &other.storage_);
                    vtable_ = other.vtable_;
                    other.reset();
                }
            }
            return *this;
        }

        AnyWidgetT(const AnyWidgetT&) = delete;
        AnyWidgetT& operator=(const AnyWidgetT&) = delete;

        ~AnyWidgetT() { reset(); }

        [[nodiscard]] bool valid() const noexcept { return vtable_ != nullptr; }
        explicit operator bool() const noexcept { return valid(); }

        [[nodiscard]] Size2D measure(const measure_ctx& mc) const noexcept {
            return vtable_ ? vtable_->measure(&storage_, mc) : Size2D{};
        }

        [[nodiscard]] LayoutStyle style() const noexcept {
            return vtable_ ? vtable_->style(&storage_) : LayoutStyle{};
        }

        EventResult on_event(EventCtx& ec) noexcept {
            return vtable_ ? vtable_->on_event(&storage_, ec) : EventResult::Ignored;
        }

        void paint(Painter_& painter, Rect2D box) const {
            if (vtable_) vtable_->paint(&storage_, painter, box);
        }

    private:
        void reset() noexcept {
            if (vtable_) {
                vtable_->destroy(&storage_);
                vtable_ = nullptr;
            }
        }

        template <typename W>
        static const Vtable* vtable_for() noexcept {
            static constexpr Vtable vt{
                .measure = [](const void* p, const measure_ctx& mc) noexcept -> Size2D {
                    return static_cast<const W*>(p)->measure(mc);
                },
                .style = [](const void* p) noexcept -> LayoutStyle {
                    return static_cast<const W*>(p)->style();
                },
                .on_event = [](void* p, EventCtx& ec) noexcept -> EventResult {
                    return static_cast<W*>(p)->on_event(ec);
                },
                .paint = [](const void* p, Painter_& painter, Rect2D box) {
                    static_cast<const W*>(p)->paint(painter, box);
                },
                .move_construct = [](void* dst, void* src) noexcept {
                    ::new(dst) W(std::move(*static_cast<W*>(src)));
                },
                .destroy = [](void* p) noexcept { static_cast<W*>(p)->~W(); },
            };
            return &vt;
        }

        alignas(InlineAlign) std::byte storage_[InlineBytes]{};
        const Vtable* vtable_ = nullptr;
    };
} // namespace pebble::drishya
